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
#include "i18n.h"
#include "common.h"

#include <gtk/gtk.h>
#include <string.h>
#include <locale.h>
#include <stdlib.h>
#include <gio/gio.h>
#include <unistd.h>
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


/* Порядок определений: init ниже свойств, нужен прототип. */
static void cl_properties(XsPlugin *p, GtkNotebook *nb);

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
        priv->watch_id = 0;   /* источник сейчас умрёт: сбрось id,
                               * иначе следующий cl_stop() дёрнет
                               * g_source_remove() по мёртвому id */
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
    /* Порядок и владение здесь критичны.
     *
     * 1. Источник снимается ПЕРВЫМ: пока он жив, GIOChannel держит
     *    ссылку, и его удаление может произойти уже во время следующей
     *    итерации main loop.
     * 2. Канал закрывается через g_io_channel_shutdown(), а не просто
     *    unref: обёртка должна разорвать связь с дескриптором до того,
     *    как дескриптор закроет GSubprocess.
     * 3. in_stream НЕ unref-ится здесь: он принадлежит GSubprocess,
     *    и его finalize происходит асинхронно, когда отработает
     *    child-watch. Наш unref поверх чужой ссылки давал SIGSEGV в
     *    g_object_unref внутри g_main_context_iteration.
     *    Ссылка, полученная в cl_start, снимается там же сразу. */
    if (priv->watch_id) {
        g_source_remove(priv->watch_id);
        priv->watch_id = 0;
    }
    /* redraw_id здесь НЕ снимаем: cl_stop вызывается и из cl_start при
     * перезапуске команды, и таймер отрисовки переживает перезапуск —
     * он принадлежит applet-у, а не процессу. Снимает его cl_shutdown. */
    if (priv->chan) {
        g_io_channel_shutdown(priv->chan, FALSE, NULL);
        g_io_channel_unref(priv->chan);
        priv->chan = NULL;
    }
    priv->in_stream = NULL;
    if (priv->proc) {
        g_subprocess_force_exit(priv->proc);
        /* g_subprocess_newv() — transfer full, ссылка наша: unref
         * обязателен, иначе процесс и его каналы текут на каждом
         * перезапуске команды. */
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

        /* Ссылка BORROWED: g_subprocess_get_stdin_pipe() не даёт
         * transfer full, unref-ить её нельзя — это и убивало демон. */
        if (stdin_stream)
            g_output_stream_close(stdin_stream, NULL, NULL);
    }
    priv->in_stream = g_subprocess_get_stdout_pipe(priv->proc);
    if (!priv->in_stream) {
        /* Не оставляем процесс висеть: без читателя он упрётся в
         * заполненный pipe и заблокируется навсегда. */
        g_subprocess_force_exit(priv->proc);
        return;
    }
    /* Дескриптор берём через g_file_descriptor_based_get_fd: это публичный
     * интерфейс GIO, в отличие от GUnixInputStream из gio-unix-2.0.
     * Дублируем fd: канал не должен зависеть от жизни GSubprocess. */
    fd = g_file_descriptor_based_get_fd(
             G_FILE_DESCRIPTOR_BASED(priv->in_stream));
    if (fd >= 0) {
        int dup_fd = dup(fd);

        if (dup_fd >= 0) {
            priv->chan = g_io_channel_unix_new(dup_fd);
            g_io_channel_set_close_on_unref(priv->chan, TRUE);
        }
    }
    /* Ссылка на поток BORROWED и остаётся валидной, пока жив
     * GSubprocess: сохраняем для shutdown, unref-ить нельзя. */
    priv->in_stream = NULL;   /* дескриптор скопирован, ссылка не нужна */
    if (!priv->chan) {
        g_subprocess_force_exit(priv->proc);
        return;
    }
    g_io_channel_set_encoding(priv->chan, NULL, NULL);
    /* Буферизацию ВЫКЛЮЧАЕМ намеренно. При set_buffered(TRUE) GLib
     * читает во внутренний буфер размером G_IO_NICE_BUF_SIZE = 1024
     * байт, сколько бы мы ни просили, то есть read() идёт тысячебайтными
     * кусками и замысел "крупный кусок = 16K" не выполняется.
     * Замерено на чтении 256 КиБ: 268 read() с буферизацией против
     * 28 без неё. */
    g_io_channel_set_buffered(priv->chan, FALSE);
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

/* Пересчитать высоту зоны заголовка по реальным метрикам шрифта. Считает
 * один раз, а не на каждом кадре: pango в cl_draw меряет заново. */
static void cl_recalc_title_h(ConlogPriv *priv)
{
    cairo_surface_t *surf;
    cairo_t *cr;
    PangoLayout *l;
    PangoFontDescription *tf;
    int tw = 0, th = 0;

    priv->title_h = 0;
    if (!priv->title || !priv->title[0])
        return;
    surf = cairo_image_surface_create(CAIRO_FORMAT_ARGB32, 1, 1);
    cr = cairo_create(surf);
    l = pango_cairo_create_layout(cr);
    tf = cl_font(priv->title_font);
    pango_layout_set_font_description(l, tf);
    pango_layout_set_text(l, priv->title, -1);
    pango_layout_get_pixel_size(l, &tw, &th);
    priv->title_h = th + CONLOG_TITLE_TOP;
    pango_font_description_free(tf);
    g_object_unref(l);
    cairo_destroy(cr);
    cairo_surface_destroy(surf);
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

/* Разбор цвета БЕЗ sscanf и без strtod.
 *
 * sscanf("%d,...") в локали работает, но sscanf("%lf,...") — нет: в
 * ru_RU десятичный разделитель запятая, и на точке разбор ломается
 * (возвращал 2 из 4). strtod страдает тем же. А у демона
 * LC_ALL=ru_RU.UTF-8, так что это не теоретический случай.
 *
 * Поэтому разбираем целые числа вручную: компоненты rgba пишутся
 * всегда целыми байтами 0..255, то есть разбирать нужно только цифры
 * и запятые — а они от локали не зависят. */
static gboolean cl_u32_after(const char **p, double *out)
{
    const char *s = *p;
    double v = 0.0;
    gboolean any = FALSE;

    while (*s == ' ')
        s++;
    while (*s >= '0' && *s <= '9') {
        v = v * 10.0 + (*s - '0');
        s++;
        any = TRUE;
    }
    if (!any)
        return FALSE;
    *out = v;
    *p = s;
    return TRUE;
}

static void cl_rgba(ConlogPriv *priv, const char *key, const double d[4],
                    ConlogColor *out)
{
    char *v = xs_host_api()->conf_str(priv->kf, priv->plugin->name, key, NULL);
    const char *p;

    out->r = d[0]; out->g = d[1]; out->b = d[2]; out->a = d[3];
    if (!v || !v[0])
        return;

    p = v;
    if (strncmp(p, "rgba(", 5) == 0) {
        double r, g, b, a;

        p += 5;
        /* После каждого числа запятую нужно ПРОПУСТИТЬ: cl_u32_after()
         * останавливается на первой не-цифре и сам её не ест. Без
         * p++ следующий вызов видел запятую, возвращал FALSE, и весь
         * разбор падал на дефолт. */
        if (cl_u32_after(&p, &r) && *p++ == ',' &&
            cl_u32_after(&p, &g) && *p++ == ',' &&
            cl_u32_after(&p, &b) && *p++ == ',' &&
            cl_u32_after(&p, &a)) {
            out->r = r / 255.0;
            out->g = g / 255.0;
            out->b = b / 255.0;
            out->a = a / 255.0;
        }
    } else {
        /* Старый формат conlog_core: четыре float 0..1. Тут нужен
         * именно strtod, и он в ru_RU ждёт запятую — поэтому сначала
         * подменяем локаль на C на время разбора. */
        char *old = g_strdup(setlocale(LC_NUMERIC, NULL));
        double f[4];
        int ok = 1;

        setlocale(LC_NUMERIC, "C");
        for (int i = 0; i < 4 && ok; i++) {
            char *end = NULL;

            f[i] = strtod(p, &end);
            if (end == p) {
                ok = 0;
                break;
            }
            p = end;
            if (i < 3) {
                if (*p != ',') { ok = 0; break; }
                p++;
            }
        }
        setlocale(LC_NUMERIC, old ? old : "C");
        g_free(old);
        if (ok && f[0] >= 0.0 && f[0] <= 1.0 && f[1] >= 0.0 && f[1] <= 1.0 &&
            f[2] >= 0.0 && f[2] <= 1.0 && f[3] >= 0.0 && f[3] <= 1.0) {
            /* Проверка диапазона обязательна: в старых конфигах есть
             * шестикомпонентный мусор
             * 0,870588,0,866667,0,854902,1, где первые четыре — не
             * компоненты цвета. */
            out->r = f[0]; out->g = f[1]; out->b = f[2]; out->a = f[3];
        }
    }
    g_free(v);
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
    /* conf_str() уже возвращает СВЕЖУЮ копию (g_key_file_get_string),
     * поэтому g_strdup вокруг него просто терял строку. */
    priv->command = xs_host_api()->conf_str(kf, p->name, "command",
                                            CONLOG_DEFAULT_COMMAND);
    priv->title = xs_host_api()->conf_str(kf, p->name, "title", "");
    priv->row_font = xs_host_api()->conf_str(kf, p->name, "row_font",
                                            CONLOG_DEFAULT_FONT);
    priv->title_font = xs_host_api()->conf_str(kf, p->name, "title_font",
                                              CONLOG_DEFAULT_FONT);
    /* Клампим и при чтении: в конфиг из Properties писалось сырое
     * значение спиннера, так что ручная правка файла тоже обязана
     * давать разумные числа, а не окно в -5000 пикселей. */
    priv->line_step = CLAMP(xs_host_api()->conf_int(kf, p->name, "line_step", 0),
                            0, 60);
    priv->first_row_y = CLAMP(xs_host_api()->conf_int(kf, p->name,
                                                      "first_row_y", 6), 0, 60);
    priv->width = CLAMP(xs_host_api()->conf_int(kf, p->name, "window_width",
                                                CONLOG_DEFAULT_WIDTH),
                        120, 2000);
    priv->height = CLAMP(xs_host_api()->conf_int(kf, p->name, "window_height",
                                                 CONLOG_DEFAULT_HEIGHT),
                         60, 2000);
    cl_rgba(priv, "row_color", def_row, &priv->row_color);
    cl_rgba(priv, "background_color", def_bg, &priv->bg_color);
    cl_rgba(priv, "title_color", def_title, &priv->title_color);

    cl_recalc_title_h(priv);

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

/* ---------------------------------------------------------------- свойства */

/* Обработчики пишут значение в конфиг и перерисовывают окно. Конфиг
 * сбрасывать не нужно: applet читает ключи через xs_host_api()->conf_*,
 * а после flush я перечитываю изменённые поля здесь же. */
static void cl_save(XsPlugin *p)
{
    xs_core_plugin_conf_flush(p->name);
    if (p->win)
        gtk_widget_queue_draw(p->win);
}

static void cl_entry_changed(GtkEditable *e, gpointer data)
{
    XsPlugin *p = data;
    ConlogPriv *priv = p ? p->priv : NULL;
    const char *key = g_object_get_data(G_OBJECT(e), "xs-key");
    const char *text;

    if (!priv || !key)
        return;
    text = gtk_entry_get_text(GTK_ENTRY(e));
    if (strcmp(key, "command") == 0) {
        g_free(priv->command);
        priv->command = g_strdup(text ? text : "");
        /* Команда сменилась — перезапускаем процесс: иначе в окне так и
         * останется вывод старой команды. */
        cl_start(priv);
    } else if (strcmp(key, "title") == 0) {
        g_free(priv->title);
        priv->title = g_strdup(text ? text : "");
        cl_recalc_title_h(priv);
    } else if (strcmp(key, "row_font") == 0) {
        g_free(priv->row_font);
        priv->row_font = g_strdup(text ? text : CONLOG_DEFAULT_FONT);
    } else if (strcmp(key, "title_font") == 0) {
        g_free(priv->title_font);
        priv->title_font = g_strdup(text ? text : CONLOG_DEFAULT_FONT);
        cl_recalc_title_h(priv);
    }
    g_key_file_set_string(priv->kf, p->name, key, text ? text : "");
    cl_save(p);
}

static void cl_spin_changed(GtkSpinButton *spin, gpointer data)
{
    XsPlugin *p = data;
    ConlogPriv *priv = p ? p->priv : NULL;
    const char *key = g_object_get_data(G_OBJECT(spin), "xs-key");
    int v;

    if (!priv || !key)
        return;
    v = (int) gtk_spin_button_get_value(GTK_SPIN_BUTTON(spin));
    if (strcmp(key, "max_lines") == 0)
        priv->max_lines = (guint) CLAMP(v, 10, 10000);
    else if (strcmp(key, "line_step") == 0)
        priv->line_step = CLAMP(v, 0, 60);
    else if (strcmp(key, "first_row_y") == 0)
        priv->first_row_y = CLAMP(v, 0, 60);
    g_key_file_set_integer(priv->kf, p->name, key, v);
    cl_save(p);
}

static void cl_size_changed(GtkSpinButton *spin, gpointer data)
{
    XsPlugin *p = data;
    ConlogPriv *priv = p ? p->priv : NULL;
    const char *key = g_object_get_data(G_OBJECT(spin), "xs-key");
    int v;

    if (!priv || !key)
        return;
    v = (int) gtk_spin_button_get_value(GTK_SPIN_BUTTON(spin));
    if (strcmp(key, "window_width") == 0)
        priv->width = CLAMP(v, 120, 2000);
    else if (strcmp(key, "window_height") == 0)
        priv->height = CLAMP(v, 60, 2000);
    g_key_file_set_integer(priv->kf, p->name, key, v);
    if (p->win)
        gtk_widget_set_size_request(p->win, priv->width, priv->height);
    cl_save(p);
}

static void cl_color_set(GtkColorButton *btn, gpointer data)
{
    XsPlugin *p = data;
    ConlogPriv *priv = p ? p->priv : NULL;
    const char *key = g_object_get_data(G_OBJECT(btn), "xs-key");
    GdkRGBA c;
    char *s;

    if (!priv || !key)
        return;
    gtk_color_chooser_get_rgba(GTK_COLOR_CHOOSER(btn), &c);
    if (strcmp(key, "row_color") == 0) {
        priv->row_color.r = c.red; priv->row_color.g = c.green;
        priv->row_color.b = c.blue; priv->row_color.a = c.alpha;
    } else if (strcmp(key, "background_color") == 0) {
        priv->bg_color.r = c.red; priv->bg_color.g = c.green;
        priv->bg_color.b = c.blue; priv->bg_color.a = c.alpha;
    } else if (strcmp(key, "title_color") == 0) {
        priv->title_color.r = c.red; priv->title_color.g = c.green;
        priv->title_color.b = c.blue; priv->title_color.a = c.alpha;
    }
    /* Формат ДОЛЖЕН совпадать с тем, что читает cl_rgba():
     * rgba(r,g,b,a) в байтах 0..255. Раньше писалось "%g,%g,%g,%g",
     * а читалось "rgba(%d,...)" — форматы не совпадали, и выбранный
     * цвет молча терялся при рестарте: applet читал дефолт.
     * Именно в этом формате пишет и оригинальный conlog.c, так что
     * ключи из его конфигов читаются без правок. */
    /* alpha — ЦЕЛЫМ в байтах, как r/g/b. Это не косметика: у демона
     * LC_ALL=ru_RU.UTF-8, где десятичный разделитель — запятая, и
     * printf("%.3f", 1.0) печатает "1,000". Такое значение GKeyFile
     * сохранял как есть, и при следующем чтении sscanf видел
     * "rgba(0,255,0,1,000)" — пять компонент вместо четырёх, цвет
     * не распознавался и applet становился белым.
     * (Оригинальный conlog.c писал здесь %.3f и был тем же болен.) */
    s = g_strdup_printf("rgba(%d,%d,%d,%d)",
                       (int) (c.red * 255), (int) (c.green * 255),
                       (int) (c.blue * 255), (int) (c.alpha * 255));
    g_key_file_set_string(priv->kf, p->name, key, s);
    g_free(s);
    cl_save(p);
}

static void cl_font_set(GtkFontButton *btn, gpointer data)
{
    XsPlugin *p = data;
    ConlogPriv *priv = p ? p->priv : NULL;
    const char *key = g_object_get_data(G_OBJECT(btn), "xs-key");
    char *fname;   /* gtk_font_chooser_get_font() отдаёт transfer full,
                    * строка принадлежит нам и её надо освободить */

    if (!priv || !key)
        return;
    fname = gtk_font_chooser_get_font(GTK_FONT_CHOOSER(btn));
    if (!fname)
        return;
    if (strcmp(key, "row_font") == 0) {
        g_free(priv->row_font);
        priv->row_font = g_strdup(fname);
    } else if (strcmp(key, "title_font") == 0) {
        g_free(priv->title_font);
        priv->title_font = g_strdup(fname);
        cl_recalc_title_h(priv);
    }
    g_key_file_set_string(priv->kf, p->name, key, fname);
    g_free(fname);
    cl_save(p);
}

/* Страница настроек: описание сверху, как у других апплетов. */
static GtkWidget *cl_props_group(GtkNotebook *nb, const char *title,
                                 const char *info)
{
    GtkWidget *page = gtk_box_new(GTK_ORIENTATION_VERTICAL, 3);

    gtk_container_set_border_width(GTK_CONTAINER(page), 10);
    if (info && info[0]) {
        /* Описание и заголовок группы переводятся в хелпере: литералы
         * приходят из вызовов, их несколько. */
        GtkWidget *lbl = gtk_label_new(_(info));
        GtkWidget *sep;

        gtk_widget_set_halign(lbl, GTK_ALIGN_START);
        gtk_box_pack_start(GTK_BOX(page), lbl, FALSE, FALSE, 7);
        sep = gtk_separator_new(GTK_ORIENTATION_HORIZONTAL);
        gtk_box_pack_start(GTK_BOX(page), sep, FALSE, FALSE, 5);
    }
    gtk_notebook_append_page(nb, page, gtk_label_new(_(title)));
    return page;
}

static void cl_properties(XsPlugin *p, GtkNotebook *nb)
{
    ConlogPriv *priv = p ? p->priv : NULL;
    GtkWidget *page, *w;
    GtkBox *box;

    if (!priv)
        return;
    page = cl_props_group(nb, "Command",
                          "Показывать вывод команды. Буфер и разметка — "
                          "ниже; applet намеренно ничего не разбирает.");
    box = GTK_BOX(page);

    w = xs_prop_add_string(box, "Command",
                           "Команда, вывод которой показывать. Выполняется "
                           "через /bin/sh -c, поэтому работают кавычки, "
                           "перенаправление и переменные. Пример: "
                           "journalctl -f -n 15, dmesg --follow, "
                           "tail -F /var/log/messages.log",
                           priv->command);
    g_object_set_data_full(G_OBJECT(w), "xs-key", g_strdup("command"), g_free);
    g_signal_connect(w, "changed", G_CALLBACK(cl_entry_changed), p);

    w = xs_prop_add_string(box, "Title",
                           "Надпись над списком строк. Пусто — не рисуется "
                           "вовсе, вместе с разделителем.",
                           priv->title);
    g_object_set_data_full(G_OBJECT(w), "xs-key", g_strdup("title"), g_free);
    g_signal_connect(w, "changed", G_CALLBACK(cl_entry_changed), p);

    xs_prop_add_group_header(box, "Буфер и разметка");

    w = xs_prop_add_int(box, "Max lines",
                        "Сколько строк хранить. Более старые вытесняются. "
                        "Нагрузку это не ограничивает: applet всё равно "
                        "читает весь поток, предел приёма задан в коде.",
                        priv->max_lines, 10, 10000, 10);
    g_object_set_data_full(G_OBJECT(w), "xs-key", g_strdup("max_lines"),
                           g_free);
    g_signal_connect(w, "value-changed", G_CALLBACK(cl_spin_changed), p);

    w = xs_prop_add_int(box, "Line height",
                        "Высота строки в пикселях. 0 — считать по шрифту.",
                        priv->line_step, 0, 60, 1);
    g_object_set_data_full(G_OBJECT(w), "xs-key", g_strdup("line_step"),
                           g_free);
    g_signal_connect(w, "value-changed", G_CALLBACK(cl_spin_changed), p);

    w = xs_prop_add_int(box, "First row offset",
                        "Отступ от верхней рамки до зоны заголовка.",
                        priv->first_row_y, 0, 60, 1);
    g_object_set_data_full(G_OBJECT(w), "xs-key", g_strdup("first_row_y"),
                           g_free);
    g_signal_connect(w, "value-changed", G_CALLBACK(cl_spin_changed), p);

    xs_prop_add_group_header(box, "Шрифты и цвета");

    w = xs_prop_add_font(box, "Text font", "Шрифт строк вывода.",
                         priv->row_font);
    g_object_set_data_full(G_OBJECT(w), "xs-key", g_strdup("row_font"),
                           g_free);
    g_signal_connect(w, "font-set", G_CALLBACK(cl_font_set), p);

    w = xs_prop_add_font(box, "Title font", "Шрифт надписи.",
                         priv->title_font);
    g_object_set_data_full(G_OBJECT(w), "xs-key", g_strdup("title_font"),
                           g_free);
    g_signal_connect(w, "font-set", G_CALLBACK(cl_font_set), p);

    w = xs_prop_add_color(box, "Text color", "Цвет строк вывода.",
                          priv->row_color.r, priv->row_color.g,
                          priv->row_color.b, priv->row_color.a);
    g_object_set_data_full(G_OBJECT(w), "xs-key", g_strdup("row_color"),
                           g_free);
    g_signal_connect(w, "color-set", G_CALLBACK(cl_color_set), p);

    w = xs_prop_add_color(box, "Background", "Фон окна; alpha задаёт "
                          "прозрачность.",
                          priv->bg_color.r, priv->bg_color.g,
                          priv->bg_color.b, priv->bg_color.a);
    g_object_set_data_full(G_OBJECT(w), "xs-key",
                           g_strdup("background_color"), g_free);
    g_signal_connect(w, "color-set", G_CALLBACK(cl_color_set), p);

    w = xs_prop_add_color(box, "Title color", "Цвет надписи; им же красится "
                          "разделитель.",
                          priv->title_color.r, priv->title_color.g,
                          priv->title_color.b, priv->title_color.a);
    g_object_set_data_full(G_OBJECT(w), "xs-key", g_strdup("title_color"),
                           g_free);
    g_signal_connect(w, "color-set", G_CALLBACK(cl_color_set), p);

    xs_prop_add_group_header(box, "Размер окна");

    w = xs_prop_add_int(box, "Width", "Ширина окна в пикселях.",
                        priv->width, 120, 2000, 10);
    g_object_set_data_full(G_OBJECT(w), "xs-key", g_strdup("window_width"),
                           g_free);
    g_signal_connect(w, "value-changed", G_CALLBACK(cl_size_changed), p);

    w = xs_prop_add_int(box, "Height", "Высота окна в пикселях.",
                        priv->height, 60, 2000, 10);
    g_object_set_data_full(G_OBJECT(w), "xs-key", g_strdup("window_height"),
                           g_free);
    g_signal_connect(w, "value-changed", G_CALLBACK(cl_size_changed), p);

    gtk_widget_show_all(page);
}

/* ---------------------------------------------------------------- экспорт */

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
    "Вывод команды: журнал, dmesg (урезанная версия, см. CONLOG-FUNCTIONALITY.md)",
    "kosmik2001",
    "2.0-min"
};

XS_PLUGIN_EXPORT(&cl_desc)
