/* conlog_core.c — буфер строк вывода команды и разбор уровней.
 *
 * Главная сложность здесь — НЕ в GTK, а в границах порций вывода.
 * Команда, запущенная через канал, отдаёт данные кусками по 4-64 КБ,
 * и границы этих кусков не совпадают с границами строк. Наивное
 * «пришёл кусок — разбили по \n» даёт на экране обрывки слов и
 * теряет символы на стыке порций. Здесь держится недописанный хвост
 * и дописывается к следующей порции.
 */
#include "conlog_core.h"

#include <string.h>
#include <pango/pango.h>
#include <pango/pangocairo.h>
#include <cairo.h>

struct _ConLogBuffer {
    GPtrArray   *lines;      /* ConLogLine*, с free_func на элемент */
    guint        max_lines;
    guint64      total_seen; /* сколько строк пришло за всю жизнь */
    char        *pending;    /* недописанный хвост последней порции */
    size_t       pending_len;
    gboolean     strip_ansi; /* чистить ли собранные строки от escape */
};

/* ------------------------------------------------- разбор уровней */

/* Слово найдено как отдельное: перед ним либо начало строки, либо
 * не-буква и не-цифра. Именно это отличает «error» от «terror» и
 * «err» от «terrain».
 *
 * Регистронезависимо: dmesg пишет «EXT4-fs error», journalctl — «INFO»,
 * а команда вроде systemctl может вывести «Warning» с большой буквы.
 * Поиск с учётом регистра оставлял бы такие строки неокрашенными —
 * ровно те, ради которых подсветка и нужна. */
static gboolean word_present_ci(const char *hay_lower, const char *word)
{
    size_t wlen = strlen(word);
    const char *p = hay_lower;

    if (!hay_lower || !wlen)
        return FALSE;
    while ((p = strstr(p, word)) != NULL) {
        gboolean left_ok = (p == hay_lower)
                        || !(g_ascii_isalnum(*(p - 1)) || *(p - 1) == '_');
        gboolean right_ok = !g_ascii_isalnum(p[wlen]) && p[wlen] != '_';

        if (left_ok && right_ok)
            return TRUE;
        p += wlen;
    }
    return FALSE;
}

ConLogLevel conlog_classify(const char *text)
{
    char *lower;
    ConLogLevel level = CONLOG_LEVEL_NORMAL;

    if (!text || !*text)
        return CONLOG_LEVEL_NORMAL;
    lower = g_ascii_strdown(text, -1);

    /* Порядок значим: составные слова проверяются раньше коротких.
     * «critical» и «fatal» — это ошибки, а не отдельный уровень:
     * красить их иначе пользователя смутило бы.
     *
     * Окончания перечислены ЯВНО, а не отрезаются: матчер требует
     * границу слова, и «fail» не находит «failed» — а именно эту
     * форму пишет systemd («FAILED to start …»). Понятная ложь
     * хуже молчаливого провала: строка с ошибкой осталась бы серой. */
    if (word_present_ci(lower, "panic") || word_present_ci(lower, "fatal")
     || word_present_ci(lower, "critical") || word_present_ci(lower, "crit")
     || word_present_ci(lower, "error") || word_present_ci(lower, "err")
     || word_present_ci(lower, "fail") || word_present_ci(lower, "failed")
     || word_present_ci(lower, "failure")
     || word_present_ci(lower, "denied") || word_present_ci(lower, "deny")
     || word_present_ci(lower, "refused") || word_present_ci(lower, "refuse"))
        level = CONLOG_LEVEL_ERROR;
    else if (word_present_ci(lower, "warn") || word_present_ci(lower, "warning")
          || word_present_ci(lower, "deprecated")
          || word_present_ci(lower, "warned"))
        level = CONLOG_LEVEL_WARN;
    else if (word_present_ci(lower, "debug") || word_present_ci(lower, "trace")
             || word_present_ci(lower, "debugging"))
        level = CONLOG_LEVEL_DEBUG;
    else if (word_present_ci(lower, "info") || word_present_ci(lower, "notice")
          || word_present_ci(lower, "started") || word_present_ci(lower, "connected")
          || word_present_ci(lower, "loaded"))
        level = CONLOG_LEVEL_INFO;

    g_free(lower);
    return level;
}

void conlog_level_color(ConLogLevel lvl, gdouble rgba[4])
{
    if (!rgba)
        return;
    /* Мягкие цвета на тёмном фоне: чистый красный и зелёный на чёрном
     * «слепят» и выглядят как ошибка отрисовки, а не как смысл строки. */
    switch (lvl) {
    case CONLOG_LEVEL_ERROR:
        rgba[0] = 1.00; rgba[1] = 0.42; rgba[2] = 0.42; rgba[3] = 1.0;
        break;
    case CONLOG_LEVEL_WARN:
        rgba[0] = 1.00; rgba[1] = 0.80; rgba[2] = 0.30; rgba[3] = 1.0;
        break;
    case CONLOG_LEVEL_INFO:
        rgba[0] = 0.55; rgba[1] = 0.85; rgba[2] = 1.00; rgba[3] = 1.0;
        break;
    case CONLOG_LEVEL_DEBUG:
        rgba[0] = 0.62; rgba[1] = 0.62; rgba[2] = 0.70; rgba[3] = 1.0;
        break;
    case CONLOG_LEVEL_NORMAL:
    default:
        rgba[0] = 0.85; rgba[1] = 0.85; rgba[2] = 0.85; rgba[3] = 1.0;
        break;
    }
}

/* Вырезать escape-последовательности и управляющие символы.
 *
 * Работает по ЯВНОЙ длине, а не по strlen: вход приходит из
 * g_io_channel_read_chars() и не обязан быть NUL-терминирован. Раньше
 * сканер полагался на strlen() и, выйдя за пределы порции, терял
 * переводы строк — на живом journalctl это копило «хвост» в мегабайты
 * и оставляло окно пустым. */
char *conlog_strip_ansi(const char *text, gssize len)
{
    GString *out;
    const char *p = text;
    const char *end;

    if (!text)
        return NULL;
    if (len < 0)
        len = (gssize) strlen(text);
    end = text + len;

    out = g_string_new(NULL);
    while (p < end) {
        if (*p == '\033') {
            /* CSI: ESC [ ... конечная буква в диапазоне @..~ */
            if (p + 1 < end && p[1] == '[') {
                const char *q = p + 2;

                while (q < end && !((guchar) *q >= '@' && (guchar) *q <= '~'))
                    q++;
                /* q == end: последовательность недописана, но это
                 * не повод выйти из функции — хвост доберётся
                 * следующей порцией, как и обычная строка. */
                p = (q < end) ? q + 1 : q;
                continue;
            }
            /* Прочие двухбайтовые последовательности. Хвост из одного
             * ESC на границе порции просто отбрасываем. */
            p += 2;
            if (p > end)
                p = end;
            continue;
        }
        /* Пропускаем управляющие символы, но сохраняем перевод строки,
         * табуляцию, возврат каретки и ВСЮ не-ASCII графику.
         *
         * Сравнение — через guchar: char на этой платформе знаковый, и
         * байты UTF-8 (0xC0..0xFF для кириллицы) дают отрицательные
         * значения. Условие «*p >= 0x20» на signed char выбрасывало
         * весь русский текст, а с ним и перевод строки (0x0A).
         * Ошибка выглядела безобидно: на живом журнале, где почти нет
         * управляющих символов, окно выглядело почти правильно. */
        {
            guchar uc = (guchar) *p;

            if (uc == '\n' || uc == '\r' || uc == '\t' || uc >= 0x20)
                g_string_append_c(out, (char) uc);
        }
        p++;
    }
    if (out->len == 0) {
        g_string_free(out, TRUE);
        return g_strdup("");
    }
    return g_string_free(out, FALSE);
}


/* Какие строки попадут в окно при заданной геометрии.
 *
 * Существует из-за дефекта отрисовки: условие «строка целиком выше
 * окна» проверялось как «y - step >= first_row_y», что истинно уже
 * для первой строки (6 >= 6 при first_row_y == 6). Тогда shown всегда
 * оставался 0, и на экране появлялась подсказка «ожидание вывода…»
 * при полном буфере — апплет выглядел сломанным без единой ошибки. */
ConLogView conlog_visible_lines(guint total, int first_row_y, int step,
                                int height, int scroll_top)
{
    ConLogView v = { 0, 0 };
    double y;
    guint i;

    if (total == 0 || step <= 0 || height <= 0)
        return v;
    if (scroll_top < 0)
        scroll_top = 0;
    if ((guint) scroll_top >= total)
        scroll_top = (int) total - 1;
    v.first = (guint) scroll_top;
    /* Базовая линия ВСЕГДА у первой видимой строки, независимо от
     * scroll_top. Раньше она считалась как first_row_y + step*(first+1),
     * то есть прокрутка сдвигала и саму строку за нижнюю границу окна:
     * при scroll_top == 199 в 200-пиксельном окне не оставалось НИ
     * ОДНОЙ строки, и прокрутка выглядела как «всё пропало». */
    y = first_row_y + (double) step;
    for (i = v.first; i < total; i++) {
        if (y > height + step)
            break;
        v.count++;
        y += step;
    }
    return v;
}

/* --------------------------------------------------- буфер строк */

static void conlog_line_free(gpointer p)
{
    ConLogLine *l = p;

    if (!l)
        return;
    g_free(l->text);
    g_free(l);
}

ConLogBuffer *conlog_buffer_new(guint max_lines)
{
    ConLogBuffer *b = g_new0(ConLogBuffer, 1);

    b->lines = g_ptr_array_new_with_free_func(conlog_line_free);
    /* Снизу не зажимаем: MIN_LINES — это ограничение ЭЛЕМЕНТА
     * управления в Настройках, а не ядра. Ядро обязано честно держать
     * и 3 строки, иначе тест на малых объёмах врал бы. */
    b->max_lines = max_lines ? MIN(max_lines, CONLOG_MAX_LINES)
                             : CONLOG_DEFAULT_LINES;
    return b;
}

void conlog_buffer_free(ConLogBuffer *b)
{
    if (!b)
        return;
    g_ptr_array_free(b->lines, TRUE);
    g_free(b->pending);
    g_free(b);
}

void conlog_set_max_lines(ConLogBuffer *b, guint max_lines)
{
    guint want;

    if (!b)
        return;
    want = max_lines ? MIN(max_lines, CONLOG_MAX_LINES)
                     : CONLOG_DEFAULT_LINES;
    b->max_lines = want;
    /* Уменьшили лимит — лишние строки уходят сразу, а не при следующей
     * записи: иначе будан мгновенно превышал бы только что заданный
     * пользователем предел. */
    while (b->lines->len > b->max_lines)
        g_ptr_array_remove_index(b->lines, 0);
}

guint   conlog_max_lines(const ConLogBuffer *b)   { return b ? b->max_lines : 0; }
guint   conlog_len(const ConLogBuffer *b)        { return b ? b->lines->len : 0; }
guint64 conlog_total_seen(const ConLogBuffer *b) { return b ? b->total_seen : 0; }

void conlog_append(ConLogBuffer *b, const char *text, ConLogLevel level)
{
    ConLogLine *line;

    if (!b)
        return;
    line = g_new0(ConLogLine, 1);
    /* Копия, а не указатель на буфер процесса: тот освобождается сразу
     * после чтения, а строка должна пережить прокрутку назад.
     *
     * Чистка escape-последовательностей — ЗДЕСЬ, на собранной строке,
     * а не на сырой порции в conlog_drain(). На порции последовательность
     * может быть разорвана границей чтения («ESC[» | «31m»), и тогда
     * на экране остался бы мусор «31m». */
    if (text && b->strip_ansi) {
        char *clean = conlog_strip_ansi(text, -1);

        line->text = clean;
    } else {
        line->text = g_strdup(text ? text : "");
    }
    line->level = level;
    line->seq = b->total_seen++;
    g_ptr_array_add(b->lines, line);
    while (b->lines->len > b->max_lines)
        g_ptr_array_remove_index(b->lines, 0);
}

/* Строка может прийти пустой — это обычное дело для команд, печатающих
 * перевод строки в начале строки. Показываем пустую строку как есть,
 * но не как «нет текста»: иначе вывод сдвинулся бы на строку. */
void conlog_append_chunk(ConLogBuffer *b, const char *data, gssize len)
{
    gsize total, pos, start;
    char *owned = NULL;   /* склеенная строка, наша обязанность освободить */

    if (!b || !data)
        return;
    if (len < 0)
        len = (gssize) strlen(data);
    if (len == 0)
        return;

    /* Склеиваем недописанный хвост с новой порцией: границы порций
     * произвольны, строка могла прийти по частям.
     *
     * g_string_free(..., FALSE), а не TRUE: при TRUE память освободилась
     * бы, а data на неё уже указывает. Это use-after-free, и он
     * проявлялся только на длинном потоке — короткий тест его не видел,
     * потому что освобождённый блок ещё лежал в кеше malloc. */
    if (b->pending_len) {
        GString *joined = g_string_new_len(b->pending, (gssize) b->pending_len);

        g_string_append_len(joined, data, len);
        g_free(b->pending);
        b->pending = NULL;
        b->pending_len = 0;
        owned = g_string_free(joined, FALSE);
        data = owned;
        len = (gssize) strlen(owned);
    }

    total = (gsize) len;
    start = 0;
    for (pos = 0; pos < total; pos++) {
        if (data[pos] != '\n')
            continue;
        {
            gsize n = pos - start;
            gboolean crlf = n > 0 && data[start + n - 1] == '\r';
            char *line = g_strndup(data + start, n);

            if (crlf)
                line[n - 1] = '\0';   /* CR от CRLF не нужен в строке */
            conlog_append(b, line, conlog_classify(line));
            g_free(line);
        }
        start = pos + 1;
    }

    if (start < total) {
        /* Хвост без перевода строки НЕ показываем: команда допишет его
         * следующей порцией, а показанный сейчас обрывок слова на
         * экране выглядел бы как дубль строки. */
        b->pending = g_strndup(data + start, (gssize) (total - start));
        b->pending_len = (size_t) (total - start);
    }

    g_free(owned);   /* хвост выше скопирован отдельно, можно освободить */
}

void conlog_buffer_set_strip_ansi(ConLogBuffer *b, gboolean on)
{
    if (b)
        b->strip_ansi = on;
}

void conlog_flush_pending(ConLogBuffer *b)
{
    if (!b || !b->pending_len)
        return;
    conlog_append(b, b->pending, conlog_classify(b->pending));
    g_free(b->pending);
    b->pending = NULL;
    b->pending_len = 0;
}

gboolean conlog_has_pending(const ConLogBuffer *b)
{
    return b && b->pending_len > 0;
}

const ConLogLine *conlog_get(const ConLogBuffer *b, guint index)
{
    if (!b || index >= b->lines->len)
        return NULL;
    return g_ptr_array_index(b->lines, index);
}

const char *conlog_text(const ConLogBuffer *b, guint index)
{
    const ConLogLine *l = conlog_get(b, index);

    return l ? l->text : NULL;
}

ConLogLevel conlog_level(const ConLogBuffer *b, guint index)
{
    const ConLogLine *l = conlog_get(b, index);

    return l ? l->level : CONLOG_LEVEL_NORMAL;
}

void conlog_clear(ConLogBuffer *b)
{
    if (!b)
        return;
    g_ptr_array_set_size(b->lines, 0);
    /* total_seen НЕ обнуляем: он нужен, чтобы «свежие» строки после
     * очистки не выглядели старыми. seq продолжает расти. */
    g_free(b->pending);
    b->pending = NULL;
    b->pending_len = 0;
}

/* ------------------------------------------------------- метрики */

int conlog_text_width(const char *font_desc, const char *text)
{
    PangoFontDescription *fd;
    PangoLayout *layout;
    PangoRectangle logical;
    cairo_surface_t *surface;
    cairo_t *cr;
    int width;

    if (!text || !*text)
        return 0;
    fd = pango_font_description_from_string(font_desc ? font_desc : "Monospace 9");
    surface = cairo_image_surface_create(CAIRO_FORMAT_ARGB32, 1, 1);
    cr = cairo_create(surface);
    layout = pango_cairo_create_layout(cr);
    pango_layout_set_font_description(layout, fd);
    pango_layout_set_text(layout, text, -1);
    pango_layout_get_extents(layout, NULL, &logical);
    /* Pango отдаёт единицы в PANGO_SCALE; без деления полоса в 1024
     * раза выше окна. */
    width = logical.width / PANGO_SCALE;
    g_object_unref(layout);
    cairo_destroy(cr);
    cairo_surface_destroy(surface);
    pango_font_description_free(fd);
    return width;
}

int conlog_line_height(const char *font_desc)
{
    PangoFontDescription *fd;
    PangoLayout *layout;
    PangoRectangle logical;
    cairo_surface_t *surface;
    cairo_t *cr;
    int height;

    fd = pango_font_description_from_string(font_desc ? font_desc : "Monospace 9");
    surface = cairo_image_surface_create(CAIRO_FORMAT_ARGB32, 1, 1);
    cr = cairo_create(surface);
    layout = pango_cairo_create_layout(cr);
    pango_layout_set_font_description(layout, fd);
    pango_layout_set_text(layout, "Mg0", -1);
    pango_layout_get_extents(layout, NULL, &logical);
    height = logical.height / PANGO_SCALE;
    if (height < 1)
        height = 1;
    g_object_unref(layout);
    cairo_destroy(cr);
    cairo_surface_destroy(surface);
    pango_font_description_free(fd);
    return height;
}

int conlog_text_height(const char *font_desc, const char *text)
{
    PangoFontDescription *fd;
    PangoLayout *layout;
    PangoRectangle logical;
    cairo_surface_t *surface;
    cairo_t *cr;
    int height;

    if (!text || !*text)
        return 0;
    fd = pango_font_description_from_string(font_desc ? font_desc : "Monospace 9");
    surface = cairo_image_surface_create(CAIRO_FORMAT_ARGB32, 1, 1);
    cr = cairo_create(surface);
    layout = pango_cairo_create_layout(cr);
    pango_layout_set_font_description(layout, fd);
    pango_layout_set_text(layout, text, -1);
    pango_layout_get_extents(layout, NULL, &logical);
    /* Высота строки целиком. Координаты отсчитываются от верха, поэтому
     * достаточно одного logical.height. */
    height = logical.height / PANGO_SCALE;
    if (height < 1)
        height = 1;
    g_object_unref(layout);
    cairo_destroy(cr);
    cairo_surface_destroy(surface);
    pango_font_description_free(fd);
    return height;
}

int conlog_font_metric(const char *font_desc, int which)
{
    PangoFontDescription *fd;
    PangoFontMetrics *m;
    cairo_surface_t *surface;
    cairo_t *cr;
    int v;

    fd = pango_font_description_from_string(font_desc ? font_desc : "Monospace 9");
    surface = cairo_image_surface_create(CAIRO_FORMAT_ARGB32, 1, 1);
    cr = cairo_create(surface);
    /* pango_font_map_load_font() возвращает PangoFont*, а метрики
     * лежат на нём же: get_metrics(). Путать эти два типа — ошибка
     * компиляции, а не runtime. */
    {
        /* Второй аргумент pango_font_map_load_font() — PangoContext*,
         * а pango_font_get_metrics() принимает PangoLanguage*. Я передал
         * контекст в оба места: первый сработал, второй — ошибка
         * компиляции. Язык NULL означает «системный по умолчанию». */
        PangoContext *pc = pango_cairo_create_context(cr);
        PangoFont *f = pango_font_map_load_font(pango_cairo_font_map_get_default(),
                                                pc, fd);

        m = f ? pango_font_get_metrics(f, NULL) : NULL;
        if (f)
            g_object_unref(f);
        g_object_unref(pc);
    }
    if (m) {
        v = (which == 0 ? pango_font_metrics_get_ascent(m)
                        : pango_font_metrics_get_descent(m)) / PANGO_SCALE;
        pango_font_metrics_unref(m);
    } else {
        v = 0;
    }
    cairo_destroy(cr);
    cairo_surface_destroy(surface);
    pango_font_description_free(fd);
    return v;
}

int conlog_text_ascent(const char *font_desc, const char *text)
{
    if (!text || !*text)
        return 0;
    return conlog_font_metric(font_desc, 0);
}

int conlog_text_descent(const char *font_desc, const char *text)
{
    if (!text || !*text)
        return 0;
    return conlog_font_metric(font_desc, 1);
}

gboolean conlog_needs_scroll(const ConLogBuffer *b, int window_height,
                             int first_row_y, int line_step)
{
    int need;

    if (!b || line_step <= 0)
        return FALSE;
    need = first_row_y + (int) b->lines->len * line_step;
    return need > window_height;
}
