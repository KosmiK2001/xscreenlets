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


/* ── Палитра SGR ────────────────────────────────────────────────
 * Базовые 16 цветов в кубической шкале (0, 95, 135, 175, 215, 255) —
 * те же значения, что у xterm и VTE, поэтому ls --color выглядит
 * привычно. */
static const guint8 sgr_basic[16][3] = {
    {   0,   0,   0 }, { 205,   0,   0 }, {   0, 205,   0 },
    { 205, 205,   0 }, {   0,   0, 238 }, { 205,   0, 205 },
    {   0, 205, 205 }, { 229, 229, 229 }, { 127, 127, 127 },
    { 255, 114, 114 }, { 114, 255, 114 }, { 255, 255, 114 },
    { 114, 114, 255 }, { 255, 114, 255 }, { 114, 255, 255 },
    { 255, 255, 255 }
};

static void sgr_256_rgb(guint n, gdouble rgb[3])
{
    /* 0..15 базовые, 16..231 куб 6x6x6, 232..255 серая шкала. */
    if (n < 16) {
        rgb[0] = sgr_basic[n][0] / 255.0;
        rgb[1] = sgr_basic[n][1] / 255.0;
        rgb[2] = sgr_basic[n][2] / 255.0;
    } else if (n < 232) {
        static const gdouble step[6] = { 0.0, 0.208, 0.416, 0.624, 0.792, 0.953 };
        guint c = n - 16;

        rgb[0] = step[(c / 36) % 6];
        rgb[1] = step[(c / 6) % 6];
        rgb[2] = step[c % 6];
    } else {
        gdouble v = (gdouble) (8 + (n - 232) * 10) / 255.0;

        rgb[0] = rgb[1] = rgb[2] = v;
    }
}

/* Сравнение стилей по полям, а не memcmp: в структуре есть padding,
 * и он не обязан совпадать даже между двумя memset-нулями. */
static gboolean sgr_style_equal(const ConLogStyle *a, const ConLogStyle *b)
{
    return a->bold == b->bold && a->italic == b->italic
        && a->underline == b->underline && a->inverse == b->inverse
        && a->dim == b->dim
        && a->has_fg == b->has_fg && a->has_bg == b->has_bg
        && a->fg[0] == b->fg[0] && a->fg[1] == b->fg[1]
        && a->fg[2] == b->fg[2] && a->fg[3] == b->fg[3]
        && a->bg[0] == b->bg[0] && a->bg[1] == b->bg[1]
        && a->bg[2] == b->bg[2] && a->bg[3] == b->bg[3];
}

gboolean conlog_has_sgr(const char *line)
{
    const char *p;

    if (!line)
        return FALSE;
    for (p = line; *p; p++) {
        if (*p != '\033' || p[1] != '[')
            continue;
        p += 2;
        /* Нужен именно «m»: прочие CSI (курсор, очистка экрана)
         * цветом не являются. */
        while (*p && !((guchar) *p >= '@' && (guchar) *p <= '~'))
            p++;
        if (*p == 'm')
            return TRUE;
        if (!*p)
            break;
    }
    return FALSE;
}

/* Освобождение временного прогона внутри разбора: и структура, и её
 * текст. Текст копируется в результат conlog_sgr_parse(), поэтому
 * здесь он освобождается ровно один раз. */
static void sgr_run_free(gpointer data)
{
    ConLogRun *r = data;

    if (!r)
        return;
    g_free(r->text);
    g_free(r);
}

void conlog_sgr_free(ConLogRun *runs, guint n_runs)
{
    guint i;

    /* n_runs приходит от того же вызова conlog_sgr_parse(), но если
     * разбор вернул NULL (пустая строка), счётчик всё равно мог бы
     * остаться ненулевым у вызывающего. Проверка на NULL обязательна:
     * без неё цикл читал бы runs[0].text из NULL. */
    if (!runs)
        return;
    for (i = 0; i < n_runs; i++) {
        if (runs[i].text)
            g_free(runs[i].text);
    }
    g_free(runs);
}

/* Закрыть текущий прогон, если в нём есть что показать. Пустые
 * прогоны не храним: два SGR подряд без текста между ними — это
 * смена стиля, а не два видимых куска. */
static void sgr_flush_run(GPtrArray *out, GString *text,
                          const ConLogStyle *st, const gdouble line_color[4])
{
    ConLogRun run;

    if (text->len == 0)
        return;
    memcpy(&run.style, st, sizeof run.style);
    if (!run.style.has_fg) {
        memcpy(run.style.fg, line_color, sizeof run.style.fg);
    }
    if (run.style.inverse) {
        gdouble t[4];

        memcpy(t, run.style.fg, sizeof t);
        if (run.style.has_bg)
            memcpy(run.style.fg, run.style.bg, sizeof t);
        else
            memcpy(run.style.fg, line_color, sizeof t);
        run.style.fg[3] = 1.0;
        memcpy(run.style.bg, t, sizeof t);
    }
    /* В GPtrArray кладётся УКАЗАТЕЛЬ на прогон, а free_func освобождает
     * и его, и текст. Итоговый плотный массив собирается разыменованием.
     * Класть значение (g_memdup2 в pdata) нельзя: тогда элементы
     * pdata — указатели на структуры, а memcpy с n*sizeof(ConLogRun)
     * копировал бы сами указатели вместо прогонов. */
    /* run.text обязателен: структура копируется целиком, и без
     * присваивания в прогон попадал мусор со стека — g_free() на нём
     * и давал «free(): invalid size», а runs[i].text показывал
     * пустую строку. */
    run.text = g_strdup(text->str);
    g_ptr_array_add(out, g_memdup2(&run, sizeof run));
    g_string_truncate(text, 0);
}

ConLogRun *conlog_sgr_parse(const char *line, const gdouble line_color[4],
                            guint *n_runs)
{
    GPtrArray *out;
    ConLogStyle st;
    GString *text;
    const char *p;
    gdouble base[4] = { 0.85, 0.85, 0.85, 1.0 };

    if (n_runs)
        *n_runs = 0;
    if (!line)
        return NULL;
    if (line_color)
        memcpy(base, line_color, sizeof base);

    memset(&st, 0, sizeof st);
    text = g_string_new(NULL);
    out = g_ptr_array_new_with_free_func((GDestroyNotify) sgr_run_free);

    for (p = line; *p; ) {
        if (*p == '\033' && p[1] == ']') {
            /* OSC: до BEL или ST (ESC \\). Заголовок окна из xterm
             * не должен попадать в текст строки. */
            const char *q = p + 2;

            while (*q && *q != '\007' && !(*q == '\033' && q[1] == '\\'))
                q++;
            p = (*q == '\007') ? q + 1 : (*q ? q + 2 : q);
            continue;
        }
        if (*p == '\033' && p[1] == '[') {
            const char *q = p + 2;
            /* Инициализация обязательна: при nparams=0 первый параметр
             * писался в params[0] += цифра, то есть к мусору со
             * стека. Из-за этого «ESC[31m» давал случайный код
             * цвета, а прогон не делился. */
            int params[10] = { 0 };
            int nparams = 0;
            gboolean is_sgr = FALSE;
            int k;

            while (*q && !((guchar) *q >= '@' && (guchar) *q <= '~')) {
                if (*q >= '0' && *q <= '9') {
                    if (nparams == 0)
                        nparams = 1;
                    if (nparams <= 9)
                        params[nparams - 1] = params[nparams - 1] * 10
                                              + (*q - '0');
                } else if (*q == ';') {
                    if (nparams < 9)
                        nparams++;
                    else
                        nparams++;   /* переполнение игнорируем */
                }
                q++;
            }
            is_sgr = (*q == 'm');
            if (!*q)
                q = NULL;
            else
                q++;

            if (is_sgr) {
                /* Стиль ДО разбора: если он отличается от текущего,
                 * прогон закрывается и начинается новый. Раньше прогон
                 * закрывался только на коде 0, из-за чего «31mA0mB»
                 * слипался в один кусок и красился одним цветом. */
                ConLogStyle before = st;

                if (nparams == 0)
                    params[nparams++] = 0;   /* «ESC[m» = полный сброс */
                for (k = 0; k < nparams; k++) {
                    int v = params[k];

                    switch (v) {
                    case 0:
                        memset(&st, 0, sizeof st);
                        break;
                    case 1:  st.bold = TRUE; break;
                    case 2:  st.dim = TRUE; break;
                    case 3:  st.italic = TRUE; break;
                    case 4:  st.underline = TRUE; break;
                    case 7:  st.inverse = TRUE; break;
                    case 22: st.bold = st.dim = FALSE; break;
                    case 23: st.italic = FALSE; break;
                    case 24: st.underline = FALSE; break;
                    case 27: st.inverse = FALSE; break;
                    case 30: case 31: case 32: case 33:
                    case 34: case 35: case 36: case 37:
                        /* 30..37: стандартные цвета терминала. На тёмном
                         * фоне они нечитаемы, поэтому берём тот же цвет,
                         * что и у соответствующего яркого кода 90..97. */
                        st.has_fg = TRUE;
                        st.fg[0] = sgr_basic[v - 30 + 8][0] / 255.0;
                        st.fg[1] = sgr_basic[v - 30 + 8][1] / 255.0;
                        st.fg[2] = sgr_basic[v - 30 + 8][2] / 255.0;
                        st.fg[3] = 1.0;
                        break;
                    case 39: st.has_fg = FALSE; break;
                    case 90: case 91: case 92: case 93:
                    case 94: case 95: case 96: case 97:
                        st.has_fg = TRUE;
                        st.fg[0] = sgr_basic[v - 90 + 8][0] / 255.0;
                        st.fg[1] = sgr_basic[v - 90 + 8][1] / 255.0;
                        st.fg[2] = sgr_basic[v - 90 + 8][2] / 255.0;
                        st.fg[3] = 1.0;
                        break;
                    case 49: st.has_bg = FALSE; break;
                    case 40: case 41: case 42: case 43:
                    case 44: case 45: case 46: case 47:
                        st.has_bg = TRUE;
                        st.bg[3] = 1.0;
                        st.bg[0] = sgr_basic[v - 40 + 8][0] / 255.0;
                        st.bg[1] = sgr_basic[v - 40 + 8][1] / 255.0;
                        st.bg[2] = sgr_basic[v - 40 + 8][2] / 255.0;
                        break;
                    case 38: case 48: {
                        gboolean fg = (v == 38);
                        int mode = (k + 1 < nparams) ? params[k + 1] : -1;
                        gdouble rgb[3];

                        if (mode == 5 && k + 2 < nparams) {
                            sgr_256_rgb((guint) params[k + 2], rgb);
                            k += 2;
                        } else if (mode == 2 && k + 4 < nparams) {
                            rgb[0] = params[k + 2] / 255.0;
                            rgb[1] = params[k + 3] / 255.0;
                            rgb[2] = params[k + 4] / 255.0;
                            k += 4;
                        } else {
                            break;   /* неполярный код: игнорируем */
                        }
                        if (fg) {
                            st.has_fg = TRUE;
                            memcpy(st.fg, rgb, sizeof rgb);
                            st.fg[3] = 1.0;
                        } else {
                            st.has_bg = TRUE;
                            memcpy(st.bg, rgb, sizeof rgb);
                            st.bg[3] = 1.0;
                        }
                        break;
                    }
                    default:
                        break;
                    }
                }
                /* Прогон закрывается ДО смены стиля и несёт стиль,
                 * действовавший, пока набирался его текст. Закрытие
                 * после разбора давало пустой прогон с уже-новым
                 * стилем, и весь SGR молча терялся. */
                if (!sgr_style_equal(&before, &st))
                    sgr_flush_run(out, text, &before, base);
                p = q ? q : line + strlen(line);
                continue;
            }
            /* Прочие CSI (курсор, очистка): вырезаем, текст не рвётся. */
            p = q ? q : line + strlen(line);
            continue;
        }
        if ((guchar) *p < 0x20 && *p != '\t')
            p++;          /* управляющие символы, кроме табуляции */
        else
            g_string_append_c(text, *p), p++;
    }

    sgr_flush_run(out, text, &st, base);
    g_string_free(text, TRUE);

    /* Результат — плотный массив ConLogRun, а не GPtrArray:
     * отрисовка идёт по нему подряд, без разыменования указателей.
     * Сами прогоны копируются и освобождаются conlog_sgr_free(). */
    {
        guint n = out->len;
        ConLogRun *res;

        if (n_runs)
            *n_runs = n;
        if (n == 0) {
            g_ptr_array_free(out, TRUE);
            return NULL;
        }
        res = g_new(ConLogRun, n);
        {
            guint k;

            for (k = 0; k < n; k++) {
                const ConLogRun *src = g_ptr_array_index(out, k);

                res[k] = *src;
                res[k].text = g_strdup(src->text);
            }
        }
        g_ptr_array_free(out, TRUE);
        return res;
    }
}

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
    /* keep_ansi: строка сохраняется С escape-последовательностями —
     * их разберёт conlog_sgr_parse() при отрисовке. Чистить здесь
     * нельзя: после этого разбирать было бы нечего. */
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

guint64 conlog_seq(const ConLogBuffer *b, guint index)
{
    if (!b || index >= b->lines->len)
        return 0;
    return ((ConLogLine *) g_ptr_array_index(b->lines, index))->seq;
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
