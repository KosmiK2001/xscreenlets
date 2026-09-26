/* network_monitor.c — сетевой applet: скорости up/down по выбранному
 * интерфейсу с историей в графике.
 *
 * Каркас (окно, округление, подгонка координат, серии, Properties)
 * написан по образцу disk_monitor. Его исходники здесь не
 * используются и не меняются: это отдельный плагин со своим core.
 */
#include <gtk/gtk.h>
#include <gio/gio.h>
#include <limits.h>
#include <math.h>
#include <stdlib.h>
#include <string.h>
#include "xs_api.h"
#include "common.h"
#include "network_monitor_core.h"

#define NM_DEFAULT_WIDTH 420
#define NM_DEFAULT_HEIGHT 110
#define NM_DEFAULT_FONT "Sans 8"
#define NM_NETDEV "/proc/net/dev"
/* График занимает всё окно: подписи и значения рисуются поверх него,
 * поэтому для них ничего не резервируется. */
#define NM_GRAPH_TOP 0
#define NM_BORDER_PATH_INSET 0.5
/* Сколько значений усреднять: 1 = мгновенная скорость (поведение conky
 * по умолчанию), больше — сглаживает всплески. */
#define NM_RATE_SMOOTH_MIN 1
#define NM_RATE_SMOOTH_MAX 14
#define NM_MIN_WINDOW_WIDTH 160
#define NM_MIN_WINDOW_HEIGHT 60
/* Зазор между половинами в режиме Split: без него две заливки смыкаются
 * в одну фигуру и границы между download и upload не видно. */
#define NM_SPLIT_GAP 8

/* Текст рисуется поверх заливки графика и несёт тёмную тень. */
#define NM_TEXT_SHADOW_RADIUS 3
#define NM_TEXT_SHADOW_ALPHA 0.9

/* Где рисуется подпись серии: внутри графика (как в disk_monitor) или
 * над ним, на отдельной строке (как в conky с alignr). */
typedef enum {
    NM_LABEL_INSIDE = 0,
    NM_LABEL_OUTSIDE,
} NmLabelPlacement;

typedef struct {
    XsPlugin *plugin;
    GKeyFile *kf;
    char *ifname;          /* выбранный интерфейс */
    char *ip;              /* IPv4, NULL если адресов нет */
    char *font;
    char *label_font;
    char *series_font[NM_SERIES_MAX];
    char *series_label[NM_SERIES_MAX];
    int series_x[NM_SERIES_MAX], series_y[NM_SERIES_MAX];
    int series_label_x[NM_SERIES_MAX], series_label_y[NM_SERIES_MAX];
    NmLabelPlacement label_placement[NM_SERIES_MAX];
    int width, height;
    int corner_radius;
    int shape_radius, shape_w, shape_h;
    int design_width, design_height;
    guint update_ms;
    int rate_smooth;
    NmGraphMode graph_mode;
    /* Предел шкалы в КиБ/с: 0 = брать максимум истории. Ручная
     * константа, как в conky (95 для 100 Мбит, 47-59 для софта). */
    gint64 graph_max_kib;
    int header_x, header_y;   /* имя интерфейса + IP */
    int total_x, total_y;     /* суммарные байты */
    gdouble graph_bg[4], border[4], text_color[4];
    gdouble series_color[NM_SERIES_MAX][4];
    gdouble series_text_color[NM_SERIES_MAX][4];
    /* история: 0 = download (rx), 1 = upload (tx) */
    guint64 history[NM_SERIES_MAX][NM_HISTORY_MAX];
    guint head[NM_SERIES_MAX], count[NM_SERIES_MAX];
    guint64 prev_bytes[NM_SERIES_MAX];
    gboolean prev_valid;
    gint64 prev_time_us;
    guint64 current_rate[NM_SERIES_MAX];
    /* Сглаживание живёт в собственном кольце на 14 отсчётов, а не в
     * истории графика на 256: усреднение по всей истории забило бы
     * свежие значения. */
    NmSmoothRing smooth[NM_SERIES_MAX];
    guint64 total_bytes[NM_SERIES_MAX];
    gboolean sample_valid;
    cairo_surface_t *cache;
    int cache_width, cache_height;
} PrivData;

static const gdouble nm_graph_bg_default[4] = {0.02, 0.03, 0.05, 1.0};
static const gdouble nm_border_default[4] = {0.55, 0.58, 0.62, 1.0};
static const gdouble nm_text_default[4] = {1, 1, 1, 1};
/* download — голубой, upload — зелёный: те же цвета, что у read/write в
 * disk_monitor, чтобы два applet'а на экране читались одинаково. */
static const gdouble nm_rx_default[4] = {0.20, 0.75, 1.0, 1.0};
static const gdouble nm_tx_default[4] = {0.25, 0.85, 0.35, 1.0};

static void nm_shutdown(XsPlugin *p);
static void nm_rebuild(PrivData *priv);

/* Живые экземпляры для обработчиков Properties: имя плагина -> priv.
 * Объявлено здесь, потому что nm_shutdown (ниже) тоже пользуется таблицей. */
static GHashTable *nm_instances;

static char *nm_color_text(const gdouble rgba[4])
{
    return nm_format_rgba(rgba);
}

static void nm_read_color(PrivData *priv, const char *key,
                          const gdouble fallback[4], gdouble out[4])
{
    char *text = xs_host_api()->conf_str(priv->kf, priv->plugin->name, key,
                                         nm_color_text(fallback));
    if (!nm_parse_rgba(text, out))
        memcpy(out, fallback, sizeof(gdouble) * 4);
    g_free(text);
}

/* ------------------------------------------------------------------ */
/* Данные                                                              */
/* ------------------------------------------------------------------ */

static void nm_sample(PrivData *priv)
{
    gint64 now = g_get_monotonic_time();
    char *text = NULL;
    NmNetSample sample;
    gboolean have_rates;
    gint64 elapsed = priv->prev_valid
                         ? (now - priv->prev_time_us) : 0;

    priv->sample_valid = FALSE;
    if (!priv->ifname || !*priv->ifname)
        return;
    if (!g_file_get_contents(NM_NETDEV, &text, NULL, NULL))
        return;
    if (!nm_parse_netdev(text, priv->ifname, &sample) || !sample.valid) {
        g_free(text);
        return;
    }
    g_free(text);

    have_rates = priv->prev_valid && elapsed > 0;
    if (have_rates) {
        guint i;
        gboolean reset_baseline = FALSE;

        for (i = 0; i < NM_SERIES_MAX; i++) {
            guint64 now_bytes = (i == 0) ? sample.rx_bytes : sample.tx_bytes;
            gint64 rate = nm_rate_bytes_per_second(priv->prev_bytes[i],
                                                   now_bytes,
                                                   priv->prev_time_us, now);
            if (rate < 0) {
                /* Оборот 32-битного счётчика: baseline сбрасывается, но
                 * в кольцо кладётся 0, а не 4 гигабайта. */
                reset_baseline = TRUE;
                priv->current_rate[i] = 0;
                nm_push_history(priv->history[i], &priv->head[i],
                                &priv->count[i], 0, now);
            } else {
                priv->current_rate[i] = (guint64)rate;
                nm_push_history(priv->history[i], &priv->head[i],
                                &priv->count[i], priv->current_rate[i], now);
            }
            nm_rate_smooth_push(&priv->smooth[i], rate);
            priv->total_bytes[i] = now_bytes;
            priv->prev_bytes[i] = now_bytes;
        }
        priv->sample_valid = TRUE;
        if (reset_baseline) {
            /* Оборот на одном направлении обнуляет baseline обоих: они
             * приходят из одной строки /proc/net/dev. */
            priv->prev_bytes[0] = sample.rx_bytes;
            priv->prev_bytes[1] = sample.tx_bytes;
        }
        if (priv->rate_smooth > 1) {
            guint i;
            for (i = 0; i < NM_SERIES_MAX; i++)
                priv->current_rate[i] = (guint64)
                    nm_rate_smoothed(&priv->smooth[i],
                                     (guint) priv->rate_smooth);
        }
    } else {
        /* Первое чтение только создаёт baseline: класть его в историю
         * нельзя, иначе первый график начинается с полной высоты. */
        priv->prev_bytes[0] = sample.rx_bytes;
        priv->prev_bytes[1] = sample.tx_bytes;
        priv->total_bytes[0] = sample.rx_bytes;
        priv->total_bytes[1] = sample.tx_bytes;
    }
    priv->prev_time_us = now;
    priv->prev_valid = TRUE;
}

/* ------------------------------------------------------------------ */
/* Отрисовка                                                           */
/* ------------------------------------------------------------------ */

static void nm_show_text(cairo_t *cr, PangoLayout *layout,
                         const char *font_name, int x, int y,
                         const char *text, const gdouble color[4],
                         int width, int height)
{
    PangoFontDescription *font = pango_font_description_from_string(font_name);
    int text_w = 0;

    pango_layout_set_font_description(layout, font);
    pango_font_description_free(font);
    pango_layout_set_text(layout, text, -1);
    pango_layout_get_pixel_size(layout, &text_w, NULL);
    /* Подгоняем по ОРЕОЛУ, а не по глифам: тень идёт на
     * NM_TEXT_SHADOW_RADIUS px во все стороны, и подгонка по text_w
     * оставляет значение формально внутри окна, когда его ореол уже
     * пересекает рамку. */
    x = nm_fit_text_coordinate(x, text_w + 2 * NM_TEXT_SHADOW_RADIUS, width);
    if (height > 0 && y > height - 1 - NM_TEXT_SHADOW_RADIUS)
        y = height - 1 - NM_TEXT_SHADOW_RADIUS;
    if (y < NM_TEXT_SHADOW_RADIUS)
        y = NM_TEXT_SHADOW_RADIUS;
    {
        static const int offsets[8][2] = {{-1, -1}, {0, -1}, {1, -1}, {-1, 0},
                                          {1, 0}, {-1, 1}, {0, 1}, {1, 1}};
        int i;
        cairo_set_source_rgba(cr, 0.0, 0.0, 0.0, NM_TEXT_SHADOW_ALPHA);
        for (i = 0; i < 8; i++) {
            cairo_move_to(cr, x + offsets[i][0] * NM_TEXT_SHADOW_RADIUS,
                          y + offsets[i][1] * NM_TEXT_SHADOW_RADIUS);
            pango_cairo_show_layout(cr, layout);
        }
    }
    cairo_set_source_rgba(cr, color[0], color[1], color[2], color[3]);
    cairo_move_to(cr, x, y);
    pango_cairo_show_layout(cr, layout);
}

static void nm_draw_series(cairo_t *cr, const guint64 *history, guint head,
                           guint count, guint columns, int plot_width,
                           guint64 scale_max, double x, double y, int height,
                           const gdouble color[4])
{
    guint i;
    double origin = x + nm_history_origin_x(plot_width, columns);
    double baseline = y + height;
    gdouble fill[4];

    nm_fill_rgba(color, color[3], fill);
    cairo_set_source_rgba(cr, fill[0], fill[1], fill[2], fill[3]);
    if (columns == 0)
        return;
    /* Область замыкается двумя вертикалями и базовой линией: один
     * cairo_close_path соединил бы последнюю точку с первой и прочертил
     * диагональ через весь график. */
    {
        double right_x = origin + (double)(columns - 1);
        double first_y = y + height - height * (double)
            MIN(nm_history_value(history, head, count, 0), scale_max) /
            (double)scale_max;
        cairo_new_sub_path(cr);
        cairo_move_to(cr, right_x, baseline);
        cairo_line_to(cr, right_x, first_y);
        for (i = 0; i < columns; i++) {
            guint64 value = nm_history_value(history, head, count, i);
            double point_x = origin + (double)(columns - 1 - i);
            double point_y = y + height - height * (double)
                MIN(value, scale_max) / (double)scale_max;
            cairo_line_to(cr, point_x, point_y);
        }
        cairo_line_to(cr, origin, baseline);
        cairo_close_path(cr);
    }
    cairo_fill(cr);
}

/* Округлённый контур окна как путь (не заливка). */
static void nm_rounded_path(cairo_t *cr, int width, int height, int radius,
                            double border_inset)
{
    const double inset = 1.0;
    double w = width - 2 * inset, h = height - 2 * inset;
    double r = nm_corner_radius_value(radius);

    if (w <= 0 || h <= 0) {
        cairo_rectangle(cr, 0, 0, width, height);
        return;
    }
    if (!nm_corner_radius_is_rounded(r)) {
        cairo_rectangle(cr, inset, inset, w, h);
        return;
    }
    r = MIN(r, MIN(w, h) / 2.0);
    if (r > border_inset)
        r -= border_inset;
    else
        r = 0.0;
    cairo_new_sub_path(cr);
    cairo_arc(cr, inset + w - r, inset + r, r, -G_PI / 2.0, 0.0);
    cairo_arc(cr, inset + w - r, inset + h - r, r, 0.0, G_PI / 2.0);
    cairo_arc(cr, inset + r, inset + h - r, r, G_PI / 2.0, G_PI);
    cairo_arc(cr, inset + r, inset + r, r, G_PI, 1.5 * G_PI);
    cairo_close_path(cr);
}

static cairo_surface_t *nm_render(PrivData *priv, int width, int height)
{
    cairo_surface_t *surface = cairo_image_surface_create(CAIRO_FORMAT_ARGB32,
                                                          width, height);
    cairo_t *cr = cairo_create(surface);
    PangoLayout *layout = pango_cairo_create_layout(cr);
    guint columns;
    guint64 scale_max[NM_SERIES_MAX] = {1, 1};
    guint64 observed_max = 0;
    gboolean split = (priv->graph_mode == NM_GRAPH_SPLIT);
    guint i;
    int graph_y, graph_h;
    int dw = priv->design_width > 0 ? priv->design_width : width;
    int dh = priv->design_height > 0 ? priv->design_height : height;
    char *rate_text[NM_SERIES_MAX];
    char *header_text, *total_text;

    pango_layout_set_font_description(layout, NULL);
    {
        const double radius = nm_corner_radius_value(priv->corner_radius);
        if (nm_corner_radius_is_rounded(radius)) {
            double r = MIN(radius, MIN(width, height) / 2.0);
            cairo_new_sub_path(cr);
            cairo_arc(cr, width - r, r, r, -G_PI / 2.0, 0.0);
            cairo_arc(cr, width - r, height - r, r, 0.0, G_PI / 2.0);
            cairo_arc(cr, r, height - r, r, G_PI / 2.0, G_PI);
            cairo_arc(cr, r, r, r, G_PI, 1.5 * G_PI);
            cairo_close_path(cr);
            cairo_clip(cr);
        }
    }
    cairo_set_operator(cr, CAIRO_OPERATOR_SOURCE);
    cairo_set_source_rgba(cr, 0.0, 0.0, 0.0, 0.0);
    cairo_paint(cr);
    cairo_set_operator(cr, CAIRO_OPERATOR_OVER);

    columns = nm_history_columns(width - 4,
                                 MAX(priv->count[0], priv->count[1]));
    /* Предел шкалы ОДИН на обе серии, в любом режиме. Общая шкала — это
     * и есть смысл графика: 11 Мбайт должны выглядеть выше, чем 100
     * килобайт. Раздельные максимумы (как у conky) превращали бы upload
     * в размазанную полосу у нижнего края, и сравнивать направления
     * на глаз было бы нельзя. Порядок: ручная константа из настроек
     * (0 = брать максимум наблюдённой истории). */
    for (i = 0; i < columns; i++) {
        guint64 rx = nm_history_value(priv->history[0], priv->head[0],
                                      priv->count[0], i);
        guint64 tx = nm_history_value(priv->history[1], priv->head[1],
                                      priv->count[1], i);
        if (rx > observed_max) observed_max = rx;
        if (tx > observed_max) observed_max = tx;
    }
    {
        guint64 cap = priv->graph_max_kib > 0
                          ? (guint64) priv->graph_max_kib * 1024
                          : MAX((guint64)1, observed_max);
        for (i = 0; i < NM_SERIES_MAX; i++)
            scale_max[i] = cap;
    }

    graph_y = NM_GRAPH_TOP;
    graph_h = MAX(1, height - graph_y - 2);

    cairo_save(cr);
    nm_rounded_path(cr, width, height, priv->corner_radius, 0.0);
    cairo_set_source_rgba(cr, priv->graph_bg[0], priv->graph_bg[1],
                          priv->graph_bg[2], priv->graph_bg[3]);
    cairo_fill(cr);
    cairo_restore(cr);
    cairo_save(cr);
    cairo_rectangle(cr, 2, graph_y + 1, width - 4, graph_h - 2);
    cairo_clip(cr);
    nm_rounded_path(cr, width, height, priv->corner_radius, 0.0);
    cairo_clip(cr);
    if (split) {
        /* Две половины с зазором между ними. Без зазора заливки смыкались
         * в одну пешку, и было не видно, где кончается download и
         * начинается upload, — а это ровно то, ради чего режим раздельный.
         * Зазор занимает NM_SPLIT_GAP пикселей и рисуется разделителем в
         * цвет рамки. */
        const int gap = MIN(NM_SPLIT_GAP, MAX(0, (width - 4) / 4));
        const int plot_w = MAX(1, (width - 4 - gap) / 2);
        const double left_x = 2.0;
        const double right_x = left_x + plot_w + gap;

        nm_draw_series(cr, priv->history[0], priv->head[0], priv->count[0],
                       nm_history_columns(plot_w, priv->count[0]), plot_w,
                       scale_max[0], left_x, graph_y + 1.0, graph_h - 2.0,
                       priv->series_color[0]);
        nm_draw_series(cr, priv->history[1], priv->head[1], priv->count[1],
                       nm_history_columns(plot_w, priv->count[1]), plot_w,
                       scale_max[1], right_x, graph_y + 1.0, graph_h - 2.0,
                       priv->series_color[1]);
        if (gap > 0) {
            double mid = left_x + plot_w + gap / 2.0;

            cairo_set_source_rgba(cr, priv->border[0], priv->border[1],
                                  priv->border[2], priv->border[3]);
            cairo_set_line_width(cr, 1.0);
            cairo_move_to(cr, floor(mid), graph_y + 1.0);
            cairo_line_to(cr, floor(mid), graph_y + graph_h - 1.0);
            cairo_stroke(cr);
        }
    } else {
        /* Общий график: обе серии в одном поле, порядок по сумме, чтобы
         * более крупная ушла на задний план и не закрыла мелкую. */
        guint64 sum[NM_SERIES_MAX] = {0, 0};
        for (i = 0; i < columns; i++) {
            sum[0] += nm_history_value(priv->history[0], priv->head[0],
                                       priv->count[0], i);
            sum[1] += nm_history_value(priv->history[1], priv->head[1],
                                       priv->count[1], i);
        }
        if (sum[0] >= sum[1]) {
            nm_draw_series(cr, priv->history[0], priv->head[0], priv->count[0],
                           columns, width - 4, scale_max[0], 2.0,
                           graph_y + 1.0, graph_h - 2.0, priv->series_color[0]);
            nm_draw_series(cr, priv->history[1], priv->head[1], priv->count[1],
                           columns, width - 4, scale_max[1], 2.0,
                           graph_y + 1.0, graph_h - 2.0, priv->series_color[1]);
        } else {
            nm_draw_series(cr, priv->history[1], priv->head[1], priv->count[1],
                           columns, width - 4, scale_max[1], 2.0,
                           graph_y + 1.0, graph_h - 2.0, priv->series_color[1]);
            nm_draw_series(cr, priv->history[0], priv->head[0], priv->count[0],
                           columns, width - 4, scale_max[0], 2.0,
                           graph_y + 1.0, graph_h - 2.0, priv->series_color[0]);
        }
    }
    cairo_restore(cr);
    cairo_set_source_rgba(cr, priv->border[0], priv->border[1],
                          priv->border[2], priv->border[3]);
    cairo_set_line_width(cr, 1.0);
    cairo_save(cr);
    nm_rounded_path(cr, width, height, priv->corner_radius,
                    NM_BORDER_PATH_INSET);
    cairo_stroke(cr);
    cairo_restore(cr);

    /* Текст рисуется поверх графика и клипается тем же контуром, что и
     * рамка: глиф у угла просто не рисуется, без ручных границ. */
    cairo_save(cr);
    nm_rounded_path(cr, width, height, priv->corner_radius, 0.0);
    cairo_clip(cr);

    for (i = 0; i < NM_SERIES_MAX; i++) {
        rate_text[i] = priv->sample_valid
                           ? nm_format_rate(priv->current_rate[i])
                           : g_strdup("N/A");
    }
    /* Шапка: имя интерфейса и адрес. Адреса может не быть (ifb, tun) —
     * тогда показываем только имя, без пустого хвоста. */
    if (priv->ip && *priv->ip)
        header_text = g_strdup_printf("%s: %s", priv->ifname, priv->ip);
    else
        header_text = g_strdup(priv->ifname ? priv->ifname : "");
    total_text = g_strdup_printf("Total: %s / %s",
                                 nm_format_bytes(priv->total_bytes[0]),
                                 nm_format_bytes(priv->total_bytes[1]));

    for (i = 0; i < NM_SERIES_MAX; i++) {
        int sx = nm_scale_position(priv->series_x[i], dw, width, width - 1);
        int sy = nm_scale_position(priv->series_y[i], dh, height, height - 1);
        int lx = nm_scale_position(priv->series_label_x[i], dw, width,
                                   width - 1);
        int ly = nm_scale_position(priv->series_label_y[i], dh, height,
                                   height - 1);
        char *full;

        if (priv->label_placement[i] == NM_LABEL_INSIDE) {
            full = g_strdup_printf("%s: %s", priv->series_label[i],
                                   rate_text[i]);
        } else {
            /* Снаружи графика: подпись над значением, как alignr в conky */
            nm_show_text(cr, layout, priv->series_font[i], lx, ly,
                         priv->series_label[i], priv->series_text_color[i],
                         width, height);
            full = g_strdup(rate_text[i]);
        }
        nm_show_text(cr, layout, priv->series_font[i], sx, sy, full,
                     priv->series_text_color[i], width, height);
        g_free(full);
    }
    nm_show_text(cr, layout, priv->label_font,
                 nm_scale_position(priv->header_x, dw, width, width - 1),
                 nm_scale_position(priv->header_y, dh, height, height - 1),
                 header_text, priv->text_color, width, height);
    nm_show_text(cr, layout, priv->label_font,
                 nm_scale_position(priv->total_x, dw, width, width - 1),
                 nm_scale_position(priv->total_y, dh, height, height - 1),
                 total_text, priv->text_color, width, height);
    g_free(header_text);
    g_free(total_text);
    for (i = 0; i < NM_SERIES_MAX; i++)
        g_free(rate_text[i]);
    cairo_restore(cr);
    g_object_unref(layout);
    cairo_destroy(cr);
    cairo_surface_mark_dirty(surface);
    return surface;
}

static void nm_rebuild(PrivData *priv)
{
    cairo_surface_t *replacement;

    if (!priv->plugin->win)
        return;
    replacement = nm_render(priv, priv->cache_width, priv->cache_height);
    if (priv->cache)
        cairo_surface_destroy(priv->cache);
    priv->cache = replacement;
}

/* ------------------------------------------------------------------ */
/* Жизненный цикл                                                      */
/* ------------------------------------------------------------------ */

/* Список интерфейсов из /proc/net/dev. lo по умолчанию скрыт: его
 * трафик — это loopback между процессами, а не сетевой обмен. */
static GPtrArray *nm_discover_interfaces(gboolean include_loopback)
{
    GPtrArray *names = g_ptr_array_new_with_free_func(g_free);
    char *text = NULL;
    NmNetSample samples[NM_MAX_IFACES];
    guint count, i;

    if (!g_file_get_contents(NM_NETDEV, &text, NULL, NULL))
        return names;
    count = nm_parse_netdev_all(text, samples, NM_MAX_IFACES);
    g_free(text);
    for (i = 0; i < count; i++) {
        if (!include_loopback && strcmp(samples[i].ifname, "lo") == 0) {
            g_free(samples[i].ifname);
            continue;
        }
        g_ptr_array_add(names, samples[i].ifname);
        samples[i].ifname = NULL;   /* перешёл во владение массива */
    }
    return names;
}

static int nm_init(XsPlugin *p, GKeyFile *kf)
{
    PrivData *priv = g_new0(PrivData, 1);
    GPtrArray *names;
    int x, y;
    gdouble opacity;
    guint i;

    p->priv = priv;
    priv->plugin = p;
    priv->kf = kf;
    priv->width = CLAMP(xs_host_api()->conf_int(kf, p->name, "window_width",
                                              NM_DEFAULT_WIDTH),
                        NM_MIN_WINDOW_WIDTH, 1600);
    priv->height = CLAMP(xs_host_api()->conf_int(kf, p->name, "window_height",
                                               NM_DEFAULT_HEIGHT),
                         NM_MIN_WINDOW_HEIGHT, 1200);
    priv->corner_radius = CLAMP(xs_host_api()->conf_int(kf, p->name,
                                                        "corner_radius", 0),
                                0, 200);
    priv->design_width = priv->width;
    priv->design_height = priv->height;
    priv->update_ms = CLAMP(xs_host_api()->conf_int(kf, p->name, "update_ms",
                                                   1000), 100, 60000);
    priv->rate_smooth = CLAMP(xs_host_api()->conf_int(kf, p->name,
                                                     "rate_smooth", 1),
                              NM_RATE_SMOOTH_MIN, NM_RATE_SMOOTH_MAX);
    /* conf_int возвращает int: предел выше ~2 млн КиБ/с (2 Тбит/с) обрезался
     * бы, поэтому читаем через conf_dbl и округляем вниз. */
    priv->graph_max_kib = (gint64) xs_host_api()->conf_dbl(
        kf, p->name, "graph_max_kib", 0.0);
    /* Читаем СТРОКОЙ, потому что Properties пишет "split"/"combined":
     * conf_int() на таком значении молча вернул бы 0 и applet всегда был
     * бы в combined, что бы ни выбрал пользователь. */
    {
        char *mode = xs_host_api()->conf_str(kf, p->name, "graph_mode",
                                             "split");
        priv->graph_mode = (g_ascii_strcasecmp(mode, "combined") == 0)
                               ? NM_GRAPH_COMBINED : NM_GRAPH_SPLIT;
        g_free(mode);
    }
    /* Положение подписи — свой КЛЮЧ НА СЕРИЮ: properties пишет
     * series<N>_placement. Общий "label_placement" здесь ничего не давал
     * и молча сбрасывал бы обе подписи внутрь графика. */
    for (i = 0; i < NM_SERIES_MAX; i++) {
        char key[32];
        char *placement;

        g_snprintf(key, sizeof(key), "series%u_placement", i);
        placement = xs_host_api()->conf_str(kf, p->name, key, "inside");
        priv->label_placement[i] =
            (g_ascii_strcasecmp(placement, "outside") == 0)
                ? NM_LABEL_OUTSIDE : NM_LABEL_INSIDE;
        g_free(placement);
    }

    names = nm_discover_interfaces(FALSE);
    priv->ifname = xs_host_api()->conf_str(kf, p->name, "ifname", NULL);
    if (!priv->ifname || !*priv->ifname) {
        g_free(priv->ifname);
        priv->ifname = names->len ? g_strdup(g_ptr_array_index(names, 0))
                                  : NULL;
    }
    /* Выбранного интерфейса может не быть (уехал, переименован, не
     * поднялся). Это НЕ фатально: имя остаётся в конфиге, applet
     * показывает N/A и ждёт возврата интерфейса. */
    g_ptr_array_free(names, TRUE);
    if (!priv->ifname || !*priv->ifname) {
        p->host->log("network_monitor %s: no network interfaces found",
                     p->name);
        g_free(priv);
        p->priv = NULL;
        return -1;
    }
    priv->ip = nm_interface_ipv4(priv->ifname);

    priv->font = xs_host_api()->conf_str(kf, p->name, "font", NM_DEFAULT_FONT);
    priv->label_font = xs_host_api()->conf_str(kf, p->name, "label_font",
                                               priv->font);
    {
        char *rx = xs_host_api()->conf_str(kf, p->name, "rx_label", "Down");
        char *tx = xs_host_api()->conf_str(kf, p->name, "tx_label", "Up");
        priv->series_label[0] = rx;
        priv->series_label[1] = tx;
    }
    for (i = 0; i < NM_SERIES_MAX; i++) {
        char key[32];
        g_snprintf(key, sizeof(key), "series%u_font", i);
        priv->series_font[i] = xs_host_api()->conf_str(kf, p->name, key,
                                                       priv->font);
        /* Три строки по вертикали, иначе подпись серии в режиме "снаружи"
         * накладывалась на строку интерфейса и обе исчезали:
         *   y=2  — имя интерфейса и IP
         *   y=12 — подпись серии (только в режиме "снаружи")
         *   y=21 — значение скорости
         * Подпись стоит в той же колонке, что и её значение. */
        g_snprintf(key, sizeof(key), "series%u_x", i);
        /* Вторая колонка сдвинута за разделитель: при ширине 420 первая
         * половина идёт от 2 до 212, зазор 8, вторая — от 220. */
        priv->series_x[i] = xs_host_api()->conf_int(kf, p->name, key,
                                                    8 + (int)i * 212);
        g_snprintf(key, sizeof(key), "series%u_y", i);
        priv->series_y[i] = xs_host_api()->conf_int(kf, p->name, key, 21);
        g_snprintf(key, sizeof(key), "series%u_label_x", i);
        priv->series_label_x[i] = xs_host_api()->conf_int(kf, p->name, key,
                                                          8 + (int)i * 212);
        g_snprintf(key, sizeof(key), "series%u_label_y", i);
        priv->series_label_y[i] = xs_host_api()->conf_int(kf, p->name, key,
                                                          12);
    }
    priv->header_x = xs_host_api()->conf_int(kf, p->name, "header_x", 4);
    priv->header_y = xs_host_api()->conf_int(kf, p->name, "header_y", 2);
    priv->total_x = xs_host_api()->conf_int(kf, p->name, "total_x", 4);
    priv->total_y = xs_host_api()->conf_int(kf, p->name, "total_y",
                                            priv->height - 12);

    nm_read_color(priv, "graph_background_color", nm_graph_bg_default,
                  priv->graph_bg);
    nm_read_color(priv, "border_color", nm_border_default, priv->border);
    nm_read_color(priv, "text_color", nm_text_default, priv->text_color);
    nm_read_color(priv, "rx_color", nm_rx_default, priv->series_color[0]);
    nm_read_color(priv, "tx_color", nm_tx_default, priv->series_color[1]);
    memcpy(priv->series_text_color[0], priv->series_color[0],
           sizeof(gdouble) * 4);
    memcpy(priv->series_text_color[1], priv->series_color[1],
           sizeof(gdouble) * 4);

    g_key_file_set_string(kf, p->name, "ifname", priv->ifname);
    g_key_file_set_string(kf, p->name, "font", priv->font);
    g_key_file_set_string(kf, p->name, "rx_label", priv->series_label[0]);
    g_key_file_set_string(kf, p->name, "tx_label", priv->series_label[1]);
    g_key_file_set_integer(kf, p->name, "window_width", priv->width);
    g_key_file_set_integer(kf, p->name, "window_height", priv->height);
    g_key_file_set_integer(kf, p->name, "update_ms", priv->update_ms);
    g_key_file_set_string(kf, p->name, "graph_mode",
                          (priv->graph_mode == NM_GRAPH_SPLIT) ? "split"
                                                              : "combined");
    for (i = 0; i < NM_SERIES_MAX; i++) {
        char key[32];
        g_snprintf(key, sizeof(key), "series%u_placement", i);
        g_key_file_set_string(kf, p->name, key,
                              (priv->label_placement[i] == NM_LABEL_OUTSIDE)
                                  ? "outside" : "inside");
    }
    g_key_file_set_integer(kf, p->name, "rate_smooth", priv->rate_smooth);
    g_key_file_set_integer(kf, p->name, "corner_radius",
                           priv->corner_radius);
    xs_core_plugin_conf_flush(p->name);

    x = xs_host_api()->conf_int(kf, p->name, "x", 80);
    y = xs_host_api()->conf_int(kf, p->name, "y", 80);
    opacity = xs_host_api()->conf_dbl(kf, p->name, "opacity", 1.0);
    p->win = xs_host_api()->make_window(p, x, y, priv->width, priv->height);
    if (!p->win) {
        p->host->log("network_monitor: failed to create window");
        nm_shutdown(p);
        return -1;
    }
    priv->cache_width = priv->width;
    priv->cache_height = priv->height;
    nm_sample(priv);
    priv->cache = nm_render(priv, priv->width, priv->height);
    xs_host_api()->set_opacity(p, CLAMP(opacity, 0.1, 1.0));
    xs_host_api()->set_tick(p, priv->update_ms);
    return 0;
}

static guint nm_tick(XsPlugin *p)
{
    PrivData *priv = p ? p->priv : NULL;

    if (!priv || !p->win)
        return 0;
    nm_sample(priv);
    nm_rebuild(priv);
    xs_host_api()->invalidate(p);
    gtk_widget_queue_draw(p->win);
    return priv->update_ms;
}

/* Скруглённый контур как ВИДИМАЯ фигура окна. cairo-клип в nm_render()
 * ограничивает только рисование плагина; само X-окно остаётся
 * прямоугольным, пока не задан shape — именно поэтому углы были
 * квадратными на экране. NULL снимает shape (corner_radius=0). */
static void nm_apply_shape(XsPlugin *p, int w, int h)
{
    PrivData *priv = p ? p->priv : NULL;
    GdkWindow *window;
    cairo_region_t *region;

    if (!priv || !p->win || w <= 0 || h <= 0)
        return;
    if (priv->shape_radius == priv->corner_radius &&
        priv->shape_w == w && priv->shape_h == h)
        return;
    window = gtk_widget_get_window(p->win);
    if (!window)
        return;
    region = nm_rounded_region(w, h, priv->corner_radius);
    gdk_window_shape_combine_region(window, region, 0, 0);
    if (region)
        cairo_region_destroy(region);
    priv->shape_radius = priv->corner_radius;
    priv->shape_w = w;
    priv->shape_h = h;
}

static void nm_draw(XsPlugin *p, cairo_t *cr, int w, int h)
{
    PrivData *priv = p ? p->priv : NULL;

    if (!priv || !priv->cache)
        return;
    if (w != priv->cache_width || h != priv->cache_height) {
        priv->cache_width = w;
        priv->cache_height = h;
        nm_rebuild(priv);
    }
    nm_apply_shape(p, w, h);
    cairo_set_source_surface(cr, priv->cache, 0, 0);
    cairo_paint(cr);
}

static void nm_shutdown(XsPlugin *p)
{
    PrivData *priv = p ? p->priv : NULL;

    if (!priv)
        return;
    /* Запись таблицы указывала бы на уже освобождённый priv: обработчик
     * Properties после перезапуска плагина получил бы висячий указатель. */
    if (nm_instances)
        g_hash_table_remove(nm_instances, p->name);
    if (p->win) {
        GdkWindow *win = gtk_widget_get_window(p->win);
        if (win)
            gdk_window_shape_combine_region(win, NULL, 0, 0);
    }
    if (priv->cache)
        cairo_surface_destroy(priv->cache);
    g_free(priv->ifname);
    g_free(priv->ip);
    g_free(priv->font);
    g_free(priv->label_font);
    {
        guint i;
        for (i = 0; i < NM_SERIES_MAX; i++) {
            g_free(priv->series_font[i]);
            g_free(priv->series_label[i]);
        }
    }
    g_free(priv);
    p->priv = NULL;
}

static int nm_find_combo_text(GtkComboBox *combo, const char *text)
{
    GtkTreeModel *model;
    GtkTreeIter iter;
    gboolean valid;

    if (!combo || !text)
        return -1;
    model = gtk_combo_box_get_model(combo);
    if (!model)
        return -1;
    valid = gtk_tree_model_get_iter_first(model, &iter);
    while (valid) {
        gchar *value = NULL;
        gtk_tree_model_get(model, &iter, 0, &value, -1);
        if (value && !strcmp(value, text)) {
            GtkTreePath *path = gtk_tree_model_get_path(model, &iter);
            gint index = path ? gtk_tree_path_get_indices(path)[0] : -1;
            g_free(value);
            if (path)
                gtk_tree_path_free(path);
            return index;
        }
        g_free(value);
        valid = gtk_tree_model_iter_next(model, &iter);
    }
    return -1;
}

/* Живой экземпляр, к которому относится диалог. Таблица живёт на уровне
 * файла: nm_properties регистрирует priv в ней, а обработчики ищут его
 * по имени плагина — иначе Properties не видел бы живой applet. */
static void nm_instances_ensure(void)
{
    if (!nm_instances)
        nm_instances = g_hash_table_new(g_str_hash, g_str_equal);
}

static PrivData *nm_live_priv(NmDialogContext *ctx)
{
    gpointer data;

    if (!ctx)
        return NULL;
    nm_instances_ensure();
    data = g_hash_table_lookup(nm_instances, nm_dialog_context_name(ctx));
    return data;
}

static void nm_position_changed(GtkSpinButton *spin, gpointer data)
{
    NmDialogContext *ctx = data;
    PrivData *priv = nm_live_priv(ctx);
    const char *key;
    int value;
    XsPlugin *p;
    char numeric_key[32];
    int series_index = -1;

    if (!priv)
        return;
    p = priv->plugin;
    key = g_object_get_data(G_OBJECT(spin), "xs-key");
    if (!key)
        return;
    value = (int) gtk_spin_button_get_value(spin);

    if (g_str_has_prefix(key, "series") &&
        sscanf(key, "series%d_%31s", &series_index, numeric_key) == 2) {
        if (series_index >= 0 && series_index < NM_SERIES_MAX) {
            if (!strcmp(numeric_key, "x"))
                priv->series_x[series_index] = value;
            else if (!strcmp(numeric_key, "y"))
                priv->series_y[series_index] = value;
            else if (!strcmp(numeric_key, "label_x"))
                priv->series_label_x[series_index] = value;
            else if (!strcmp(numeric_key, "label_y"))
                priv->series_label_y[series_index] = value;
        }
    } else if (!strcmp(key, "header_x")) {
        priv->header_x = value;
    } else if (!strcmp(key, "header_y")) {
        priv->header_y = value;
    } else if (!strcmp(key, "total_x")) {
        priv->total_x = value;
    } else if (!strcmp(key, "total_y")) {
        priv->total_y = value;
    } else if (!strcmp(key, "window_width")) {
        priv->width = value;
    } else if (!strcmp(key, "window_height")) {
        priv->height = value;
    } else if (!strcmp(key, "corner_radius")) {
        priv->corner_radius = value;
    } else if (!strcmp(key, "graph_max_kib")) {
        priv->graph_max_kib = value;
    } else if (!strcmp(key, "rate_smooth")) {
        priv->rate_smooth = CLAMP(value, NM_RATE_SMOOTH_MIN,
                                  NM_RATE_SMOOTH_MAX);
        value = priv->rate_smooth;
    } else if (!strcmp(key, "update_ms")) {
        priv->update_ms = value;
        xs_host_api()->set_tick(p, value);
    }
    /* graph_max_kib может не влезть в int, поэтому пишем его строкой
     * через conf_dbl-совместимый путь; остальные ключи — обычный int. */
    if (!strcmp(key, "graph_max_kib")) {
        char *text = g_strdup_printf("%ld", (long) priv->graph_max_kib);
        g_key_file_set_string(priv->kf, p->name, key, text);
        g_free(text);
    } else
        g_key_file_set_integer(priv->kf, p->name, key, value);
    xs_core_plugin_conf_flush(p->name);
    xs_host_api()->resize(p, priv->width, priv->height);
    priv->cache_width = priv->width;
    priv->cache_height = priv->height;
    nm_rebuild(priv);
    gtk_widget_queue_draw(p->win);
}

static void nm_label_changed(GtkEditable *entry, gpointer data)
{
    NmDialogContext *ctx = data;
    PrivData *priv = nm_live_priv(ctx);
    const char *key;
    const char *value;
    XsPlugin *p;
    int series_index = -1;
    char tail[32];

    if (!priv)
        return;
    p = priv->plugin;
    key = g_object_get_data(G_OBJECT(entry), "xs-key");
    if (!key)
        return;
    value = gtk_entry_get_text(GTK_ENTRY(entry));
    if (g_str_has_prefix(key, "series") &&
        sscanf(key, "series%d_%31s", &series_index, tail) == 2) {
        if (series_index >= 0 && series_index < NM_SERIES_MAX &&
            !strcmp(tail, "label")) {
            g_free(priv->series_label[series_index]);
            priv->series_label[series_index] = g_strdup(value);
        }
    }
    g_key_file_set_string(priv->kf, p->name, key, value);
    xs_core_plugin_conf_flush(p->name);
    nm_rebuild(priv);
    if (p->win)
        gtk_widget_queue_draw(p->win);
}

static void nm_placement_changed(GtkComboBox *combo, gpointer data)
{
    NmDialogContext *ctx = data;
    PrivData *priv = nm_live_priv(ctx);
    gint active;
    const char *key;
    int series_index = -1;
    char tail[32];
    XsPlugin *p;

    if (!priv)
        return;
    p = priv->plugin;
    key = g_object_get_data(G_OBJECT(combo), "xs-key");
    if (!key || sscanf(key, "series%d_%31s", &series_index, tail) != 2 ||
        strcmp(tail, "placement"))
        return;
    active = gtk_combo_box_get_active(combo);
    if (series_index < 0 || series_index >= NM_SERIES_MAX)
        return;
    priv->label_placement[series_index] =
        (active == 1) ? NM_LABEL_OUTSIDE : NM_LABEL_INSIDE;
    g_key_file_set_string(priv->kf, p->name, key,
                          (active == 1) ? "outside" : "inside");
    xs_core_plugin_conf_flush(p->name);
    nm_rebuild(priv);
    if (p->win)
        gtk_widget_queue_draw(p->win);
}

static void nm_graph_mode_changed(GtkComboBox *combo, gpointer data)
{
    NmDialogContext *ctx = data;
    PrivData *priv = nm_live_priv(ctx);
    gint active;
    XsPlugin *p;

    if (!priv)
        return;
    p = priv->plugin;
    active = gtk_combo_box_get_active(combo);
    priv->graph_mode = (active == 1) ? NM_GRAPH_COMBINED : NM_GRAPH_SPLIT;
    g_key_file_set_string(priv->kf, p->name, "graph_mode",
                          (active == 1) ? "combined" : "split");
    xs_core_plugin_conf_flush(p->name);
    nm_rebuild(priv);
    if (p->win)
        gtk_widget_queue_draw(p->win);
}

static void nm_iface_changed(GtkComboBox *combo, gpointer data)
{
    NmDialogContext *ctx = data;
    PrivData *priv = nm_live_priv(ctx);
    GtkTreeModel *model;
    GtkTreeIter iter;
    gchar *value = NULL;
    XsPlugin *p;

    if (!priv)
        return;
    p = priv->plugin;
    model = gtk_combo_box_get_model(combo);
    if (!model || !gtk_combo_box_get_active_iter(combo, &iter))
        return;
    gtk_tree_model_get(model, &iter, 0, &value, -1);
    if (value) {
        g_free(priv->ifname);
        priv->ifname = g_strdup(value);
        g_free(priv->ip);
        priv->ip = nm_interface_ipv4(priv->ifname);
        /* Смена цели обнуляет baseline: иначе первая дельта после
         * переключения считалась бы от счётчиков прошлого интерфейса. */
        priv->prev_valid = FALSE;
        priv->sample_valid = FALSE;
        memset(priv->history, 0, sizeof(priv->history));
        memset(priv->head, 0, sizeof(priv->head));
        memset(priv->count, 0, sizeof(priv->count));
        memset(priv->total_bytes, 0, sizeof(priv->total_bytes));
        memset(priv->current_rate, 0, sizeof(priv->current_rate));
        memset(priv->smooth, 0, sizeof(priv->smooth));
        g_key_file_set_string(priv->kf, p->name, "ifname", priv->ifname);
        xs_core_plugin_conf_flush(p->name);
        nm_rebuild(priv);
        if (p->win)
            gtk_widget_queue_draw(p->win);
        g_free(value);
    }
}

static void nm_color_set(GtkColorButton *button, gpointer data)
{
    NmDialogContext *ctx = data;
    PrivData *priv = nm_live_priv(ctx);
    const char *key;
    GdkRGBA rgba;
    gdouble out[4];
    int series_index = -1;
    char tail[32];
    XsPlugin *p;

    if (!priv)
        return;
    p = priv->plugin;
    key = g_object_get_data(G_OBJECT(button), "xs-key");
    if (!key)
        return;
    gtk_color_chooser_get_rgba(GTK_COLOR_CHOOSER(button), &rgba);
    out[0] = rgba.red;
    out[1] = rgba.green;
    out[2] = rgba.blue;
    out[3] = rgba.alpha;
    /* У alpha-only селекторов (фон, рамка) берём alpha ИЗ КНОПКИ, у RGB-only
     * берём её из сохранённого состояния: gtk_color_chooser без шкалы
     * возвращает alpha=1.0 и обнулил бы прозрачность при первом же клике. */
    {
        gboolean rgb_only = g_object_get_data(G_OBJECT(button),
                                              "xs-rgb-only") ? TRUE : FALSE;
        if (rgb_only) {
            const gdouble *current = NULL;
            if (g_str_has_prefix(key, "series") &&
                sscanf(key, "series%d_%31s", &series_index, tail) == 2) {
                if (series_index >= 0 && series_index < NM_SERIES_MAX)
                    current = priv->series_color[series_index];
            } else if (!strcmp(key, "text_color")) {
                current = priv->text_color;
            }
            if (current)
                out[3] = current[3];
        }
    }
    if (g_str_has_prefix(key, "series") &&
        sscanf(key, "series%d_%31s", &series_index, tail) == 2) {
        if (series_index >= 0 && series_index < NM_SERIES_MAX) {
            if (!strcmp(tail, "color")) {
                memcpy(priv->series_color[series_index], out,
                       sizeof(gdouble) * 4);
                memcpy(priv->series_text_color[series_index], out,
                       sizeof(gdouble) * 4);
            } else if (!strcmp(tail, "text_color")) {
                memcpy(priv->series_text_color[series_index], out,
                       sizeof(gdouble) * 4);
            }
        }
    } else if (!strcmp(key, "graph_background_color")) {
        memcpy(priv->graph_bg, out, sizeof(gdouble) * 4);
    } else if (!strcmp(key, "border_color")) {
        memcpy(priv->border, out, sizeof(gdouble) * 4);
    } else if (!strcmp(key, "text_color")) {
        memcpy(priv->text_color, out, sizeof(gdouble) * 4);
    }
    g_key_file_set_string(priv->kf, p->name, key, nm_format_rgba(out));
    xs_core_plugin_conf_flush(p->name);
    nm_rebuild(priv);
    if (p->win)
        gtk_widget_queue_draw(p->win);
}

static void nm_series_font_set(GtkFontButton *button, gpointer data)
{
    NmDialogContext *ctx = data;
    PrivData *priv = nm_live_priv(ctx);
    const char *key;
    const char *value;
    int series_index = -1;
    char tail[32];
    XsPlugin *p;

    if (!priv)
        return;
    p = priv->plugin;
    key = g_object_get_data(G_OBJECT(button), "xs-key");
    if (!key)
        return;
    value = gtk_font_button_get_font_name(GTK_FONT_BUTTON(button));
    if (g_str_has_prefix(key, "series") &&
        sscanf(key, "series%d_%31s", &series_index, tail) == 2 &&
        series_index >= 0 && series_index < NM_SERIES_MAX) {
        g_free(priv->series_font[series_index]);
        priv->series_font[series_index] = g_strdup(value);
    }
    g_key_file_set_string(priv->kf, p->name, key, value);
    xs_core_plugin_conf_flush(p->name);
    nm_rebuild(priv);
    if (p->win)
        gtk_widget_queue_draw(p->win);
}

/* Рекурсивная привязка по типу виджета: у ключевых контролов
 * разная сигнатура (font-set, color-set, value-changed), и дублировать
 * хрупкий обход по вложенным индексам в плагине не стоит. */
static void nm_bind_keyed_descendants(GtkWidget *widget, NmDialogContext *ctx)
{
    if (!widget)
        return;
    if (GTK_IS_FONT_BUTTON(widget)) {
        g_signal_connect(widget, "font-set",
                         G_CALLBACK(nm_series_font_set), ctx);
        return;
    }
    if (GTK_IS_COLOR_BUTTON(widget)) {
        g_signal_connect(widget, "color-set",
                         G_CALLBACK(nm_color_set), ctx);
        return;
    }
    if (GTK_IS_SPIN_BUTTON(widget)) {
        g_signal_connect(widget, "value-changed",
                         G_CALLBACK(nm_position_changed), ctx);
        return;
    }
    if (GTK_IS_ENTRY(widget)) {
        g_signal_connect(widget, "changed", G_CALLBACK(nm_label_changed), ctx);
        return;
    }
    if (GTK_IS_COMBO_BOX(widget)) {
        const char *key = g_object_get_data(G_OBJECT(widget), "xs-key");
        if (key && g_str_has_suffix(key, "placement"))
            g_signal_connect(widget, "changed",
                             G_CALLBACK(nm_placement_changed), ctx);
        else if (key && !strcmp(key, "graph_mode"))
            g_signal_connect(widget, "changed",
                             G_CALLBACK(nm_graph_mode_changed), ctx);
        else
            g_signal_connect(widget, "changed",
                             G_CALLBACK(nm_iface_changed), ctx);
        return;
    }
    if (GTK_IS_CONTAINER(widget)) {
        GList *children = gtk_container_get_children(GTK_CONTAINER(widget));
        GList *it;
        for (it = children; it; it = it->next)
            nm_bind_keyed_descendants(GTK_WIDGET(it->data), ctx);
        g_list_free(children);
    }
}

/* Секция настроек: грид + счётчик строк.
 *
 * Раньше каждый хелпер жёстко писал в строку 0, и все поля секции
 * ложились в одну строку друг на друга. Счётчик двигает каждую пару
 * подпись/контрол на свою строку. */
typedef struct {
    GtkWidget *grid;
    int row;
} NmGrid;

static NmGrid *nm_grid_new(void)
{
    NmGrid *g = g_new0(NmGrid, 1);

    g->grid = gtk_grid_new();
    gtk_grid_set_column_spacing(GTK_GRID(g->grid), 8);
    gtk_grid_set_row_spacing(GTK_GRID(g->grid), 4);
    return g;
}

static void nm_grid_add_label(NmGrid *g, const char *label)
{
    GtkWidget *l = gtk_label_new(label);

    gtk_widget_set_halign(l, GTK_ALIGN_START);
    gtk_grid_attach(GTK_GRID(g->grid), l, 0, g->row, 1, 1);
}

static void nm_grid_add_widget(NmGrid *g, GtkWidget *w)
{
    gtk_grid_attach(GTK_GRID(g->grid), w, 1, g->row, 1, 1);
    g->row++;
}

/* Пара координат в ОДНОЙ строке. Восемь отдельных полей Position X/Y и
 * Label X/Y разворачивали каждую секцию серии вчетверо и делали диалог
 * вчетверо длиннее; в строке "X / Y" те же восемь значений занимают
 * две строки на серию. */
static void nm_add_xy(NmGrid *g, NmDialogContext *ctx,
                      const char *label, const char *key_x, const char *key_y,
                      int value_x, int value_y, int max)
{
    GtkWidget *box = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 4);
    GtkWidget *sx = gtk_spin_button_new_with_range(0, max, 1);
    GtkWidget *sy = gtk_spin_button_new_with_range(0, max, 1);

    gtk_spin_button_set_value(GTK_SPIN_BUTTON(sx), value_x);
    gtk_spin_button_set_value(GTK_SPIN_BUTTON(sy), value_y);
    gtk_widget_set_size_request(sx, 72, -1);
    gtk_widget_set_size_request(sy, 72, -1);
    g_object_set_data(G_OBJECT(sx), "xs-key", (gpointer) key_x);
    g_object_set_data(G_OBJECT(sy), "xs-key", (gpointer) key_y);
    gtk_box_pack_start(GTK_BOX(box), sx, TRUE, TRUE, 0);
    gtk_box_pack_start(GTK_BOX(box), gtk_label_new("/"), FALSE, FALSE, 0);
    gtk_box_pack_start(GTK_BOX(box), sy, TRUE, TRUE, 0);
    nm_grid_add_label(g, label);
    nm_grid_add_widget(g, box);
    nm_bind_keyed_descendants(sx, ctx);
    nm_bind_keyed_descendants(sy, ctx);
}

static void nm_add_int(NmGrid *g, NmDialogContext *ctx, const char *key,
                       const char *label, int value, int min, int max)
{
    GtkWidget *spin = gtk_spin_button_new_with_range(min, max, 1);

    gtk_spin_button_set_value(GTK_SPIN_BUTTON(spin), value);
    gtk_widget_set_size_request(spin, 90, -1);
    g_object_set_data(G_OBJECT(spin), "xs-key", (gpointer) key);
    nm_grid_add_label(g, label);
    nm_grid_add_widget(g, spin);
    nm_bind_keyed_descendants(spin, ctx);
}

static void nm_add_color(NmGrid *g, NmDialogContext *ctx, const char *key,
                         const char *label, const gdouble color[4],
                         gboolean with_alpha)
{
    GtkWidget *button = gtk_color_button_new_with_rgba(&(GdkRGBA) {
        color[0], color[1], color[2], color[3] });

    gtk_color_chooser_set_use_alpha(GTK_COLOR_CHOOSER(button), with_alpha);
    if (!with_alpha)
        g_object_set_data(G_OBJECT(button), "xs-rgb-only", GINT_TO_POINTER(1));
    g_object_set_data(G_OBJECT(button), "xs-key", (gpointer) key);
    nm_grid_add_label(g, label);
    nm_grid_add_widget(g, button);
    nm_bind_keyed_descendants(button, ctx);
}

static void nm_add_text(NmGrid *g, NmDialogContext *ctx, const char *key,
                        const char *label, const char *value)
{
    GtkWidget *entry = gtk_entry_new();
    GtkWidget *box;

    gtk_entry_set_text(GTK_ENTRY(entry), value ? value : "");
    gtk_widget_set_size_request(entry, 180, -1);
    g_object_set_data(G_OBJECT(entry), "xs-key", (gpointer) key);
    box = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 4);
    gtk_box_pack_start(GTK_BOX(box), entry, TRUE, TRUE, 0);
    nm_grid_add_label(g, label);
    nm_grid_add_widget(g, box);
    nm_bind_keyed_descendants(entry, ctx);
}

static void nm_add_font(NmGrid *g, NmDialogContext *ctx, const char *key,
                        const char *label, const char *value)
{
    GtkWidget *button = gtk_font_button_new_with_font(value ? value : "Sans 8");

    gtk_widget_set_size_request(button, 180, -1);
    g_object_set_data(G_OBJECT(button), "xs-key", (gpointer) key);
    nm_grid_add_label(g, label);
    nm_grid_add_widget(g, button);
    nm_bind_keyed_descendants(button, ctx);
}

static GtkWidget *nm_add_combo(NmGrid *g, NmDialogContext *ctx,
                               const char *key, const char *label,
                               const char *const *options, int active)
{
    GtkWidget *combo = gtk_combo_box_text_new();
    int i;

    for (i = 0; options[i]; i++)
        gtk_combo_box_text_append(GTK_COMBO_BOX_TEXT(combo), options[i],
                                  options[i]);
    gtk_combo_box_set_active(GTK_COMBO_BOX(combo), active);
    g_object_set_data(G_OBJECT(combo), "xs-key", (gpointer) key);
    nm_grid_add_label(g, label);
    nm_grid_add_widget(g, combo);
    nm_bind_keyed_descendants(combo, ctx);
    return combo;
}

static GtkWidget *nm_section(GtkWidget *page, const char *title)
{
    GtkWidget *frame = gtk_frame_new(title);

    gtk_box_pack_start(GTK_BOX(page), frame, FALSE, FALSE, 4);
    return frame;
}

static void nm_properties(XsPlugin *p, GtkNotebook *notebook)
{
    PrivData *priv = p->priv;
    NmDialogContext *ctx;
    GtkWidget *page, *frame;
    NmGrid *g;
    GPtrArray *names;
    int i;

    if (!priv)
        return;
    nm_instances_ensure();
    g_hash_table_insert(nm_instances, g_strdup(p->name), priv);
    ctx = nm_dialog_context_new(p->name);

    page = gtk_box_new(GTK_ORIENTATION_VERTICAL, 6);
    gtk_container_set_border_width(GTK_CONTAINER(page), 8);

    /* --- Интерфейс и график --- */
    frame = nm_section(page, "Interface");
    g = nm_grid_new();
    {
        guint index;
        GtkWidget *combo;

        names = nm_discover_interfaces(FALSE);
        combo = gtk_combo_box_text_new();
        for (index = 0; index < names->len; index++)
            gtk_combo_box_text_append(GTK_COMBO_BOX_TEXT(combo),
                                      g_ptr_array_index(names, index),
                                      g_ptr_array_index(names, index));
        index = (guint) MAX(0, nm_find_combo_text(GTK_COMBO_BOX(combo),
                                                 priv->ifname));
        gtk_combo_box_set_active(GTK_COMBO_BOX(combo), (gint) index);
        g_object_set_data(G_OBJECT(combo), "xs-key", (gpointer) "ifname");
        nm_grid_add_label(g, "Interface");
        nm_grid_add_widget(g, combo);
        nm_bind_keyed_descendants(combo, ctx);
        g_ptr_array_free(names, TRUE);
    }
    {
        static const char *modes[] = {"Split", "Combined", NULL};
        nm_add_combo(g, ctx, "graph_mode", "Graph", modes,
                     priv->graph_mode == NM_GRAPH_SPLIT ? 0 : 1);
    }
    /* Предел шкалы в КиБ/с: 0 = по максимуму истории. Шкала общая для обеих
     * серий, поэтому одна настройка на оба графика. */
    nm_add_int(g, ctx, "graph_max_kib", "Graph max (KiB/s, 0=auto)",
               (int) priv->graph_max_kib, 0, 2000000);
    gtk_container_add(GTK_CONTAINER(frame), g->grid);

    /* --- Серии --- */
    for (i = 0; i < NM_SERIES_MAX; i++) {
        char label[32];
        char key[32];
        static const char *placements[] = {"Inside graph", "Outside", NULL};

        g_snprintf(label, sizeof(label), "%s series",
                   i == 0 ? "Download" : "Upload");
        frame = nm_section(page, label);
        g = nm_grid_new();
        g_snprintf(key, sizeof(key), "series%d_label", i);
        nm_add_text(g, ctx, key, "Label", priv->series_label[i]);
        g_snprintf(key, sizeof(key), "series%d_font", i);
        nm_add_font(g, ctx, key, "Font", priv->series_font[i]);
        {
            gdouble color[4];
            memcpy(color, priv->series_color[i], sizeof(color));
            g_snprintf(key, sizeof(key), "series%d_color", i);
            nm_add_color(g, ctx, key, "Color", color, FALSE);
        }
        g_snprintf(key, sizeof(key), "series%d_placement", i);
        nm_add_combo(g, ctx, key, "Placement", placements,
                     priv->label_placement[i] == NM_LABEL_OUTSIDE ? 1 : 0);
        {
            char key_x[40], key_y[40];

            g_snprintf(key_x, sizeof(key_x), "series%d_x", i);
            g_snprintf(key_y, sizeof(key_y), "series%d_y", i);
            nm_add_xy(g, ctx, "Position", key_x, key_y, priv->series_x[i],
                      priv->series_y[i], MAX(priv->design_width,
                                             priv->design_height) - 1);
            g_snprintf(key_x, sizeof(key_x), "series%d_label_x", i);
            g_snprintf(key_y, sizeof(key_y), "series%d_label_y", i);
            nm_add_xy(g, ctx, "Label pos.", key_x, key_y,
                      priv->series_label_x[i], priv->series_label_y[i],
                      MAX(priv->design_width, priv->design_height) - 1);
        }
        gtk_container_add(GTK_CONTAINER(frame), g->grid);
        g_free(g);
    }

    /* --- Оформление --- */
    frame = nm_section(page, "Appearance");
    g = nm_grid_new();
    nm_add_color(g, ctx, "graph_background_color", "Graph background",
                 priv->graph_bg, TRUE);
    nm_add_color(g, ctx, "text_color", "Text", priv->text_color, FALSE);
    nm_add_color(g, ctx, "border_color", "Graph border", priv->border, TRUE);
    nm_add_font(g, ctx, "label_font", "Header font", priv->label_font);
    nm_add_xy(g, ctx, "Header", "header_x", "header_y", priv->header_x,
              priv->header_y, MAX(priv->design_width, priv->design_height) - 1);
    nm_add_xy(g, ctx, "Total", "total_x", "total_y", priv->total_x,
              priv->total_y, MAX(priv->design_width, priv->design_height) - 1);
    gtk_container_add(GTK_CONTAINER(frame), g->grid);
    g_free(g);

    /* --- Окно --- */
    frame = nm_section(page, "Window");
    g = nm_grid_new();
    {
        /* Ширина и высота несут разные минимумы, поэтому пара
         * nm_add_xy с общим максимумом тут не годится. */
        nm_add_int(g, ctx, "window_width", "Width", priv->width,
                   NM_MIN_WINDOW_WIDTH, 1600);
        nm_add_int(g, ctx, "window_height", "Height", priv->height,
                   NM_MIN_WINDOW_HEIGHT, 1200);
    }
    nm_add_int(g, ctx, "corner_radius", "Corner radius", priv->corner_radius,
               0, 200);
    nm_add_int(g, ctx, "update_ms", "Update (ms)", priv->update_ms, 100,
               60000);
    nm_add_int(g, ctx, "rate_smooth", "Smoothing", priv->rate_smooth,
               NM_RATE_SMOOTH_MIN, NM_RATE_SMOOTH_MAX);
    gtk_container_add(GTK_CONTAINER(frame), g->grid);
    g_free(g);

    /* Страница длиннее окна диалога: без прокрутки Properties вырос бы
     * под все строки и ушёл бы за пределы экрана. */
    {
        GtkWidget *tab = page;
        int width, height;

        nm_properties_size(p->type, &width, &height);
        if (width > 0 && height > 0) {
            GtkWidget *scroller = nm_properties_scroller(page, width, height);
            if (scroller)
                tab = scroller;
        }
        /* Контекст живёт ДОЛЬШЕ диалога: каждый контрол держит его в
         * g_signal_connect(user_data) и при срабатывании читает
         * instance_name. Освобождать его здесь нельзя — обработчик
         * получал бы указатель на уже освобождённую строку, и
         * g_hash_table_lookup() падал бы внутри g_str_hash(). Ловится
         * как segfault при прокрутке диалога колесом мыши. */
        g_object_set_data_full(G_OBJECT(tab), NM_CTX_KEY, ctx,
                               (GDestroyNotify) nm_dialog_context_unref);
        gtk_notebook_append_page(notebook, tab,
                                 gtk_label_new("Network Monitor"));
        gtk_widget_show_all(tab);
    }
}

static const XsPluginOps nm_ops = {
    .init = nm_init,
    .draw = nm_draw,
    .tick = nm_tick,
    .shutdown = nm_shutdown,
    .properties = nm_properties,
};
static XsPluginDesc nm_desc = {
    "network_monitor", XS_API_VERSION, &nm_ops,
    "Per-interface up/down speed and history monitor",
    "xscreenlets", "1.0"
};
XS_PLUGIN_EXPORT(&nm_desc)
