/* conlog_min.c — апплет «вывод команды», минимальная версия.
 *
 * ВАЖНО: это НЕ тот conlog, который был раньше. Урезанная версия
 * оставлена намеренно, после замера на предельном потоке.
 *
 * Что было: 1574 строки плюс conlog_core (1065) — разбор SGR с кэшем
 * прогонов, подсветка по уровню, перенос строк, авторазмер окна по
 * содержимому, прокрутка мышью, полный экран настроек.
 *
 * Что измерено на команде `cat -A /dev/urandom | grep -e '[a-z|A-Z|0-9]'`
 * (~290 000 строк/с):
 *
 *   CPU демона 90.6% — почти целое ядро, главный поток постоянно R;
 *   applet при этом обновлялся раз в несколько секунд;
 *   чтение шло 11 280 read/с по ~3967 байт, поток 40 МБ/с;
 *   стоимость обработки линейна: 23 мс процессорного времени на 1 МБ.
 *
 * Упирался не алгоритм, а объём данных: applet успевал прочитать и
 * разобрать 40 МБ/с. Оптимизация разбора это не чинит — нужен предел
 * приёма, он и стоит в основе этой версии.
 *
 * Возвращая функциональность, читай CONLOG-FUNCTIONALITY.md: там что
 * было, чем чинилось, какие цифры и в каком порядке возвращать.
 */
#include "xs_api.h"
#include "common.h"

#include <gtk/gtk.h>
#include <string.h>
#include <stdlib.h>
#include <gio/gio.h>
#include <gio/gfiledescriptorbased.h>

#define CONLOG_DEFAULT_COMMAND  "journalctl -f -n 20"
#define CONLOG_DEFAULT_WIDTH    380
#define CONLOG_DEFAULT_HEIGHT   180
#define CONLOG_DEFAULT_FONT     "Monospace 9"
#define CONLOG_MARGIN           6
#define CONLOG_ROWS_GAP        4   /* просвет под разделителем */
#define CONLOG_TITLE_TOP       3   /* отступ рамки до верха метки */

/* Предел приёма. Главное, ради чего applet урезан.
 *
 * CONLOG_MAX_LINES ограничивает только хранение: лишние строки всё
 * равно приходится прочитать. Нужен предел на ПРОЧИТАННЫЕ БАЙТЫ за один
 * вызов обработчика — тогда applet физически не может уйти в бесконечный
 * разбор, сколько бы данных ни лежало в pipe. */
#define CONLOG_MAX_LINES        200
#define CONLOG_MAX_READ_BYTES   (64 * 1024)

/* Один read. Крупный кусок полезен: pipe отдаёт всё готовое, и на потоке
 * в 40 МБ/с мелкие чтения съедали бы процессор на переходах. */
#define CONLOG_READ_CHUNK       (16 * 1024)

/* Как часто перерисовывать, мс. Раньше invalidate звался на каждое
 * чтение: при потоке это сотни вызовов в секунду на пустом деле,
 * потому что видны только CONLOG_MAX_LINES строк. */
#define CONLOG_REDRAW_MS        100

typedef struct {
    double r, g, b, a;
} ConlogColor;

typedef struct _ConlogPriv ConlogPriv;

struct _ConlogPriv {
    XsPlugin     *plugin;
    GKeyFile     *kf;

    char         *command;
    GSubprocess  *proc;
    GInputStream *in_stream;
    GIOChannel   *chan;
    guint         watch_id;
    guint         redraw_id;
    gboolean      running;
    gboolean      dirty;        /* есть новые строки, пора перерисовать */

    GPtrArray    *lines;        /* g_free-строки, от старых к новым */
    guint         max_lines;
    GString      *pending;      /* недописанная строка с прошлого чтения */

    int           scroll_top;   /* индекс первой видимой строки */
    int           width, height;
    int           line_step;
    char         *title;
    char         *row_font;
    char         *title_font;
    int           first_row_y;
    int           title_h;     /* 0 = заголовка нет, зона не резервируется */
    ConlogColor   row_color, bg_color, title_color;
};

/* ---------------------------------------------------------------- отрисовка */

static void cl_request_redraw(ConlogPriv *priv)
{
    if (!priv->dirty)
        return;
    priv->dirty = FALSE;
    if (priv->plugin && priv->plugin->win) {
        xs_host_api()->invalidate(priv->plugin);
        gtk_widget_queue_draw(priv->plugin->win);
    }
}

/* Таймер отрисовки: ОДИН на всё время жизни applet-а.
 *
 * Не «взводится при каждом чтении» — это была утечка таймеров, съедавшая
 * 32% CPU. cl_redraw_cb возвращала G_SOURCE_CONTINUE, но обнуляла
 * redraw_id, поэтому источник оставался жив, а следующий cl_on_io видел
 * redraw_id == 0 и добавлял ЕЩЁ ОДИН. На ~19 строк/с через минуту
 * накапливались сотни таймеров, и каждый дёргал invalidate с частотой
 * 10 Гц: суммарно десятки тысяч перерисовок в секунду.
 *
 * Теперь источник один и создан при init; он просто проверяет dirty. */
static gboolean cl_redraw_cb(gpointer data)
{
    ConlogPriv *priv = data;

    if (!priv)
        return G_SOURCE_REMOVE;
    cl_request_redraw(priv);
    return G_SOURCE_CONTINUE;
}

/* ---------------------------------------------------------------- чтение */

static void cl_push_line(ConlogPriv *priv, char *line)
{
    /* ВНИМАНИЕ: у массива стоит free func (g_free), поэтому
     * g_ptr_array_remove_index() освобождает элемент САМ. Свой g_free
     * перед ним давал double free — демон падал с «double free or
     * corruption» при первом же вытеснении старой строки. */
    if (priv->lines->len >= priv->max_lines)
        g_ptr_array_remove_index(priv->lines, 0);
    g_ptr_array_add(priv->lines, line);
    priv->dirty = TRUE;
}

/* Разобрать порцию на строки. Здесь нет SGR, уровней и переносов — только
 * поиск перевода строки. Это и есть весь «разбор» этой версии. */
static void cl_consume_chunk(ConlogPriv *priv, const char *data, gsize len)
{
    gsize start = 0, i;

    for (i = 0; i < len; i++) {
        if (data[i] != '\n')
            continue;
        g_string_append_len(priv->pending, data + start, i - start);
        /* \r убираем: вывод с терминала приносит его вместе с \n. */
        if (priv->pending->len > 0 &&
            priv->pending->str[priv->pending->len - 1] == '\r')
            g_string_truncate(priv->pending, priv->pending->len - 1);
        cl_push_line(priv, g_strdup(priv->pending->str));
        g_string_truncate(priv->pending, 0);
        start = i + 1;
    }
    if (start < len)
        g_string_append_len(priv->pending, data + start, len - start);
}

/* Прочитать доступное, но не больше CONLOG_MAX_READ_BYTES за вызов. */
static gboolean cl_drain(ConlogPriv *priv)
{
    char *buf;
    gsize total = 0;
    gboolean alive = FALSE;

    if (!priv->chan)
        return FALSE;
    buf = g_malloc(CONLOG_READ_CHUNK);
    while (total < CONLOG_MAX_READ_BYTES) {
        gsize want = CONLOG_READ_CHUNK;
        gsize got = 0;
        GIOStatus st;

        if (CONLOG_MAX_READ_BYTES - total < want)
            want = CONLOG_MAX_READ_BYTES - total;
        st = g_io_channel_read_chars(priv->chan, buf, want, &got, NULL);
        if (got > 0) {
            cl_consume_chunk(priv, buf, got);
            total += got;
            alive = TRUE;
            if (st != G_IO_STATUS_NORMAL)
                break;
            continue;
        }
        if (st == G_IO_STATUS_AGAIN) {
            alive = TRUE;
            break;
        }
        break;
    }
    g_free(buf);
    return alive;
}

static gboolean cl_on_io(GIOChannel *chan, GIOCondition cond, gpointer data)
{
    ConlogPriv *priv = data;

    (void) chan;
    if (!priv)
        return G_SOURCE_REMOVE;

    cl_drain(priv);

    if (cond & (G_IO_HUP | G_IO_ERR)) {
        /* В pipe может лежать непрочитанный хвост. */
        cl_drain(priv);
        if (priv->pending->len > 0) {
            cl_push_line(priv, g_strdup(priv->pending->str));
            g_string_truncate(priv->pending, 0);
        }
        priv->running = FALSE;
        cl_request_redraw(priv);
        return G_SOURCE_REMOVE;
    }

    /* Перерисовку делает постоянный таймер cl_redraw_cb: он смотрит
     * dirty и перерисовывает не чаще CONLOG_REDRAW_MS. */
    return G_SOURCE_CONTINUE;
}

/* ---------------------------------------------------------------- запуск */

static void cl_stop(ConlogPriv *priv)
{
    if (priv->watch_id) {
        g_source_remove(priv->watch_id);
        priv->watch_id = 0;
    }
    /* redraw_id здесь НЕ снимаем: cl_stop вызывается и из cl_start при
     * перезапуске команды, и таймер отрисовки переживает перезапуск —
     * он принадлежит applet-у, а не процессу. Снимает его cl_shutdown. */
    if (priv->chan) {
        g_io_channel_unref(priv->chan);
        priv->chan = NULL;
    }
    if (priv->in_stream) {
        g_object_unref(priv->in_stream);
        priv->in_stream = NULL;
    }
    if (priv->proc) {
        if (priv->running)
            g_subprocess_force_exit(priv->proc);
        g_object_unref(priv->proc);
        priv->proc = NULL;
    }
    priv->running = FALSE;
}

static void cl_start(ConlogPriv *priv)
{
    GError *err = NULL;
    char *argv[4];
    char *sh;
    int fd;

    cl_stop(priv);
    if (!priv->command || !priv->command[0])
        return;
    /* Через /bin/sh -c: пользователь пишет в Настройках именно то, что
     * ввёл бы в терминале — с кавычками, перенаправлением и переменными.
     * Свой разбор строки в argv поддержал бы половину синтаксиса
     * оболочки, а конфиги у нас уже написаны с перенаправлением. */
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
        return;
    }
    /* Команде нечего писать в stdin — закрываем, иначе она может ждать
     * ввода вечно. */
    {
        GOutputStream *stdin_stream = g_subprocess_get_stdin_pipe(priv->proc);

        if (stdin_stream) {
            g_output_stream_close(stdin_stream, NULL, NULL);
            g_object_unref(stdin_stream);
        }
    }
    priv->in_stream = g_subprocess_get_stdout_pipe(priv->proc);
    if (!priv->in_stream)
        return;
    /* Дескриптор берём через g_file_descriptor_based_get_fd: это публичный
     * интерфейс GIO, в отличие от GUnixInputStream из gio-unix-2.0. */
    fd = g_file_descriptor_based_get_fd(
             G_FILE_DESCRIPTOR_BASED(priv->in_stream));
    priv->chan = (fd >= 0) ? g_io_channel_unix_new(fd) : NULL;
    if (!priv->chan)
        return;
    g_io_channel_set_encoding(priv->chan, NULL, NULL);
    g_io_channel_set_buffered(priv->chan, TRUE);
    /* Неблокирующий режим обязателен: read() в обработчике main loop
     * повесил бы весь интерфейс. */
    g_io_channel_set_flags(priv->chan, G_IO_FLAG_NONBLOCK, NULL);
    priv->watch_id = g_io_add_watch(priv->chan,
                                    G_IO_IN | G_IO_HUP | G_IO_ERR,
                                    cl_on_io, priv);
    priv->running = TRUE;
}

/* ---------------------------------------------------------------- рисование */

static PangoFontDescription *cl_font(const char *spec)
{
    PangoFontDescription *d;

    if (spec && spec[0])
        d = pango_font_description_from_string(spec);
    else
        d = NULL;
    return d ? d : pango_font_description_from_string(CONLOG_DEFAULT_FONT);
}

static void cl_rounded(cairo_t *cr, double x, double y, double w, double h,
                       double r)
{
    cairo_new_sub_path(cr);
    cairo_arc(cr, x + w - r, y + r, r, -G_PI / 2, 0);
    cairo_arc(cr, x + w - r, y + h - r, r, 0, G_PI / 2);
    cairo_arc(cr, x + r, y + h - r, r, G_PI / 2, G_PI);
    cairo_arc(cr, x + r, y + r, r, G_PI, G_PI * 1.5);
    cairo_close_path(cr);
}

static void cl_draw(XsPlugin *p, cairo_t *cr, int w, int h)
{
    ConlogPriv *priv = p->priv;
    cairo_surface_t *surface;
    cairo_t *inner;
    PangoLayout *layout;
    PangoFontDescription *font;
    int step, y, first, count, i, rows_top;
    guint total;

    if (!priv)
        return;

    /* Рисуем в отдельную поверхность, чтобы не трогать поверхность,
     * которую дал applet-у кор. */
    surface = cairo_image_surface_create(CAIRO_FORMAT_ARGB32, w, h);
    inner = cairo_create(surface);
    cairo_set_operator(inner, CAIRO_OPERATOR_SOURCE);
    cairo_set_source_rgba(inner, 0, 0, 0, 0);
    cairo_paint(inner);
    cairo_set_operator(inner, CAIRO_OPERATOR_OVER);

    cl_rounded(inner, 0.5, 0.5, w - 1.0, h - 1.0, 3.0);
    cairo_set_source_rgba(inner, priv->bg_color.r, priv->bg_color.g,
                          priv->bg_color.b, priv->bg_color.a);
    cairo_fill_preserve(inner);
    cairo_set_source_rgba(inner, 0.45, 0.45, 0.45, 0.9);
    cairo_set_line_width(inner, 1.0);
    cairo_stroke(inner);

    layout = pango_cairo_create_layout(inner);
    font = cl_font(priv->row_font);
    pango_layout_set_font_description(layout, font);
    step = priv->line_step > 0
        ? priv->line_step
        : (pango_font_description_get_size(font) / 1024) + 3;
    pango_font_description_free(font);

    /* Зона заголовка: метка сверху, под ней разделитель, и только потом
     * строки. Заголовок рисуется ТОЛЬКО если он задан: пустой не должен
     * оставлять дыру. */
    if (priv->title_h > 0) {
        gdouble zone_top = priv->first_row_y + (gdouble) priv->title_h;
        gdouble divider = zone_top + 0.5;
        PangoFontDescription *tf;

        tf = cl_font(priv->title_font);
        pango_layout_set_font_description(layout, tf);
        pango_layout_set_text(layout, priv->title, -1);
        /* Метка позиционируется по ВЕРХУ зоны: pango ведёт себя так же,
         * поэтому никакой базовой линии тут не нужно. */
        cairo_set_source_rgba(inner, priv->title_color.r, priv->title_color.g,
                              priv->title_color.b, priv->title_color.a);
        cairo_move_to(inner, CONLOG_MARGIN,
                      priv->first_row_y + CONLOG_TITLE_TOP);
        pango_cairo_show_layout(inner, layout);
        pango_font_description_free(tf);
        /* Разделитель — в цвет метки. Контраст подобран замером
         * пикселей: при alpha 0.45 линия давала яркость 49 против фона
         * 19, то есть на глаз её не было видно совсем. При 0.75
         * получается около 170 — шапка читается как шапка. */
        cairo_set_source_rgba(inner, priv->title_color.r, priv->title_color.g,
                              priv->title_color.b, 0.75);
        cairo_set_line_width(inner, 1.0);
        cairo_move_to(inner, CONLOG_MARGIN, divider);
        cairo_line_to(inner, w - CONLOG_MARGIN, divider);
        cairo_stroke(inner);
    }

    /* Строки начинаются НИЖЕ разделителя, с просветом: иначе верхняя
     * строка прилипала бы к линии. */
    rows_top = priv->first_row_y + priv->title_h + CONLOG_ROWS_GAP;
    total = priv->lines->len;
    first = priv->scroll_top;
    if (first < 0)
        first = 0;
    if (total > 0 && first > (int) total - 1)
        first = (int) total - 1;
    count = (h - rows_top) / step;
    if (count < 0)
        count = 0;
    if (first + count > (int) total)
        count = (int) total - first;

    {
        /* cl_font() возвращает НОВЫЙ дескриптор: отдавать его прямо в
         * set_font_description нельзя, он утек бы на каждом кадре. */
        PangoFontDescription *rf = cl_font(priv->row_font);

        pango_layout_set_font_description(layout, rf);
        pango_font_description_free(rf);
    }
    cairo_set_source_rgba(inner, priv->row_color.r, priv->row_color.g,
                          priv->row_color.b, priv->row_color.a);
    y = rows_top;
    for (i = 0; i < count; i++) {
        const char *line = g_ptr_array_index(priv->lines, first + i);

        if (y > h + step)
            break;   /* строка ниже окна — дальше рисовать нечего */
        pango_layout_set_text(layout, line ? line : "", -1);
        cairo_move_to(inner, CONLOG_MARGIN, y);
        pango_cairo_show_layout(inner, layout);
        y += step;
    }

    g_object_unref(layout);
    cairo_destroy(inner);
    cairo_set_source_surface(cr, surface, 0, 0);
    cairo_paint(cr);
    cairo_surface_destroy(surface);
}

static guint cl_tick(XsPlugin *p)
{
    ConlogPriv *priv = p->priv;

    /* Страховка: если таймер отрисовки не сработал (например, после
     * G_IO_HUP), всё равно показываем накопленное. */
    if (priv)
        cl_request_redraw(priv);
    return 0;
}

static void cl_shutdown(XsPlugin *p)
{
    ConlogPriv *priv = p->priv;

    if (!priv)
        return;
    cl_stop(priv);
    if (priv->redraw_id) {
        g_source_remove(priv->redraw_id);
        priv->redraw_id = 0;
    }
    g_ptr_array_free(priv->lines, TRUE);
    g_string_free(priv->pending, TRUE);
    g_free(priv->command);
    g_free(priv->title);
    g_free(priv->row_font);
    g_free(priv->title_font);
    g_free(priv);
    p->priv = NULL;
}

/* ---------------------------------------------------------------- настройки */

static void cl_rgba(ConlogPriv *priv, const char *key, const double d[4],
                    ConlogColor *out)
{
    char *v = xs_host_api()->conf_str(priv->kf, priv->plugin->name, key, NULL);

    out->r = d[0]; out->g = d[1]; out->b = d[2]; out->a = d[3];
    if (v) {
        int r, g, b, a = 255;

        if (v[0] == 'r' && sscanf(v, "rgba(%d,%d,%d,%d)", &r, &g, &b, &a) == 4) {
            out->r = r / 255.0;
            out->g = g / 255.0;
            out->b = b / 255.0;
            out->a = a / 255.0;
        }
        g_free(v);
    }
}

static int cl_init(XsPlugin *p, GKeyFile *kf)
{
    ConlogPriv *priv;
    int x, y;
    const double def_row[4]   = { 0.85, 0.85, 0.85, 1.0 };
    const double def_bg[4]    = { 0.10, 0.10, 0.10, 0.29 };
    const double def_title[4] = { 0.85, 0.85, 0.85, 1.0 };

    if (!p || !kf)
        return -1;
    priv = g_new0(ConlogPriv, 1);
    priv->plugin = p;
    priv->kf = kf;
    priv->lines = g_ptr_array_new_with_free_func(g_free);
    priv->pending = g_string_new(NULL);

    priv->max_lines = (guint) xs_host_api()->conf_int(kf, p->name, "max_lines",
                                                      CONLOG_MAX_LINES);
    if (priv->max_lines == 0 || priv->max_lines > 10000)
        priv->max_lines = CONLOG_MAX_LINES;
    priv->command = g_strdup(xs_host_api()->conf_str(kf, p->name, "command",
                                                    CONLOG_DEFAULT_COMMAND));
    priv->title = g_strdup(xs_host_api()->conf_str(kf, p->name, "title", ""));
    priv->row_font = g_strdup(xs_host_api()->conf_str(kf, p->name, "row_font",
                                                     CONLOG_DEFAULT_FONT));
    priv->title_font = g_strdup(xs_host_api()->conf_str(kf, p->name,
                                                       "title_font",
                                                       CONLOG_DEFAULT_FONT));
    priv->line_step = xs_host_api()->conf_int(kf, p->name, "line_step", 0);
    priv->first_row_y = xs_host_api()->conf_int(kf, p->name, "first_row_y", 6);
    priv->width = xs_host_api()->conf_int(kf, p->name, "window_width",
                                          CONLOG_DEFAULT_WIDTH);
    priv->height = xs_host_api()->conf_int(kf, p->name, "window_height",
                                           CONLOG_DEFAULT_HEIGHT);
    cl_rgba(priv, "row_color", def_row, &priv->row_color);
    cl_rgba(priv, "background_color", def_bg, &priv->bg_color);
    cl_rgba(priv, "title_color", def_title, &priv->title_color);

    /* Высота зоны заголовка — по реальным метрикам шрифта, иначе
     * строки наезжали бы на метку. Считаем один раз: pango в cl_draw
     * делает это заново на каждом кадре. */
    if (priv->title && priv->title[0]) {
        cairo_surface_t *surf = cairo_image_surface_create(
            CAIRO_FORMAT_ARGB32, 1, 1);
        cairo_t *cr = cairo_create(surf);
        PangoLayout *l = pango_cairo_create_layout(cr);
        PangoFontDescription *tf = cl_font(priv->title_font);

        pango_layout_set_font_description(l, tf);
        pango_layout_set_text(l, priv->title, -1);
        {
            int tw = 0, th = 0;

            pango_layout_get_pixel_size(l, &tw, &th);
            priv->title_h = th + CONLOG_TITLE_TOP;
        }
        pango_font_description_free(tf);
        g_object_unref(l);
        cairo_destroy(cr);
        cairo_surface_destroy(surf);
    }

    p->priv = priv;
    /* Единственный таймер отрисовки на всё время жизни applet-а. */
    priv->redraw_id = g_timeout_add(CONLOG_REDRAW_MS, cl_redraw_cb, priv);
    cl_start(priv);

    /* Окно обязан создать плагин: демон после init проверяет p->win и
     * без него считает инстанс неудачным («init failed»). Плюс
     * обязательны set_opacity и set_tick — иначе окно не нарисуется. */
    x = xs_host_api()->conf_int(kf, p->name, "x", 80);
    y = xs_host_api()->conf_int(kf, p->name, "y", 80);
    p->win = xs_host_api()->make_window(p, x, y, priv->width, priv->height);
    if (!p->win) {
        p->host->log("conlog_min: не создано окно");
        cl_stop(priv);
        /* Таймер снимаем ДО освобождения priv: иначе он через секунду
         * сработает на освобождённую память. */
        if (priv->redraw_id) {
            g_source_remove(priv->redraw_id);
            priv->redraw_id = 0;
        }
        g_ptr_array_free(priv->lines, TRUE);
        g_string_free(priv->pending, TRUE);
        g_free(priv->command);
        g_free(priv->title);
        g_free(priv->row_font);
        g_free(priv->title_font);
        g_free(priv);
        p->priv = NULL;
        return -1;
    }
    xs_host_api()->set_opacity(p, 1.0);
    /* Тик нужен только как страховка отрисовки; содержимое приходит
     * событием от GIOChannel и таймером CONLOG_REDRAW_MS. */
    xs_host_api()->set_tick(p, 1000);
    return 0;
}

/* ---------------------------------------------------------------- экспорт */

static const XsPluginOps cl_ops = {
    .init = cl_init,
    .draw = cl_draw,
    .tick = cl_tick,
    .shutdown = cl_shutdown,
    .properties = NULL,
};

static XsPluginDesc cl_desc = {
    "conlog",
    XS_API_VERSION,
    &cl_ops,
    "Вывод команды: журнал, dmesg (урезанная версия, см. CONLOG-FUNCTIONALITY.md)",
    "kosmik2001",
    "2.0-min"
};

XS_PLUGIN_EXPORT(&cl_desc)
