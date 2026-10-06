/* conlog.c — апплет «вывод команды».
 *
 * Пользователь задаёт команду в Настройках, апплет запускает её и
 * показывает вывод: последние строки, с прокруткой назад, с подсветкой
 * по уровню. Примеры: journalctl -f, dmesg -w, tail -F лог.
 *
 * Три вещи, которые здесь легко испортить, и почему:
 *
 * 1. Команда живёт дольше одного тика. Она запускается ОДИН раз на
 *    весь сеанс и живёт в GSubprocess с неблокирующим чтением через
 *    GIOChannel. Никакого ожидания внутри tick(): tick зовётся из
 *    главного потока GTK, и read() там повесил бы весь интерфейс.
 *
 * 2. Буфер не растёт бесконечно. Команда может писать непрерывно
 *    (journalctl -f при активной системе), а окно — конечно. Старое
 *    отбрасывается по лимиту, и это делает conlog_core.
 *
 * 3. Команда не должна перезапускаться на каждом тике. Иначе между
 *    тиками она успевала бы напечатать первую строку заново, и в буфере
 *    лежали бы дубликаты. Триггер — только смена команды в Настройках.
 */
#include "xs_api.h"
#include "common.h"
#include "conlog_core.h"

#include <gtk/gtk.h>
#include <string.h>
#include <stdlib.h>
#include <gio/gio.h>
#include <gio/gfiledescriptorbased.h>

#include "../core/i18n.h"
#define CONLOG_DEFAULT_COMMAND  "journalctl -f -n 20"
#define CONLOG_DEFAULT_WIDTH    380
#define CONLOG_DEFAULT_HEIGHT   180
#define CONLOG_MIN_WIDTH        160
#define CONLOG_MIN_HEIGHT       60
#define CONLOG_DEFAULT_FONT     "Monospace 9"
#define CONLOG_MARGIN          6
#define CONLOG_ROWS_GAP        4   /* просвет под разделителем */
#define CONLOG_TITLE_TOP       3   /* отступ рамки до верха метки */
#define CONLOG_TITLE_PAD       4   /* просвет глифов до разделителя */
/* Сколько байт читаем за раз. Небольшой кусок нужен, чтобы не съесть
 * кадр на одном гигантском куске вывода: journalctl при перезагрузке
 * может выдать мегабайт истории одним write(). */
#define CONLOG_READ_CHUNK       4096
/* +1 на NUL: g_io_channel_read_chars() НЕ терминирует буфер, а
 * conlog_strip_ansi() работает по strlen(). Без запаса сканер
 * уходил за пределы порции, терял переводы строк и копил хвост
 * до мегабайтов — на экране окно оставалось пустым. */
#define CONLOG_READ_BUF         (CONLOG_READ_CHUNK + 1)

typedef struct _ConlogPriv ConlogPriv;

struct _ConlogPriv {
    XsPlugin   *plugin;
    GKeyFile   *kf;

    char       *command;       /* команда пользователя */
    char       *cwd;           /* рабочий каталог команды */
    ConLogBuffer *buf;

    /* Живой процесс вывода. */
    GSubprocess  *proc;
    GInputStream *in_stream;
    GIOChannel   *chan;
    guint         watch_id;
    gboolean      running;

    /* Прокрутка: индекс первой видимой строки. */
    int          scroll_top;

    /* Геометрия. */
    int          width, height;
    gboolean     width_auto, height_auto;
    int          design_width, design_height;
    int          first_row_y, line_step;
    int          corner_radius;
    double       opacity;

    char        *title;         /* метка пользователя; пусто = без неё */
    char        *title_font;
    char        *row_font;
    int          title_h;      /* высота зоны заголовка, 0 = нет */
    int          title_dx;     /* смещение метки по X, px */
    int          title_dy;     /* смещение метки по Y, px (обычно минус) */
    gdouble      title_color[4];
    gdouble      row_color[4];
    gdouble      level_colors[5][4];
    gdouble      background_color[4];
    gdouble      border_color[4];

    gboolean     colorize;     /* подсветка по уровню */
    gboolean     wrap;         /* перенос длинных строк */
    gboolean     strip_ansi;   /* вырезать escape-последовательности */
    gboolean     keep_ansi;    /* СОХРАНЯТЬ чужой цвет (SGR) в выводе */
    gboolean     autoscroll;   /* следовать за хвостом */

    cairo_surface_t *cache;
    int          cache_width, cache_height;

    /* Кэш авто-ширины: измерение 200 строк через Pango дорого, а тик
     * тикает постоянно. Пересчёт — только когда буфер изменился. */
    int          widest_cached;
    gboolean     widest_valid;
    guint64      seen_cached;

    /* Кэш разбивки строк на прогоны (SGR). На инстанс, а не на
     * процесс: ширина прогонов зависит от шрифта, а у applet
     * бывает несколько инстансов с разными шрифтами. Создаётся
     * лениво и только при keep_ansi — иначе он не нужен вовсе. */
    ConLogRunCache *runcache;
};

/* Живая таблица инстансов: контекст диалога переживает properties(),
 * и его обработчики должны находить ЖИВОЙ priv по имени секции. */
static GHashTable *conlog_instances;

#define CONLOG_MAX_DRAINS  64   /* порций за один вызов обработчика */

static guint conlog_drains;

static void conlog_start(ConlogPriv *priv);
static void conlog_stop(ConlogPriv *priv);

/* ------------------------------------------------------------- ANSI */

/* Многие команды (ip, systemctl, ls --color) пишут escape-последовательности.
 * В апплете они рисуются как мусор из символов, поэтому вырезаем.
 * Работает по строке, а не по потоку: последовательность может прийти
 * разорванной порциями, но в строку попадает уже собранной. */

/* ------------------------------------------------------- процесс */

/* Прочитать доступное и положить в буфер. Возвращает FALSE, когда
 * канал закрыт и читать больше нечего. */
static gboolean conlog_drain(ConlogPriv *priv, gboolean *got_any)
{
    char *buf;
    gsize got = 0;
    GIOStatus st;

    *got_any = FALSE;
    buf = g_malloc(CONLOG_READ_BUF);
    for (;;) {
        got = 0;
        st = g_io_channel_read_chars(priv->chan, buf, CONLOG_READ_CHUNK,
                                     &got, NULL);
        if (st == G_IO_STATUS_NORMAL || st == G_IO_STATUS_AGAIN)
            buf[got] = '\0';   /* conlog_append_chunk() ждёт строку */
        if (st != G_IO_STATUS_NORMAL || got == 0)
            break;
        *got_any = TRUE;
        /* Сырую порцию отдаём как есть: escape-последовательности
         * вырезаются в conlog_append(), уже на собранной строке, —
         * на порции они могут быть разорваны границей чтения. */
        conlog_append_chunk(priv->buf, buf, (gssize) got);
        /* Кусок прочитан, но цикл продолжает: в pipe могло быть ещё.
         * Ограничиваем итерации, чтобы на бесконечном потоке
         * (journalctl -f при потоке событий) обработчик не занял
         * главный поток — вернёмся к следующему событию. */
        if (++conlog_drains >= CONLOG_MAX_DRAINS)
            break;
    }
    g_free(buf);
    return st == G_IO_STATUS_NORMAL || st == G_IO_STATUS_AGAIN;
}

static guint conlog_drains;   /* счётчик итераций на один вызов */

/* Пришёл кусок вывода. Вызывается из главного потока GTK: здесь можно
 * трогать GTK, а процессор ждать нельзя. */
static gboolean conlog_on_io(GIOChannel *chan, GIOCondition cond, gpointer data)
{
    ConlogPriv *priv = data;
    gboolean got = FALSE;

    (void) chan;
    if (!priv)
        return G_SOURCE_REMOVE;

    conlog_drains = 0;
    /* Читаем при ЛЮБОМ условии, и в том числе при HUP: на закрытом
     * канале в pipe ещё лежит непрочитанный хвост, а терять его —
     * значит потерять последние строки вывода команды. */
    conlog_drain(priv, &got);

    if (cond & (G_IO_HUP | G_IO_ERR)) {
        /* Команда завершилась: показываем недописанную строку и
         * перестаём читать. Возобновление — только по смене команды,
         * иначе завершившийся «date» через секунду стартовал бы сам. */
        conlog_flush_pending(priv->buf);
        priv->running = FALSE;
        if (priv->plugin && priv->plugin->win)
            xs_host_api()->invalidate(priv->plugin);
        return G_SOURCE_REMOVE;
    }

    if (got) {
        /* Держим хвост в виду: пользователь смотрит в конец лога. */
        if (priv->autoscroll)
            priv->scroll_top = 0;
        if (priv->plugin && priv->plugin->win)
            xs_host_api()->invalidate(priv->plugin);
    }
    return G_SOURCE_CONTINUE;
}

static void conlog_stop(ConlogPriv *priv)
{
    if (!priv)
        return;
    if (priv->watch_id) {
        g_source_remove(priv->watch_id);
        priv->watch_id = 0;
    }
    if (priv->chan) {
        g_io_channel_unref(priv->chan);
        priv->chan = NULL;
    }
    if (priv->in_stream) {
        g_object_unref(priv->in_stream);
        priv->in_stream = NULL;
    }
    if (priv->proc) {
        /* Процесс мог зависнуть на выводе в уже закрытый канал: без
         * force процесс остался бы зомби. */
        if (priv->running)
            g_subprocess_force_exit(priv->proc);
        g_object_unref(priv->proc);
        priv->proc = NULL;
    }
    priv->running = FALSE;
}

/* Команда через /bin/sh -c: пользователь пишет в Настройках именно то,
 * что вводил бы в терминале — с кавычками, перенаправлением и
 * переменными. Разбирать строку вручную значило бы не поддержать
 * половину синтаксиса оболочки. */
static void conlog_start(ConlogPriv *priv)
{
    GError *err = NULL;
    char *argv[4];
    char *sh = NULL;

    if (!priv || !priv->command || !*priv->command)
        return;
    conlog_stop(priv);

    sh = g_find_program_in_path("/bin/sh");
    if (!sh)
        sh = g_strdup("/bin/sh");
    argv[0] = sh;
    argv[1] = (char *) "-c";
    argv[2] = priv->command;
    argv[3] = NULL;

    priv->proc = g_subprocess_newv((const char * const *) argv,
                                   G_SUBPROCESS_FLAGS_STDIN_PIPE |
                                   G_SUBPROCESS_FLAGS_STDOUT_PIPE |
                                   G_SUBPROCESS_FLAGS_STDERR_SILENCE,
                                   &err);
    g_free(sh);
    if (!priv->proc) {
        if (priv->plugin && priv->plugin->host)
            priv->plugin->host->log("conlog: не запустилась «%s»: %s",
                                    priv->command,
                                    err ? err->message : "неизвестно");
        g_clear_error(&err);
        /* Показываем причину в апплете, а не только в логе демона:
         * иначе пользователь видит пустое окно и гадает. */
        if (err) {
            char *msg = g_strdup_printf("не запустилась: %s", err->message);
            conlog_append(priv->buf, msg, CONLOG_LEVEL_ERROR);
            g_free(msg);
        }
        return;
    }

    /* GSubprocess отдаёт GInputStream, а не GIOChannel. Мост между ними
     * делает g_io_channel_unix_new — и он же переводит дескриптор в
     * неблокирующий режим, без чего чтение в обработчике повесило бы
     * главный поток. */
    priv->in_stream = g_subprocess_get_stdout_pipe(priv->proc);
    if (!priv->in_stream) {
        g_object_unref(priv->proc);
        priv->proc = NULL;
        return;
    }
    /* Дескриптор достаём через g_file_descriptor_based_get_fd: это
     * публичный интерфейс GIO, в отличие от GUnixInputStream, который
     * живёт в отдельном пакете gio-unix-2.0. На машине без него
     * плагин просто не собрался бы. */
    {
        int fd = g_file_descriptor_based_get_fd(
                     G_FILE_DESCRIPTOR_BASED(priv->in_stream));

        priv->chan = (fd >= 0) ? g_io_channel_unix_new(fd) : NULL;
    }
    if (!priv->chan) {
        g_object_unref(priv->in_stream);
        priv->in_stream = NULL;
        g_object_unref(priv->proc);
        priv->proc = NULL;
        return;
    }
    g_io_channel_set_encoding(priv->chan, NULL, NULL);
    g_io_channel_set_buffered(priv->chan, FALSE);
    g_io_channel_set_flags(priv->chan, G_IO_FLAG_NONBLOCK, NULL);
    /* G_IO_IN — «данные пришли». HUP и ERR ловим отдельно: на
     * завершении команды условия приходят одним пакетом, и смешивание
     * давало бы двойное освобождение канала. */
    /* ОДИН watch на IN|HUP|ERR. Два watch на одном канале работали
     * плохо: условия приходят пачками, и тот, кто отработал первым,
     * снимал источник, оставляя второй висеть на мёртвом канале.
     * Проверено стендом: с одним watch условия приходят по очереди,
     * каждое с одним битом, и на HUP данные ещё доступны. */
    priv->watch_id = g_io_add_watch(priv->chan, G_IO_IN | G_IO_HUP | G_IO_ERR,
                                    conlog_on_io, priv);
    priv->running = TRUE;
}

/* ------------------------------------------------------- отрисовка */

/* Скруглённый прямоугольник по текущему пути. Совпадает с формулой
 * остальных апплетов: радиус клипа, радиус рамки и радиус формы
 * окна должны быть одним и тем же, иначе клики попадали бы в углы
 * мимо окна. */
static void cl_round_rect(cairo_t *cr, int width, int height, int radius)
{
    const double inset = 1.0;
    double w = width - 2 * inset, h = height - 2 * inset;
    double r = radius > 0 ? (double) radius : 0.0;

    if (w <= 0 || h <= 0) {
        cairo_rectangle(cr, 0, 0, width, height);
        return;
    }
    if (r > w / 2.0)
        r = w / 2.0;
    if (r > h / 2.0)
        r = h / 2.0;
    if (r <= 0.0) {
        cairo_rectangle(cr, inset, inset, w, h);
        return;
    }
    cairo_new_sub_path(cr);
    cairo_arc(cr, inset + w - r, inset + r, r, -G_PI / 2, 0);
    cairo_arc(cr, inset + w - r, inset + h - r, r, 0, G_PI / 2);
    cairo_arc(cr, inset + r, inset + h - r, r, G_PI / 2, G_PI);
    cairo_arc(cr, inset + r, inset + r, r, G_PI, 3 * G_PI / 2);
    cairo_close_path(cr);
}


static int cl_text_width(cairo_t *cr, const char *font, const char *text)
{
    PangoFontDescription *fd;
    PangoLayout *layout;
    PangoRectangle logical;
    int width;

    if (!text || !*text)
        return 0;
    fd = pango_font_description_from_string(font);
    layout = pango_cairo_create_layout(cr);
    pango_layout_set_font_description(layout, fd);
    pango_layout_set_text(layout, text, -1);
    pango_layout_get_extents(layout, NULL, &logical);
    width = logical.width / PANGO_SCALE;   /* Pango отдаёт в PANGO_SCALE */
    g_object_unref(layout);
    pango_font_description_free(fd);
    return width;
}


/* y_top — координата ВЕРХА текста, как её и показывает pango.
 *
 * Раньше параметр назывался baseline, но pango_cairo_show_layout()
 * позиционирует layout по его левому ВЕРХНЕМУ углу, а не по базовой
 * линии. Из-за этого весь апплет уезжал вниз на высоту baseline
 * layout: метка упиралась в разделитель, первая строка журнала
 * уезжала ещё ниже, и под разделителем появлялась пустая строка.
 * Здесь y_top переводится в координату cairo, а имя говорит правду. */
static void cl_draw_text(cairo_t *cr, const char *font, int x, gdouble y_top,
                         const char *text, const gdouble color[4])
{
    PangoFontDescription *fd;
    PangoLayout *layout;

    if (!text || !*text)
        return;
    fd = pango_font_description_from_string(font);
    layout = pango_cairo_create_layout(cr);
    pango_layout_set_font_description(layout, fd);
    pango_layout_set_text(layout, text, -1);
    cairo_set_source_rgba(cr, color[0], color[1], color[2], color[3]);
    cairo_move_to(cr, x, y_top);
    pango_cairo_show_layout(cr, layout);
    g_object_unref(layout);
    pango_font_description_free(fd);
}

/* Показываем только строки, влезающие в окно, и только в пределах
 * буфера. */
/* Ленивая инициализация кэша: он общий на процесс, а не на инстанс. */
static GHashTable *cl_runcache_ensure(void);

/* ── Кэш разбивки строки на прогоны ─────────────────────────────
 *
 * Разбор SGR на каждый кадр стоил 99% CPU: на окне в 40 строк ls
 * --color это ~400 вызовов conlog_sgr_parse() в секунду, каждый с
 * g_strdup на каждый прогон. Разбивка и её ширина кэшируются по
 * порядковому номеру строки — см. conlog_runcache_get() в core.
 * Сам кэш живёт в core не случайно: там он тестируется без X, и
 * ограничение его размера проверяется тестом, а не глазами.
 *
 * Кэш на инстанс, а не на процесс: у applet может быть несколько
 * инстансов с разными шрифтами, а ширина прогонов зависит от
 * шрифта. Общий кэш потребовал бы ключа «шрифт + номер строки» и
 * риска отдать прогоны, посчитанные чужим шрифтом.
 */
/* Разбор с кэшем. Кэш ленивый: у applet без keep_ansi он не нужен
 * вовсе, и пустой хеш на каждый инстанс — лишняя работа. */
static const ConLogLineRuns *cl_line_runs_get(ConlogPriv *priv,
                                               const char *text,
                                               guint64 seq,
                                               const gdouble color[4])
{
    if (!priv->runcache)
        priv->runcache = conlog_runcache_new(CONLOG_RUNCACHE_DEFAULT);
    if (!priv->runcache)
        return NULL;
    return conlog_runcache_get(priv->runcache, seq, text, color,
                               priv->row_font);
}

/* Отрисовать прогон. Ширина уже посчитана при построении кэша
 * строки, поэтому здесь только отрисовка. */
static void cl_draw_run(cairo_t *cr, const char *font, gdouble x, gdouble y,
                        const char *text, const gdouble color[4])
{
    cl_draw_text(cr, font, (int) x, y, text, color);
}

static cairo_surface_t *cl_render(ConlogPriv *priv, int width, int height)
{
    cairo_surface_t *surface;
    cairo_t *cr;
    int step, shown = 0;
    gdouble y, rows_top;
    guint total, first, last;
    ConLogView view;
    const char *font = priv->row_font;

    surface = cairo_image_surface_create(CAIRO_FORMAT_ARGB32, width, height);
    cr = cairo_create(surface);
    cairo_set_operator(cr, CAIRO_OPERATOR_SOURCE);
    cairo_set_source_rgba(cr, 0, 0, 0, 0);
    cairo_paint(cr);
    cairo_set_operator(cr, CAIRO_OPERATOR_OVER);

    /* Фон и рамка — то же скругление, что у остальных апплетов. */
    cairo_set_source_rgba(cr, priv->background_color[0],
                          priv->background_color[1], priv->background_color[2],
                          priv->background_color[3]);
    cl_round_rect(cr, width, height, priv->corner_radius);
    cairo_fill_preserve(cr);
    cairo_set_source_rgba(cr, priv->border_color[0], priv->border_color[1],
                          priv->border_color[2], priv->border_color[3]);
    cairo_set_line_width(cr, 1.0);
    cairo_stroke(cr);

    cairo_save(cr);
    cl_round_rect(cr, width, height, priv->corner_radius);
    cairo_clip(cr);

    step = conlog_line_height(font);
    if (priv->line_step > step)
        step = priv->line_step;

    /* Арифметика видимых строк живёт в conlog_visible_lines(): там же
     * проверяется тестом. Дублировать её здесь нельзя — однажды копия
     * уже разошлась с оригиналом и показывала пустое окно при полном
     * буфере. */
    /* Зона заголовка: метка пользователя сверху, ниже неё — разделитель,
     * и только потом строки журнала. Метка рисуется ТОЛЬКО если задана:
     * пустой заголовок не должен оставлять дыру, как раньше. */
    if (priv->title_h > 0) {
        /* Зона заголовка: [first_row_y, first_row_y + title_h).
         * Внутри неё базовая линия метки ставится по МЕТРИКАМ ШРИФТА:
         *   baseline + descent <= divider - CONLOG_TITLE_PAD
         * Именно это условие держит кириллицу («р», «ц», «у») над
         * разделителем — descender уходит на 4 px ниже базовой линии.
         *
         * Раньше ascent читался как -logical.y из pango_layout_get_extents(),
         * что давало 1: extents отсчитываются от ВЕРХА строки, а не от
         * базовой линии. */
        /* Метка рисуется ВЕРХОМ в зону, разделитель — под зоной.
         * Верх глифов = верх зоны + отступ рамки. Никакой baseline
         * тут не нужен: pango позиционирует layout по верху. */
        gdouble  zone_top = priv->first_row_y + (gdouble) priv->title_h;
        gdouble  divider  = zone_top + 0.5;
        /* Метка рисуется ВЕРХОМ в зону, разделитель — под зоной.
         * title_dy обычно отрицательный: пользователь подтягивает
         * метку к самому верху окна, не трогая зону строк ниже. */
        gdouble  label_x  = CONLOG_MARGIN + (gdouble) priv->title_dx;
        gdouble  label_y  = priv->first_row_y + CONLOG_TITLE_TOP
                          + (gdouble) priv->title_dy;

        cl_draw_text(cr, priv->title_font, label_x, label_y,
                     priv->title, priv->title_color);
        cairo_set_source_rgba(cr, priv->border_color[0], priv->border_color[1],
                              priv->border_color[2], priv->border_color[3] * 0.55);
        cairo_set_line_width(cr, 1.0);
        cairo_move_to(cr, CONLOG_MARGIN, divider);
        cairo_line_to(cr, width - CONLOG_MARGIN, divider);
        cairo_stroke(cr);
    }

    /* Строки журнала начинаются ниже разделителя, с просветом в
     * несколько пикселей: иначе верхняя строка прилипала бы к линии. */
    rows_top = priv->first_row_y + priv->title_h + CONLOG_ROWS_GAP;
    view = conlog_visible_lines(conlog_len(priv->buf), rows_top,
                                step, height, priv->scroll_top);
    first = view.first;
    last = first + view.count;
    total = conlog_len(priv->buf);
    /* y — ВЕРХ первой строки, а не базовая линия: именно так
     * позиционирует pango_cairo_show_layout(). Раньше здесь стоял
     * «rows_top + step», то есть на целый шаг ниже зоны строк, и под
     * разделителем появлялась пустая строка. */
    y = rows_top;

    for (guint i = first; i < last; i++) {
        const char *text = conlog_text(priv->buf, i);
        const gdouble *color = priv->row_color;

        if (!text)
            break;
        if (y > height + step)
            break;   /* строка ниже окна — дальше рисовать нечего */
        if (priv->colorize)
            color = priv->level_colors[conlog_level(priv->buf, i)];

        if (priv->keep_ansi && conlog_has_sgr(text)) {
            /* Строка с чужим цветом: рисуется по прогонам. Разбивка
             * кэшируется по номеру строки — без этого каждый кадр
             * заново парсил SGR, и демон уходил в 99% CPU. */
            guint64 seq = conlog_seq(priv->buf, i);
            const ConLogLineRuns *lr = cl_line_runs_get(priv, text, seq,
                                                         color);
            gdouble rx = CONLOG_MARGIN;
            gboolean drawn = FALSE;

            /* Ширина берётся из кэша, а не перемеряется: пересчёт
             * на каждом кадре — вторая половина той же нагрузки,
             * что и разбор SGR. */
            for (guint k = 0; lr && k < lr->n; k++) {
                if (!lr->runs[k].text || !*lr->runs[k].text)
                    continue;
                cl_draw_run(cr, font, rx, y, lr->runs[k].text,
                            lr->runs[k].style.fg);
                rx += (lr->run_width ? lr->run_width[k] : 0.0);
                drawn = TRUE;
            }
            if (!drawn)
                cl_draw_run(cr, font, CONLOG_MARGIN, y, text, color);
        } else {
            cl_draw_run(cr, font, CONLOG_MARGIN, y, text, color);
        }
        y += step;
        shown++;
    }

    /* Подсказка, когда команда молчит, — иначе пустое окно выглядит
     * как «плагин сломался», и пользователь гадает, где ошибка. */
    if (shown == 0) {
        const char *hint;

        if (!priv->command || !*priv->command)
            hint = "команда не задана — см. Настройки";
        else if (!priv->running)
            hint = "команда завершилась — см. Настройки";
        else
            hint = "ожидание вывода…";
        cl_draw_text(cr, font, CONLOG_MARGIN, y, hint, priv->row_color);
    }

    cairo_restore(cr);
    cairo_destroy(cr);
    cairo_surface_mark_dirty(surface);
    return surface;
}

static void cl_rebuild_cache(ConlogPriv *priv, int w, int h)
{
    cairo_surface_t *fresh;

    if (!priv || w <= 0 || h <= 0)
        return;
    fresh = cl_render(priv, w, h);
    if (priv->cache)
        cairo_surface_destroy(priv->cache);
    priv->cache = fresh;
    priv->cache_width = w;
    priv->cache_height = h;
}

/* Пересчитать размеры окна по содержимому. */
static void cl_recalc_size(ConlogPriv *priv)
{
    int step, need_h, widest = 0;

    if (!priv)
        return;
    step = conlog_line_height(priv->row_font);
    if (priv->line_step > step)
        step = priv->line_step;

    /* Зона заголовка: ровно одна строка, только если заголовок задан.
     * Считается здесь, потому что и перерисовка, и расчёт высоты
     * окна должны знать одно и то же число — иначе строки наезжают
     * на метку при первом же изменении размера. */
    /* Зона заголовка: своя высота плюс отступ от рамки. Считается
     * по строке текста, а не шагом строки журнала. */
    priv->title_h = (priv->title && *priv->title)
                  ? conlog_text_height(priv->title_font, priv->title)
                    + CONLOG_TITLE_TOP : 0;

    if (priv->height_auto) {
        /* Все строки, а не только видимые: иначе окно «прыгало» бы
         * при прокрутке. */
        need_h = priv->first_row_y + priv->title_h + CONLOG_ROWS_GAP
               + (int) conlog_len(priv->buf) * step + 12;
        priv->height = CLAMP(need_h, CONLOG_MIN_HEIGHT, 1200);
    }
    if (priv->width_auto) {
        guint64 seen = conlog_total_seen(priv->buf);

        /* Шрифт и размер окна тоже влияют на измерение, поэтому при их
         * смене кэш сбрасывается принудительно. */
        if (!priv->widest_valid || priv->seen_cached != seen) {
            cairo_surface_t *s = cairo_image_surface_create(CAIRO_FORMAT_ARGB32, 1, 1);
            cairo_t *c = cairo_create(s);

            for (guint i = 0; i < conlog_len(priv->buf); i++) {
                int w = cl_text_width(c, priv->row_font, conlog_text(priv->buf, i));

                if (w > widest)
                    widest = w;
            }
            /* Заголовок тоже задаёт ширину: иначе длинная метка
             * обрезалась бы по краю окна. */
            if (priv->title && *priv->title) {
                int w = cl_text_width(c, priv->title_font, priv->title);

                if (w > widest)
                    widest = w;
            }
            cairo_destroy(c);
            cairo_surface_destroy(s);
            priv->widest_cached = widest;
            priv->widest_valid = TRUE;
            priv->seen_cached = seen;
        }
        widest = priv->widest_cached;
        if (widest < 120)
            widest = 120;
        priv->width = CLAMP(widest + 20, CONLOG_MIN_WIDTH, 1200);
    } else {
        priv->widest_valid = FALSE;
    }
}

/* --------------------------------------------------------- прокрутка */

static gboolean cl_scroll_cb(GtkWidget *w, GdkEventScroll *ev, gpointer data)
{
    ConlogPriv *priv = data;
    int step = conlog_line_height(priv->row_font);
    int total = (int) conlog_len(priv->buf);
    int visible;
    gboolean moved = FALSE;

    if (!priv || !priv->plugin || !priv->plugin->win)
        return FALSE;
    visible = (priv->height - priv->first_row_y) / (step > 0 ? step : 1);
    if (visible < 1)
        visible = 1;

    switch (ev->direction) {
    case GDK_SCROLL_UP:
        priv->scroll_top -= 1;
        moved = TRUE;
        break;
    case GDK_SCROLL_DOWN:
        priv->scroll_top += 1;
        moved = TRUE;
        break;
    case GDK_SCROLL_SMOOTH: {
        double dy = 0;

        if (gdk_event_get_scroll_deltas((GdkEvent *) ev, NULL, &dy))
            priv->scroll_top += (dy > 0) ? 1 : -1;
        moved = TRUE;
        break;
    }
    default:
        break;
    }

    if (priv->scroll_top < 0)
        priv->scroll_top = 0;
    if (priv->scroll_top > total - 1 && total > 0)
        priv->scroll_top = total - 1;
    if (priv->scroll_top < 0)
        priv->scroll_top = 0;
    /* Начало — конец лога: прокрутка вверх прижата. */
    if (total == 0)
        priv->scroll_top = 0;

    if (moved) {
        priv->autoscroll = (priv->scroll_top == 0);
        cl_rebuild_cache(priv, priv->cache_width, priv->cache_height);
        xs_host_api()->invalidate(priv->plugin);
    }
    return TRUE;
}

/* ----------------------------------------------------------- тик */

static guint cl_tick(XsPlugin *p)
{
    ConlogPriv *priv = p ? p->priv : NULL;

    if (!priv || !p->win)
        return 0;
    /* Тик НЕ читает процесс: чтением занимается GIOChannel. Здесь только
     * перерисовка — и то лишь если размер окна изменился. */
    cl_recalc_size(priv);
    if (priv->cache_width != priv->width || priv->cache_height != priv->height) {
        priv->design_width = priv->width;
        priv->design_height = priv->height;
        cl_rebuild_cache(priv, priv->width, priv->height);
        return 0;
    }
    return 0;
}

static void cl_draw(XsPlugin *p, cairo_t *cr, int width, int height)
{
    ConlogPriv *priv = p ? p->priv : NULL;

    if (!priv)
        return;
    if (!priv->cache || priv->cache_width != width
                     || priv->cache_height != height) {
        if (priv->cache)
            cairo_surface_destroy(priv->cache);
        priv->cache = cl_render(priv, width, height);
        priv->cache_width = width;
        priv->cache_height = height;
    }
    cairo_set_source_surface(cr, priv->cache, 0, 0);
    cairo_paint(cr);
}

/* ------------------------------------------------------- конфиг */

static gboolean cl_parse_rgba(const char *text, gdouble rgba[4])
{
    gint r, g, b;
    gdouble a = 1.0;

    if (!text)
        return FALSE;
    if (sscanf(text, "rgba(%d,%d,%d,%lf)", &r, &g, &b, &a) == 4
     || sscanf(text, "rgba(%d,%d,%d)", &r, &g, &b) == 3) {
        rgba[0] = r / 255.0;
        rgba[1] = g / 255.0;
        rgba[2] = b / 255.0;
        rgba[3] = a > 1.0 ? 1.0 : (a < 0.0 ? 0.0 : a);
        return TRUE;
    }
    return FALSE;
}

static int cl_init(XsPlugin *p, GKeyFile *kf)
{
    ConlogPriv *priv;
    int x, y;
    static const gdouble label_def[4] = { 0.85, 0.85, 0.85, 1.0 };
    static const gdouble bg_def[4]    = { 0.098, 0.098, 0.098, 0.29 };
    static const gdouble border_def[4] = { 0.451, 0.451, 0.451, 1.0 };
    char *s;

    if (!p)
        return -1;
    priv = g_new0(ConlogPriv, 1);
    priv->plugin = p;
    priv->kf = kf;
    p->priv = priv;
    priv->buf = conlog_buffer_new(CONLOG_DEFAULT_LINES);

    if (!conlog_instances)
        conlog_instances = g_hash_table_new_full(g_str_hash, g_str_equal,
                                                 g_free, NULL);
    g_hash_table_replace(conlog_instances, g_strdup(p->name), priv);

    priv->command = xs_host_api()->conf_str(kf, p->name, "command",
                                            CONLOG_DEFAULT_COMMAND);
    priv->cwd = xs_host_api()->conf_str(kf, p->name, "cwd", NULL);
    if (!priv->cwd || !*priv->cwd) {
        g_free(priv->cwd);
        priv->cwd = g_strdup("/");
    }
    conlog_set_max_lines(priv->buf, (guint) xs_host_api()->conf_int(
        kf, p->name, "max_lines", CONLOG_DEFAULT_LINES));

    priv->width = CLAMP(xs_host_api()->conf_int(kf, p->name, "window_width",
                                                CONLOG_DEFAULT_WIDTH),
                        CONLOG_MIN_WIDTH, 1600);
    priv->height = CLAMP(xs_host_api()->conf_int(kf, p->name, "window_height",
                                                 CONLOG_DEFAULT_HEIGHT),
                         CONLOG_MIN_HEIGHT, 1200);
    priv->width_auto = xs_host_api()->conf_int(kf, p->name, "width_auto",
                                               -1) < 0;
    priv->height_auto = xs_host_api()->conf_int(kf, p->name, "height_auto",
                                                -1) < 0;
    priv->first_row_y = xs_host_api()->conf_int(kf, p->name, "first_row_y", 6);
    priv->line_step = CLAMP(xs_host_api()->conf_int(kf, p->name, "line_step",
                                                    14), 4, 200);
    priv->corner_radius = CLAMP(xs_host_api()->conf_int(kf, p->name,
                                                        "corner_radius", 3),
                                0, 40);
    priv->opacity = xs_host_api()->conf_dbl(kf, p->name, "opacity", 1.0);
    s = xs_host_api()->conf_str(kf, p->name, "title", "");
    priv->title = g_strdup(s ? s : "");
    priv->title_font = xs_host_api()->conf_str(kf, p->name, "title_font",
                                               CONLOG_DEFAULT_FONT);
    priv->row_font = xs_host_api()->conf_str(kf, p->name, "row_font",
                                             CONLOG_DEFAULT_FONT);
    /* Смещение метки. По умолчанию подтягиваем её к верху окна:
     * CONLOG_TITLE_TOP — это отступ рамки, а не «дыра» перед текстом. */
    priv->title_dx = CLAMP(xs_host_api()->conf_int(kf, p->name, "title_dx", 0),
                           -400, 400);
    priv->title_dy = CLAMP(xs_host_api()->conf_int(kf, p->name, "title_dy", -2),
                           -60, 60);
    priv->widest_valid = FALSE;   /* шрифт из конфига — считаем заново */
    priv->colorize = xs_host_api()->conf_int(kf, p->name, "colorize", 1) != 0;
    priv->wrap = xs_host_api()->conf_int(kf, p->name, "wrap", 0) != 0;
    priv->strip_ansi = xs_host_api()->conf_int(kf, p->name, "strip_ansi",
                                               1) != 0;
    /* keep_ansi сохраняет чужой цвет (SGR) вместо вырезания. Он
     * бессмысленен вместе со strip_ansi: тот вырезает последовательности
     * до отрисовки, и разбирать было бы уже нечего. */
    priv->keep_ansi = xs_host_api()->conf_int(kf, p->name, "keep_ansi",
                                              0) != 0
                   && !priv->strip_ansi;
    /* Буферу нужно то же решение: при keep_ansi строка хранится
     * с escape-последовательностями, иначе разбирать их нечем. */
    conlog_buffer_set_strip_ansi(priv->buf, !priv->keep_ansi);
    priv->autoscroll = xs_host_api()->conf_int(kf, p->name, "autoscroll",
                                               1) != 0;
    priv->scroll_top = 0;

    s = xs_host_api()->conf_str(kf, p->name, "title_color", NULL);
    if (!s || !cl_parse_rgba(s, priv->title_color))
        memcpy(priv->title_color, label_def, sizeof label_def);
    s = xs_host_api()->conf_str(kf, p->name, "row_color", NULL);
    if (!s || !cl_parse_rgba(s, priv->row_color))
        memcpy(priv->row_color, label_def, sizeof label_def);
    s = xs_host_api()->conf_str(kf, p->name, "background_color", NULL);
    if (!s || !cl_parse_rgba(s, priv->background_color))
        memcpy(priv->background_color, bg_def, sizeof bg_def);
    s = xs_host_api()->conf_str(kf, p->name, "border_color", NULL);
    if (!s || !cl_parse_rgba(s, priv->border_color))
        memcpy(priv->border_color, border_def, sizeof border_def);

    /* Цвета уровней. Неизвестное значение → дефолт по уровню, а не
     * молчаливый чёрный: иначе опечатка в конфиге гасила бы строки. */
    {
        static const char *keys[5] = {
            "color_normal", "color_warn", "color_error",
            "color_info", "color_debug"
        };
        gdouble fallback[5][4];

        conlog_level_color(CONLOG_LEVEL_NORMAL, fallback[0]);
        conlog_level_color(CONLOG_LEVEL_WARN, fallback[1]);
        conlog_level_color(CONLOG_LEVEL_ERROR, fallback[2]);
        conlog_level_color(CONLOG_LEVEL_INFO, fallback[3]);
        conlog_level_color(CONLOG_LEVEL_DEBUG, fallback[4]);
        for (int i = 0; i < 5; i++) {
            s = xs_host_api()->conf_str(kf, p->name, keys[i], NULL);
            if (!s || !cl_parse_rgba(s, priv->level_colors[i]))
                memcpy(priv->level_colors[i], fallback[i], sizeof fallback[i]);
        }
    }

    conlog_start(priv);
    priv->widest_valid = FALSE;   /* первый расчёт ширины — по факту */

    x = xs_host_api()->conf_int(kf, p->name, "x", 80);
    y = xs_host_api()->conf_int(kf, p->name, "y", 80);
    p->win = xs_host_api()->make_window(p, x, y, priv->width, priv->height);
    if (!p->win) {
        p->host->log("conlog: не создано окно");
        return -1;
    }
    /* Колесо мыши прокручивает лог. Без этого «прокручивается» из
     * требований пользователя было бы недостижимо. */
    g_signal_connect(p->win, "scroll-event", G_CALLBACK(cl_scroll_cb), priv);

    cl_recalc_size(priv);
    priv->design_width = priv->width;
    priv->design_height = priv->height;
    cl_rebuild_cache(priv, priv->width, priv->height);
    xs_host_api()->set_opacity(p, CLAMP(priv->opacity, 0.1, 1.0));
    /* Тик НЕ нужен часто: содержимое приходит событием от GIOChannel,
     * которое само вызывает перерисовку. Тик нужен только чтобы
     * поймать смену размера окна, а 500 мс на это — в 10 раз больше
     * нужного. При max_lines=200 старый интервал давал 200 измерений
     * текста через Pango дважды в секунду. */
    xs_host_api()->set_tick(p, 5000);
    return 0;
}

static void cl_shutdown(XsPlugin *p)
{
    ConlogPriv *priv = p ? p->priv : NULL;

    if (!priv)
        return;
    /* Запись УДАЛЯЕТСЯ: контролы диалога переживают properties() и держат
     * контекст с именем инстанса. Если запись останется, обработчик найдёт
     * по имени уже освобождённый priv. */
    if (conlog_instances)
        g_hash_table_remove(conlog_instances, p->name);
    conlog_stop(priv);
    if (priv->cache)
        cairo_surface_destroy(priv->cache);
    /* Кэш разбивки держит g_strdup на каждый прогон, поэтому его
     * нужно отдать до priv: без этого на каждый перезапуск демона
     * остаётся память на все строки, которые когда-то показывались. */
    conlog_runcache_free(priv->runcache);
    conlog_buffer_free(priv->buf);
    g_free(priv->command);
    g_free(priv->cwd);
    g_free(priv->title);
    g_free(priv->title_font);
    g_free(priv->row_font);
    g_free(priv);
    p->priv = NULL;
}

/* -------------------------------------------------- свойства */

typedef struct {
    ConlogPriv *priv;
    char       *instance_name;
    gboolean    building;
} ConlogCtx;

static ConlogPriv *cl_live_priv(ConlogCtx *ctx)
{
    if (!ctx || !conlog_instances)
        return NULL;
    return g_hash_table_lookup(conlog_instances, ctx->instance_name);
}

static void cl_save(ConlogPriv *priv)
{
    GKeyFile *kf = priv->kf;
    const char *n = priv->plugin->name;

    if (!kf || !priv->plugin)
        return;
    xs_host_api()->conf_set_str(kf, n, "command", priv->command);
    xs_host_api()->conf_set_int(kf, n, "max_lines",
                                (int) conlog_max_lines(priv->buf));
    xs_host_api()->conf_set_int(kf, n, "window_width", priv->width);
    xs_host_api()->conf_set_int(kf, n, "window_height", priv->height);
    xs_host_api()->conf_set_int(kf, n, "width_auto",
                                priv->width_auto ? -1 : priv->width);
    xs_host_api()->conf_set_int(kf, n, "height_auto",
                                priv->height_auto ? -1 : priv->height);
    xs_host_api()->conf_set_int(kf, n, "corner_radius", priv->corner_radius);
    xs_host_api()->conf_set_int(kf, n, "line_step", priv->line_step);
    xs_host_api()->conf_set_int(kf, n, "first_row_y", priv->first_row_y);
    xs_host_api()->conf_set_dbl(kf, n, "opacity", priv->opacity);
    xs_host_api()->conf_set_str(kf, n, "title", priv->title ? priv->title : "");
    xs_host_api()->conf_set_str(kf, n, "title_font", priv->title_font);
    xs_host_api()->conf_set_str(kf, n, "row_font", priv->row_font);
    /* Смещения метки: без них настройка из диалога терялась бы при
     * перезапуске демона, и окно возвращалось бы к метке по центру. */
    xs_host_api()->conf_set_int(kf, n, "title_dx", priv->title_dx);
    xs_host_api()->conf_set_int(kf, n, "title_dy", priv->title_dy);
    {
        char rgb[32];
        /* Цвета тоже должны переживать рестарт, иначе кнопка цвета
         * в диалоге работала бы только до перезапуска демона. */
        /* Формат обязан совпадать с cl_parse_rgba(): rgba(r,g,b,a).
         * Раньше здесь стояло «r,g,b,a», что парсер не читал — цвет
         * молча сбрасывался к дефолту при каждом пересчёте размера. */
        g_snprintf(rgb, sizeof rgb, "rgba(%d,%d,%d,%.3f)",
                   (int) (priv->title_color[0] * 255 + 0.5),
                   (int) (priv->title_color[1] * 255 + 0.5),
                   (int) (priv->title_color[2] * 255 + 0.5),
                   priv->title_color[3]);
        xs_host_api()->conf_set_str(kf, n, "title_color", rgb);
        g_snprintf(rgb, sizeof rgb, "rgba(%d,%d,%d,%.3f)",
                   (int) (priv->row_color[0] * 255 + 0.5),
                   (int) (priv->row_color[1] * 255 + 0.5),
                   (int) (priv->row_color[2] * 255 + 0.5),
                   priv->row_color[3]);
        xs_host_api()->conf_set_str(kf, n, "row_color", rgb);
    }
    xs_host_api()->conf_set_int(kf, n, "colorize", priv->colorize ? 1 : 0);
    xs_host_api()->conf_set_int(kf, n, "wrap", priv->wrap ? 1 : 0);
    xs_host_api()->conf_set_int(kf, n, "strip_ansi", priv->strip_ansi ? 1 : 0);
    xs_host_api()->conf_set_int(kf, n, "keep_ansi", priv->keep_ansi ? 1 : 0);
    xs_host_api()->conf_set_int(kf, n, "autoscroll", priv->autoscroll ? 1 : 0);

    /* Буфер вывода НЕ сохраняется. Он восстанавливается запуском
     * команды заново, а прежний код дописывал весь вывод ключом
     * last_output — на живом journalctl это раздувало конфиг
     * мегабайтами и делало его нечитаемым без единой пользы. */
    g_key_file_remove_key(kf, n, "last_output", NULL);
    xs_core_plugin_conf_flush(priv->plugin->name);
}

/* Заголовок окна: меняет только подпись и зону под неё. Команду НЕ
 * перезапускает — иначе правка метки обрывала бы поток журнала. */
static void cl_title_changed(GtkEditable *e, gpointer data)
{
    ConlogCtx *ctx = data;
    ConlogPriv *priv = cl_live_priv(ctx);
    const char *txt = gtk_entry_get_text(GTK_ENTRY(e));
    char *want = g_strdup(txt ? txt : "");

    if (!priv || ctx->building) {
        g_free(want);
        return;
    }
    if (g_strcmp0(want, priv->title) == 0) {
        g_free(want);
        return;
    }
    g_free(priv->title);
    priv->title = want;
    /* Заголовок влияет на ширину окна и на зону строк, поэтому оба
     * кэша сбрасываются, а размер пересчитывается до перерисовки —
     * иначе строки наезжали бы на метку до следующего тика. */
    priv->widest_valid = FALSE;
    /* Кэш разбивки тут ни при чём: он зависит от ШРИФТА СТРОК, а
     * сменилась только метка. Сбрасывать его — потеря кэша на ровном
     * месте. */
    cl_recalc_size(priv);
    cl_save(priv);
    cl_rebuild_cache(priv, priv->width, priv->height);
    if (priv->plugin->win) {
        xs_host_api()->resize(priv->plugin, priv->width, priv->height);
        xs_host_api()->invalidate(priv->plugin);
    }
}

static void cl_cmd_changed(GtkEditable *e, gpointer data)
{
    ConlogCtx *ctx = data;
    ConlogPriv *priv = cl_live_priv(ctx);
    const char *txt;
    char *want;

    if (!priv || ctx->building)
        return;
    txt = gtk_entry_get_text(GTK_ENTRY(e));
    want = g_strdup(txt ? txt : "");
    /* Перезапуск только при РЕАЛЬНОМ изменении: GTK шлёт сигнал и на
     * каждый символ, и перезапуск команды на «ж» создавал бы десятки
     * процессов в секунду. */
    if (g_strcmp0(want, priv->command) == 0) {
        g_free(want);
        return;
    }
    g_free(priv->command);
    priv->command = want;
    conlog_clear(priv->buf);
    priv->scroll_top = 0;
    conlog_start(priv);
    priv->widest_valid = FALSE;   /* первый расчёт ширины — по факту */
    cl_save(priv);
    cl_rebuild_cache(priv, priv->cache_width, priv->cache_height);
    if (priv->plugin->win)
        xs_host_api()->invalidate(priv->plugin);
}

/* Смена шрифта метки. Меняется только оформление заголовка, поэтому
 * команда не перезапускается, но высота зоны и кэш ширины строк
 * сбрасываются: иначе строки наезжали бы на метку до следующего тика. */
static void cl_font_set(GtkFontButton *fb, gpointer data)
{
    ConlogCtx *ctx = data;
    ConlogPriv *priv = cl_live_priv(ctx);
    const char *key = g_object_get_data(G_OBJECT(fb), "xs-key");
    const char *desc = gtk_font_chooser_get_font(GTK_FONT_CHOOSER(fb));
    char *want;

    if (!priv || ctx->building || !key || !desc)
        return;
    want = g_strdup(desc);
    if (!g_strcmp0(key, "title_font")) {
        g_free(priv->title_font);
        priv->title_font = want;
    } else if (!g_strcmp0(key, "row_font")) {
        g_free(priv->row_font);
        priv->row_font = want;
        /* Ширина прогонов посчитана прежним шрифтом — кэш
         * разбивки сбрасывается целиком. Точечная инвалидация по
         * строкам обошлась бы дороже полной пересборки: строк в
         * окне десятки, а пересчёт каждой всё равно лишний. */
        conlog_runcache_clear(priv->runcache);
    } else {
        g_free(want);
        return;
    }
    priv->widest_valid = FALSE;
    cl_recalc_size(priv);
    cl_save(priv);
    cl_rebuild_cache(priv, priv->width, priv->height);
    if (priv->plugin->win) {
        xs_host_api()->resize(priv->plugin, priv->width, priv->height);
        xs_host_api()->invalidate(priv->plugin);
    }
}

static void cl_int_changed(GtkSpinButton *spin, gpointer data)
{
    ConlogCtx *ctx = data;
    ConlogPriv *priv = cl_live_priv(ctx);
    const char *key;
    int v;

    if (!priv || ctx->building)
        return;
    key = g_object_get_data(G_OBJECT(spin), "xs-key");
    if (!key)
        return;
    v = gtk_spin_button_get_value_as_int(spin);
    if (!strcmp(key, "max_lines")) {
        conlog_set_max_lines(priv->buf, (guint) v);
    } else if (!strcmp(key, "window_width")) {
        priv->width = v;
        priv->width_auto = FALSE;
    } else if (!strcmp(key, "window_height")) {
        priv->height = v;
        priv->height_auto = FALSE;
    } else if (!strcmp(key, "corner_radius")) {
        priv->corner_radius = v;
    } else if (!strcmp(key, "line_step")) {
        priv->line_step = v;
    } else if (!strcmp(key, "first_row_y")) {
        priv->first_row_y = v;
    } else if (!strcmp(key, "title_dx")) {
        priv->title_dx = v;
    } else if (!strcmp(key, "title_dy")) {
        priv->title_dy = v;
    } else if (!strcmp(key, "opacity")) {
        priv->opacity = gtk_spin_button_get_value(spin);
        xs_host_api()->set_opacity(priv->plugin, CLAMP(priv->opacity, 0.1, 1.0));
    }
    cl_recalc_size(priv);
    priv->design_width = priv->width;
    priv->design_height = priv->height;
    cl_save(priv);
    cl_rebuild_cache(priv, priv->width, priv->height);
    if (priv->plugin->win)
        xs_host_api()->invalidate(priv->plugin);
}

/* Смена режима разбора ANSI требует перезапуска команды: escape в
 * уже полученных строках либо вырезан, либо сохранён, и пересборка
 * окна этого не исправит. */
static void cl_clear_buffer_and_restart(ConlogPriv *priv)
{
    if (!priv)
        return;
    conlog_clear(priv->buf);
    priv->scroll_top = 0;
    conlog_start(priv);
    priv->widest_valid = FALSE;
    conlog_runcache_clear(priv->runcache);
}

static void cl_check_changed(GtkToggleButton *cb, gpointer data)
{
    ConlogCtx *ctx = data;
    ConlogPriv *priv = cl_live_priv(ctx);
    const char *key;
    gboolean on;

    if (!priv || ctx->building)
        return;
    key = g_object_get_data(G_OBJECT(cb), "xs-key");
    if (!key)
        return;
    on = gtk_toggle_button_get_active(cb);
    if (!strcmp(key, "colorize"))
        priv->colorize = on;
    else if (!strcmp(key, "wrap"))
        priv->wrap = on;
    else if (!strcmp(key, "strip_ansi")) {
        priv->strip_ansi = on;
        if (on)
            priv->keep_ansi = FALSE;   /* обе опции вместе бессмысленны */
        conlog_buffer_set_strip_ansi(priv->buf, !priv->keep_ansi);
        cl_clear_buffer_and_restart(priv);
    } else if (!strcmp(key, "keep_ansi")) {
        priv->keep_ansi = on;
        if (on)
            priv->strip_ansi = FALSE;
        conlog_buffer_set_strip_ansi(priv->buf, !priv->keep_ansi);
        /* Буфер переключается на ходу: escape в уже полученных
         * строках либо вырезан, либо сохранён. Смена режима без
         * перезапуска команды оставила бы окно в неверном состоянии
         * до следующего обновления. */
        cl_clear_buffer_and_restart(priv);
    }
    else if (!strcmp(key, "autoscroll")) {
        priv->autoscroll = on;
        if (on)
            priv->scroll_top = 0;
    }
    cl_save(priv);
    cl_rebuild_cache(priv, priv->cache_width, priv->cache_height);
    if (priv->plugin->win)
        xs_host_api()->invalidate(priv->plugin);
}

static void cl_clear_clicked(GtkButton *b, gpointer data)
{
    ConlogCtx *ctx = data;
    ConlogPriv *priv = cl_live_priv(ctx);

    if (!priv)
        return;
    conlog_clear(priv->buf);
    priv->scroll_top = 0;
    cl_rebuild_cache(priv, priv->cache_width, priv->cache_height);
    if (priv->plugin->win)
        xs_host_api()->invalidate(priv->plugin);
}

static void cl_color_set(GtkColorButton *cb, gpointer data)
{
    ConlogCtx *ctx = data;
    ConlogPriv *priv = cl_live_priv(ctx);
    const char *key;
    GdkRGBA c;
    char buf[64];

    if (!priv || ctx->building)
        return;
    key = g_object_get_data(G_OBJECT(cb), "xs-key");
    if (!key)
        return;
    gtk_color_chooser_get_rgba(GTK_COLOR_CHOOSER(cb), &c);
    g_snprintf(buf, sizeof buf, "rgba(%d,%d,%d,%.3f)",
               (int) (c.red * 255), (int) (c.green * 255),
               (int) (c.blue * 255), c.alpha);
    g_key_file_set_string(priv->kf, priv->plugin->name, key, buf);
    /* Конфиг обновлён, но отрисовка берёт цвет из priv: без этой
     * строки кнопка цвета меняла файл, но картинка оставалась прежней
     * до перезапуска демона. */
    if (!g_strcmp0(key, "title_color")) {
        cl_parse_rgba(buf, priv->title_color);
    } else if (!g_strcmp0(key, "row_color")) {
        cl_parse_rgba(buf, priv->row_color);
    } else if (!g_strcmp0(key, "background_color")) {
        cl_parse_rgba(buf, priv->background_color);
    } else if (!g_strcmp0(key, "border_color")) {
        cl_parse_rgba(buf, priv->border_color);
    }
    xs_core_plugin_conf_flush(priv->plugin->name);
    cl_rebuild_cache(priv, priv->cache_width, priv->cache_height);
    if (priv->plugin->win)
        xs_host_api()->invalidate(priv->plugin);
}

static void cl_context_free(ConlogCtx *ctx)
{
    if (!ctx)
        return;
    g_free(ctx->instance_name);
    g_free(ctx);
}

static GtkWidget *cl_entry(ConlogCtx *ctx, const char *key, const char *value)
{
    GtkWidget *e = gtk_entry_new();

    gtk_entry_set_text(GTK_ENTRY(e), value ? value : "");
    gtk_widget_set_hexpand(e, TRUE);
    g_object_set_data_full(G_OBJECT(e), "xs-key", g_strdup(key), g_free);
    /* Обработчик выбирается по ключу: заголовок не должен
     * перезапускать команду, а команда — игнорировать заголовок. */
    g_signal_connect(e, "changed",
                     G_CALLBACK(g_strcmp0(key, "title") == 0
                                ? cl_title_changed : cl_cmd_changed), ctx);
    return e;
}

static GtkWidget *cl_check(ConlogCtx *ctx, const char *key, gboolean on)
{
    GtkWidget *cb = gtk_check_button_new();

    gtk_toggle_button_set_active(GTK_TOGGLE_BUTTON(cb), on);
    g_object_set_data_full(G_OBJECT(cb), "xs-key", g_strdup(key), g_free);
    g_signal_connect(cb, "toggled", G_CALLBACK(cl_check_changed), ctx);
    return cb;
}

static GtkWidget *cl_spin(ConlogCtx *ctx, const char *key, int value,
                          int min, int max)
{
    GtkWidget *sp = gtk_spin_button_new_with_range(min, max, 1);

    gtk_spin_button_set_value(GTK_SPIN_BUTTON(sp), value);
    gtk_widget_set_size_request(sp, 80, -1);
    g_object_set_data_full(G_OBJECT(sp), "xs-key", g_strdup(key), g_free);
    g_signal_connect(sp, "value-changed", G_CALLBACK(cl_int_changed), ctx);
    return sp;
}

static GtkWidget *cl_color(ConlogCtx *ctx, const char *key, const gdouble rgba[4])
{
    GtkWidget *cb = gtk_color_button_new();
    GdkRGBA c;

    c.red = rgba[0];
    c.green = rgba[1];
    c.blue = rgba[2];
    c.alpha = rgba[3];
    gtk_color_chooser_set_rgba(GTK_COLOR_CHOOSER(cb), &c);
    g_object_set_data_full(G_OBJECT(cb), "xs-key", g_strdup(key), g_free);
    g_signal_connect(cb, "color-set", G_CALLBACK(cl_color_set), ctx);
    return cb;
}

/* Перевод подписи здесь: у cl_row десятки вызовов с литералами, и
 * оборачивать каждый вручную бессмысленно. Ключи конфигов идут через
 * cl_keyed_*(), а не через label, поэтому конфиги не затрагиваются. */
static void cl_row(GtkWidget *grid, int row, const char *label, GtkWidget *w)
{
    GtkWidget *l = gtk_label_new(_(label));

    gtk_widget_set_halign(l, GTK_ALIGN_START);
    gtk_grid_attach(GTK_GRID(grid), l, 0, row, 1, 1);
    gtk_widget_set_hexpand(w, TRUE);
    gtk_grid_attach(GTK_GRID(grid), w, 1, row, 1, 1);
}

static void cl_connect(GtkWidget *w, ConlogCtx *ctx)
{
    /* Обработчики не должны срабатывать при построении таблицы: GTK
     * шлёт toggled/value-changed на каждый set_value. */
    ctx->building = TRUE;
    (void) w;
    ctx->building = FALSE;
}

static void cl_properties(XsPlugin *p, GtkNotebook *notebook)
{
    ConlogPriv *priv = p ? p->priv : NULL;
    ConlogCtx *ctx;
    GtkWidget *page, *scroller, *inner, *frame, *box, *btn;
    GtkWidget *grid;
    int row = 0;

    if (!priv)
        return;
    ctx = g_new0(ConlogCtx, 1);
    ctx->priv = priv;
    ctx->instance_name = g_strdup(p->name);

    page = gtk_box_new(GTK_ORIENTATION_VERTICAL, 8);
    scroller = gtk_scrolled_window_new(NULL, NULL);
    gtk_scrolled_window_set_policy(GTK_SCROLLED_WINDOW(scroller),
                                   GTK_POLICY_NEVER, GTK_POLICY_AUTOMATIC);
    gtk_scrolled_window_set_propagate_natural_width(
        GTK_SCROLLED_WINDOW(scroller), FALSE);
    gtk_scrolled_window_set_propagate_natural_height(
        GTK_SCROLLED_WINDOW(scroller), FALSE);
    inner = gtk_box_new(GTK_ORIENTATION_VERTICAL, 8);
    gtk_widget_set_margin_start(inner, 8);
    gtk_widget_set_margin_end(inner, 8);
    gtk_widget_set_margin_bottom(inner, 8);
    gtk_container_add(GTK_CONTAINER(scroller), inner);
    gtk_box_pack_start(GTK_BOX(page), scroller, TRUE, TRUE, 0);

    /* --- Команда --- */
    frame = gtk_frame_new(_("Command"));
    box = gtk_box_new(GTK_ORIENTATION_VERTICAL, 6);
    gtk_widget_set_margin_start(box, 8);
    gtk_widget_set_margin_end(box, 8);
    gtk_widget_set_margin_top(box, 8);
    gtk_widget_set_margin_bottom(box, 8);
    {
        /* Примеры команд НЕ переводятся: это буквальный текст, который
         * пользователь копирует в терминал. Переводить его нельзя.
         * Переводится только первая фраза-предложение. */
        GtkWidget *hint;
        char *tr = _("Arguments are passed to the process directly, "
                     "without a shell.\n");
        char *joined = g_strconcat(tr,
            "Примеры:\n"
            "  journalctl -f -n 20\n"
            "  dmesg -w\n"
            "  tail -F /var/log/messages\n"
            "  ip monitor link",
            NULL);

        hint = gtk_label_new(joined);
        g_free(joined);
        gtk_label_set_xalign(GTK_LABEL(hint), 0.0);
        gtk_box_pack_start(GTK_BOX(box), hint, FALSE, FALSE, 0);
    }
    /* Заголовок окна: метка пользователя сверху. Пустая строка — окно
     * без заголовка, зона не резервируется. */
    {
        GtkWidget *cap = gtk_label_new(_("Window title (optional)"));

        gtk_label_set_xalign(GTK_LABEL(cap), 0.0);
        gtk_box_pack_start(GTK_BOX(box), cap, FALSE, FALSE, 0);
    }
    gtk_box_pack_start(GTK_BOX(box),
                       cl_entry(ctx, "title", priv->title), FALSE, FALSE, 0);
    gtk_box_pack_start(GTK_BOX(box),
                       cl_entry(ctx, "command", priv->command), FALSE, FALSE, 0);
    btn = gtk_button_new_with_label(_("Clear output"));
    g_signal_connect(btn, "clicked", G_CALLBACK(cl_clear_clicked), ctx);
    gtk_box_pack_start(GTK_BOX(box), btn, FALSE, FALSE, 0);
    gtk_container_add(GTK_CONTAINER(frame), box);
    gtk_box_pack_start(GTK_BOX(inner), frame, FALSE, FALSE, 0);

    /* --- Вид: оформление метки окна ---
     * Шрифт, цвет и смещения метки задаются отдельно от строк журнала:
     * заголовок обычно крупнее или жирнее, и двигать его вправо нужно
     * независимо от текста. */
    frame = gtk_frame_new(_("Appearance: window label"));
    box = gtk_box_new(GTK_ORIENTATION_VERTICAL, 6);
    gtk_widget_set_margin_start(box, 8);
    gtk_widget_set_margin_end(box, 8);
    gtk_widget_set_margin_top(box, 8);
    gtk_widget_set_margin_bottom(box, 8);
    grid = gtk_grid_new();
    gtk_grid_set_column_spacing(GTK_GRID(grid), 8);
    gtk_grid_set_row_spacing(GTK_GRID(grid), 6);
    {
        GtkWidget *fb = gtk_font_button_new_with_font(priv->title_font);

        g_object_set_data_full(G_OBJECT(fb), "xs-key",
                               g_strdup("title_font"), g_free);
        g_signal_connect(fb, "font-set", G_CALLBACK(cl_font_set), ctx);
        cl_row(grid, 0, "Label font", fb);
    }
    cl_row(grid, 1, "Label colour", cl_color(ctx, "title_color", priv->title_color));
    /* Диапазоны совпадают с CLAMP в cl_init(), иначе значение из
     * конфига обрезалось бы самим spin-кнопкой. */
    cl_row(grid, 2, "Offset X",
           cl_spin(ctx, "title_dx", priv->title_dx, -400, 400));
    cl_row(grid, 3, "Offset Y",
           cl_spin(ctx, "title_dy", priv->title_dy, -60, 60));
    {
        GtkWidget *hint = gtk_label_new(_("Y offset: a negative value pulls the label up."));

        gtk_label_set_xalign(GTK_LABEL(hint), 0.0);
        gtk_grid_attach(GTK_GRID(grid), hint, 0, 4, 2, 1);
    }
    gtk_container_add(GTK_CONTAINER(box), grid);
    gtk_box_pack_start(GTK_BOX(inner), frame, FALSE, FALSE, 0);

    /* --- Поведение --- */
    frame = gtk_frame_new(_("Behaviour"));
    grid = gtk_grid_new();
    gtk_grid_set_column_spacing(GTK_GRID(grid), 8);
    gtk_grid_set_row_spacing(GTK_GRID(grid), 6);
    gtk_widget_set_margin_start(grid, 8);
    gtk_widget_set_margin_end(grid, 8);
    gtk_widget_set_margin_top(grid, 8);
    gtk_widget_set_margin_bottom(grid, 8);
    {
        GtkWidget *cb;

        cb = cl_check(ctx, "strip_ansi", priv->strip_ansi);
        cl_row(grid, row++, "Strip colours (ANSI)", cb);
        cb = cl_check(ctx, "keep_ansi", priv->keep_ansi);
        cl_row(grid, row++, "Keep output colours", cb);
        cb = cl_check(ctx, "colorize", priv->colorize);
        cl_row(grid, row++, "Per-level colours", cb);
        cb = cl_check(ctx, "autoscroll", priv->autoscroll);
        cl_row(grid, row++, "Follow the tail", cb);
        cl_row(grid, row++, "Lines in buffer",
               cl_spin(ctx, "max_lines", (int) conlog_max_lines(priv->buf),
                       10, CONLOG_MAX_LINES));
    }
    gtk_container_add(GTK_CONTAINER(frame), grid);
    gtk_box_pack_start(GTK_BOX(inner), frame, FALSE, FALSE, 0);

    /* --- Окно --- */
    frame = gtk_frame_new(_("Window"));
    grid = gtk_grid_new();
    gtk_grid_set_column_spacing(GTK_GRID(grid), 8);
    gtk_grid_set_row_spacing(GTK_GRID(grid), 6);
    gtk_widget_set_margin_start(grid, 8);
    gtk_widget_set_margin_end(grid, 8);
    gtk_widget_set_margin_top(grid, 8);
    gtk_widget_set_margin_bottom(grid, 8);
    cl_row(grid, row++, "Width",
           cl_spin(ctx, "window_width", priv->width, CONLOG_MIN_WIDTH, 1600));
    cl_row(grid, row++, "Height",
           cl_spin(ctx, "window_height", priv->height, CONLOG_MIN_HEIGHT, 1200));
    cl_row(grid, row++, "Corner rounding",
           cl_spin(ctx, "corner_radius", priv->corner_radius, 0, 40));
    cl_row(grid, row++, "First-line indent",
           cl_spin(ctx, "first_row_y", priv->first_row_y, 0, 60));
    cl_row(grid, row++, "Background",
           cl_color(ctx, "background_color", priv->background_color));
    cl_row(grid, row++, "Border",
           cl_color(ctx, "border_color", priv->border_color));
    gtk_container_add(GTK_CONTAINER(frame), grid);
    gtk_box_pack_start(GTK_BOX(inner), frame, FALSE, FALSE, 0);

    /* --- Цвета уровней --- */
    frame = gtk_frame_new(_("Level colours"));
    grid = gtk_grid_new();
    gtk_grid_set_column_spacing(GTK_GRID(grid), 8);
    gtk_grid_set_row_spacing(GTK_GRID(grid), 6);
    gtk_widget_set_margin_start(grid, 8);
    gtk_widget_set_margin_end(grid, 8);
    gtk_widget_set_margin_top(grid, 8);
    gtk_widget_set_margin_bottom(grid, 8);
    {
        /* Подписи уровней идут литералами прямо в вызовы cl_row.
         * Через массив names[] было удобнее, но xgettext массивы не
         * разбирает: ключ cl_row:3 видит только вызовы с литералом,
         * и пять строк в POT просто не появлялись. */
        static const char *keys[5] = {
            "color_normal", "color_warn", "color_error",
            "color_info", "color_debug"
        };

        cl_row(grid, 0, _("Normal"),
               cl_color(ctx, keys[0], priv->level_colors[0]));
        cl_row(grid, 1, _("Warning"),
               cl_color(ctx, keys[1], priv->level_colors[1]));
        cl_row(grid, 2, _("Error"),
               cl_color(ctx, keys[2], priv->level_colors[2]));
        cl_row(grid, 3, _("Information"),
               cl_color(ctx, keys[3], priv->level_colors[3]));
        cl_row(grid, 4, _("Debug"),
               cl_color(ctx, keys[4], priv->level_colors[4]));
    }
    gtk_container_add(GTK_CONTAINER(frame), grid);
    gtk_box_pack_start(GTK_BOX(inner), frame, FALSE, FALSE, 0);

    cl_connect(page, ctx);

    gtk_widget_set_size_request(page, 520, 620);
    gtk_notebook_append_page(notebook, page, gtk_label_new(_("Conlog")));
    /* Контекст живёт до конца окна: контролы переживают properties(). */
    g_object_set_data_full(G_OBJECT(page), "xs-cl-ctx", ctx,
                           (GDestroyNotify) cl_context_free);
}

static const XsPluginOps cl_ops = {
    .init = cl_init,
    .draw = cl_draw,
    .tick = cl_tick,
    .shutdown = cl_shutdown,
    .properties = cl_properties,
};

static XsPluginDesc cl_desc = {
    "conlog",
    XS_API_VERSION,
    &cl_ops,
    "Вывод команды: журнал, dmesg, следы",
    "kosmik2001",
    "1.0"
};

XS_PLUGIN_EXPORT(&cl_desc)
