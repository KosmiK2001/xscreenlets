/* memory_monitor.c — C/GTK3 RAM and swap history monitor.
 *
 * tick() is the only path which reads /proc/meminfo. draw() only paints a
 * cached Cairo surface and therefore performs no I/O.
 */
#include <gtk/gtk.h>
#include <glib.h>
#include <math.h>
#include <string.h>
#include "xs_api.h"
#include "common.h"
#include "memory_monitor_core.h"

#include "../core/i18n.h"
#define MM_DEFAULT_WINDOW_WIDTH 320
#define MM_DEFAULT_WINDOW_HEIGHT 344
#define MM_HISTORY_MAX 4096
#define MM_DEFAULT_FONT "Ubuntu 8"
#define MM_PAD 4.0

typedef struct {
    XsPlugin *plugin;
    GKeyFile *kf;
    guint update_ms;
    int window_width;
    int window_height;
    char *font;
    gdouble background_color[4];
    gdouble graph_background_color[4];
    gdouble text_color[4];
    gdouble graph_border_color[4];
    /* Рамка ОКНА (не графика). Толщина 0 = без рамки: до этой настройки
     * applet рамок не рисовал вовсе, и добавлять их принудительно не
     * правильно. */
    gdouble border_color[4];
    double border_width;
    gdouble ram_color[4];
    gdouble shared_color[4];
    gdouble buffers_color[4];
    gdouble cache_color[4];
    gdouble swap_color[4];
    gdouble ram_history[MM_RAM_COMPONENTS][MM_HISTORY_MAX];
    gdouble swap_history[MM_HISTORY_MAX];
    guint ram_head[MM_RAM_COMPONENTS];
    guint swap_head;
    guint ram_count[MM_RAM_COMPONENTS];
    guint swap_count;
    cairo_surface_t *cache;
    int cache_width;
    int cache_height;
    MemorySample sample;
    /* Скругление углов окна. shape_* - кэш применённой маски X-сервера:
     * форма пересобирается только когда реально изменился радиус или
     * размер, а не на каждом кадре. */
    int corner_radius;
    int shape_radius;
    int shape_w;
    int shape_h;
} PrivData;

static const gdouble mm_default_background[4] = {0.098, 0.098, 0.098, 0.75};
static const gdouble mm_default_graph_background[4] = {0.04, 0.05, 0.07, 0.92};
static const gdouble mm_default_text[4] = {1.0, 1.0, 1.0, 1.0};
static const gdouble mm_default_graph_border[4] = {0.451, 0.451, 0.451, 1.0};
/* Рамка окна по умолчанию - тот же серый, что у process_list (0.62/0.66/0.72
 * при непрозрачности 1). Alpha именно 1: подложки у рамки нет, кроме фона
 * applet, и полупрозрачность только съедала контраст. */
static const gdouble mm_default_border[4] = {0.62, 0.66, 0.72, 1.0};
static const gdouble mm_default_ram[4] = {0.325, 0.510, 0.729, 1.0};
static const gdouble mm_default_shared[4] = {0.95, 0.35, 0.55, 1.0};
static const gdouble mm_default_buffers[4] = {0.95, 0.72, 0.18, 1.0};
static const gdouble mm_default_cache[4] = {0.22, 0.76, 0.42, 1.0};
static const gdouble mm_default_swap[4] = {1.0, 0.647, 0.0, 1.0};

static char *mm_color_string(const GdkRGBA *color)
{
    const gdouble rgba[4] = {color->red, color->green, color->blue,
                             color->alpha};
    return mm_format_color(rgba);
}

static void mm_read_color(PrivData *priv, const char *key,
                          const gdouble fallback[4], gdouble out[4])
{
    char *text = xs_host_api()->conf_str(priv->kf, priv->plugin->name,
                                         key, NULL);

    memcpy(out, fallback, 4 * sizeof(gdouble));
    if (!text) {
        GdkRGBA color = {fallback[0], fallback[1], fallback[2], fallback[3]};
        char *default_text = mm_color_string(&color);

        g_key_file_set_string(priv->kf, priv->plugin->name, key, default_text);
        g_free(default_text);
    } else if (!mm_parse_color(text, out)) {
        priv->plugin->host->log("memory_monitor %s: invalid %s, using defaults",
                                priv->plugin->name, key);
        if (strcmp(key, "graph_background_color") == 0 ||
            strcmp(key, "shared_color") == 0 ||
            strcmp(key, "buffers_color") == 0 ||
            strcmp(key, "cache_color") == 0) {
            GdkRGBA color = {fallback[0], fallback[1], fallback[2], fallback[3]};
            char *default_text = mm_color_string(&color);

            g_key_file_set_string(priv->kf, priv->plugin->name, key,
                                  default_text);
            g_free(default_text);
        }
    }
    g_free(text);
}

static gboolean mm_read_meminfo(MemorySample *sample)
{
    char *text = NULL;
    gboolean valid;

    if (!g_file_get_contents("/proc/meminfo", &text, NULL, NULL))
        return FALSE;
    valid = mm_parse_meminfo(text, sample);
    g_free(text);
    return valid;
}

static void mm_push(gdouble *history, guint *head, guint *count, gdouble value)
{
    history[*head] = CLAMP(value, 0.0, 1.0);
    *head = (*head + 1) % MM_HISTORY_MAX;
    *count = MIN(*count + 1, MM_HISTORY_MAX);
}

static void mm_push_ram_components(PrivData *priv, const MemorySample *sample);

static void mm_draw_graph(cairo_t *cr, double x, double y, int width,
                          int height, const gdouble *history, guint head,
                          guint count, const gdouble color[4],
                          const gdouble background[4],
                          const gdouble border[4])
{
    guint visible = MIN(count, (guint)width);
    guint start = (head + MM_HISTORY_MAX - visible) % MM_HISTORY_MAX;
    guint i;

    cairo_set_source_rgba(cr, background[0], background[1], background[2],
                          background[3]);
    cairo_rectangle(cr, x, y, width, height);
    cairo_fill(cr);
    cairo_save(cr);
    cairo_rectangle(cr, x + 1.0, y + 1.0, width - 2.0, height - 2.0);
    cairo_clip(cr);
    cairo_set_source_rgba(cr, color[0], color[1], color[2], color[3]);
    for (i = 0; i < visible; i++) {
        gdouble value = history[(start + i) % MM_HISTORY_MAX];
        double bar_height = (height - 2.0) * value;
        /* Oldest sample starts at the left; each new tick shifts right-to-left. */
        double px = x + 1.0 + (width - 2 - (int)visible) + (int)i;

        if (bar_height > 0.0)
            cairo_rectangle(cr, px, y + height - 1.0 - bar_height, 1.0,
                            bar_height);
    }
    cairo_fill(cr);
    cairo_restore(cr);
    cairo_set_source_rgba(cr, border[0], border[1], border[2], border[3]);
    cairo_set_line_width(cr, 1.0);
    cairo_rectangle(cr, x + 0.5, y + 0.5, width - 1.0, height - 1.0);
    cairo_stroke(cr);
}

static void mm_draw_ram_stack(cairo_t *cr, double x, double y, int width,
                              int height, gdouble history[MM_RAM_COMPONENTS][MM_HISTORY_MAX],
                              const guint head[MM_RAM_COMPONENTS],
                              const guint count[MM_RAM_COMPONENTS],
                              const gdouble colors[][4],
                              const gdouble background[4],
                              const gdouble border[4])
{
    guint visible = 0;
    guint start;
    guint i;
    int component;

    for (component = 0; component < MM_RAM_COMPONENTS; component++)
        visible = MAX(visible, count[component]);
    visible = MIN(visible, (guint)MAX(width, 0));
    start = (head[0] + MM_HISTORY_MAX - visible) % MM_HISTORY_MAX;

    cairo_set_source_rgba(cr, background[0], background[1], background[2],
                          background[3]);
    cairo_rectangle(cr, x, y, width, height);
    cairo_fill(cr);
    cairo_save(cr);
    cairo_rectangle(cr, x + 1.0, y + 1.0, width - 2.0, height - 2.0);
    cairo_clip(cr);
    for (i = 0; i < visible; i++) {
        guint index = (start + i) % MM_HISTORY_MAX;
        double px = x + 1.0 + (width - 2 - (int)visible) + (int)i;
        double cursor = y + height - 1.0;

        for (component = 0; component < MM_RAM_COMPONENTS; component++) {
            double segment = (height - 2.0) *
                             CLAMP(history[component][index], 0.0, 1.0);

            if (segment <= 0.0)
                continue;
            segment = MIN(segment, cursor - (y + 1.0));
            if (segment <= 0.0)
                continue;
            cursor -= segment;
            cairo_set_source_rgba(cr, colors[component][0],
                                  colors[component][1], colors[component][2],
                                  colors[component][3]);
            cairo_rectangle(cr, px, cursor, 1.0, segment);
            cairo_fill(cr);
        }
    }
    cairo_restore(cr);
    cairo_set_source_rgba(cr, border[0], border[1], border[2], border[3]);
    cairo_set_line_width(cr, 1.0);
    cairo_rectangle(cr, x + 0.5, y + 0.5, width - 1.0, height - 1.0);
    cairo_stroke(cr);
}

static void mm_set_text_color(cairo_t *cr, const gdouble color[4])
{
    cairo_set_source_rgba(cr, color[0], color[1], color[2], color[3]);
}

static void mm_draw_stippled_hr(cairo_t *cr, double x, double y, double width,
                                const gdouble color[4])
{
    double sx;

    cairo_set_source_rgba(cr, color[0], color[1], color[2], color[3]);
    for (sx = x; sx < x + width; sx += 3.0) {
        cairo_rectangle(cr, sx, y, 1.0, 1.0);
    }
    cairo_fill(cr);
}

/* Радиус углов из конфига.
 *
 * Всё, что не положительное, трактуется как «без скругления»: отрицательное
 * значение в конфиге не должно превращаться в ошибку shape-маски. */
static double mm_corner_radius_value(int value)
{
    return value > 0 ? (double)value : 0.0;
}

static gboolean mm_corner_radius_is_rounded(double radius)
{
    return radius > 0.5;
}

/* Регион со скруглёнными углами для shape-маски X-окна.
 *
 * Формула скопирована из process_list, а НЕ берётся оттуда линковкой.
 * Причина найдена на живом applet: плагины собираются изолированно,
 * memory_monitor.so линкуется только из своих object-файлов, поэтому
 * символ из process_list в него просто не попадает, и applet перестаёт
 * грузиться целиком:
 *
 *   memory_monitor.so: undefined symbol: ...
 *
 * без единой строчки в логе об ошибке, кроме undefined symbol.
 *
 * Дублирование формулы здесь не опционально, но расплата за него
 * известна: построения надо держать идентичными соседним апплетам (тот
 * же floor() - никогда не срезает глубже настоящей дуги - и та же
 * разбивка по строкам). */
static cairo_region_t *mm_rounded_region(int width, int height, int radius)
{
    const double r = mm_corner_radius_value(radius);
    cairo_region_t *region;
    cairo_rectangle_int_t box;
    int scaled;

    if (width <= 0 || height <= 0)
        return NULL;
    if (!mm_corner_radius_is_rounded(r))
        return NULL;

    scaled = (int)MIN(r, MIN(width, height) / 2.0);
    region = cairo_region_create();
    if (!region)
        return NULL;

    /* cairo_region хранит только целочисленные прямоугольники, поэтому
     * скруглённый контур приближается одним столбцом на строку. */
    for (int y = 0; y < height; y++) {
        int cut = 0;
        double dy;

        if (y < scaled)
            dy = scaled - y;
        else if (y >= height - scaled)
            dy = (double)(y - (height - scaled));
        else
            dy = 0.0;

        if (dy > 0.0) {
            double t = scaled * scaled - dy * dy;

            if (t < 0.0)
                t = 0.0;
            cut = (int)floor(scaled - sqrt(t));
        }
        box.x = cut;
        box.y = y;
        box.width = width - 2 * cut;
        box.height = 1;
        if (box.width > 0)
            cairo_region_union_rectangle(region, &box);
    }
    return region;
}

/* Контур скруглённого окна как путь, для clip в render.
 *
 * Отступ внутрь на пиксель обязателен: маска режет по краю окна, поэтому
 * контур, проведённый по самому краю, терял бы половину дуги под срез и
 * угол выходил бы рваным. */
static void mm_rounded_path(cairo_t *cr, int width, int height, int radius)
{
    const double inset = 1.0;
    double w = width - 2 * inset, h = height - 2 * inset;
    double r = mm_corner_radius_value(radius);

    if (!mm_corner_radius_is_rounded(r)) {
        cairo_rectangle(cr, inset, inset, w, h);
        return;
    }
    r = MIN(r, MIN(w, h) / 2.0);
    if (r <= 0.0) {
        cairo_rectangle(cr, inset, inset, w, h);
        return;
    }
    cairo_new_sub_path(cr);
    cairo_arc(cr, inset + w - r, inset + r, r, -G_PI / 2.0, 0.0);
    cairo_arc(cr, inset + w - r, inset + h - r, r, 0.0, G_PI / 2.0);
    cairo_arc(cr, inset + r, inset + h - r, r, G_PI / 2.0, G_PI);
    cairo_arc(cr, inset + r, inset + r, r, G_PI, 1.5 * G_PI);
    cairo_close_path(cr);
}

/* Контур рамки: округлённый прямоугольник с произвольным отступом и
 * радиусом. Отдельная функция, потому что clip и обводка требуют разных
 * пар значений - одними четырьмя cairo_arc() в render это не выразить. */
static void mm_border_path(cairo_t *cr, int width, int height,
                           double radius, double inset)
{
    double w = width - 2 * inset, h = height - 2 * inset;
    double r = MIN(radius, MIN(w, h) / 2.0);

    if (w <= 0 || h <= 0) {
        cairo_rectangle(cr, 0, 0, width, height);
        return;
    }
    if (r <= 0.0) {
        cairo_rectangle(cr, inset, inset, w, h);
        return;
    }
    cairo_new_sub_path(cr);
    cairo_arc(cr, inset + w - r, inset + r, r, -G_PI / 2.0, 0.0);
    cairo_arc(cr, inset + w - r, inset + h - r, r, 0.0, G_PI / 2.0);
    cairo_arc(cr, inset + r, inset + h - r, r, G_PI / 2.0, G_PI);
    cairo_arc(cr, inset + r, inset + r, r, G_PI, 1.5 * G_PI);
    cairo_close_path(cr);
}

/* Применить форму X-окна. Вызывается из render, где известны фактические
 * размеры окна. */
static void mm_apply_shape(PrivData *priv, XsPlugin *p, int w, int h)
{
    GdkWindow *window;
    cairo_region_t *region;

    if (!priv || !p || !p->win || w <= 0 || h <= 0)
        return;
    if (priv->shape_radius == priv->corner_radius &&
        priv->shape_w == w && priv->shape_h == h)
        return;
    window = gtk_widget_get_window(p->win);
    if (!window)
        return;
    region = mm_rounded_region(w, h, priv->corner_radius);
    gdk_window_shape_combine_region(window, region, 0, 0);
    if (region)
        cairo_region_destroy(region);
    priv->shape_radius = priv->corner_radius;
    priv->shape_w = w;
    priv->shape_h = h;
}

static cairo_surface_t *mm_render(PrivData *priv, int width, int height)
{
    cairo_surface_t *surface = cairo_image_surface_create(CAIRO_FORMAT_ARGB32,
                                                           width, height);
    cairo_t *cr = cairo_create(surface);
    PangoFontDescription *font;
    PangoLayout *layout;
    guint64 ram_used, swap_used = 0;
    char *ram_section = NULL;
    char *swap_section = NULL;
    char *text;
    int detail_width = 0;
    int graph_width;
    int graph_height;
    int header_width;
    double ram_x = MM_PAD;
    double ram_y = 22.0;
    double swap_y;
    double swap_label_y;
    double swap_text_y;
    double section_hr_y;
    gboolean compact;
    double text_x;
    int text_width;

    /* Клип по округлому контуру ДО заливки фона.
     *
     * Фон заливается прямоугольником на весь размер окна, а маска X-сервера
     * срезает углы уже после того, как всё нарисовано. Без клипа в углах
     * остаются пиксели фона, маска их срежет, и получится рваный угол.
     *
     * Форму X-окна здесь НЕ применяется: render зовётся из rebuild_cache,
     * а на первом draw кэш ещё NULL и mm_draw уходит на ранний return, до
     * render дело не доходит. Форма применяется в mm_draw. */
    cairo_save(cr);
    mm_rounded_path(cr, width, height, priv->corner_radius);
    cairo_clip(cr);

    cairo_set_operator(cr, CAIRO_OPERATOR_SOURCE);
    cairo_set_source_rgba(cr, priv->background_color[0], priv->background_color[1],
                          priv->background_color[2], priv->background_color[3]);
    cairo_paint(cr);
    cairo_set_operator(cr, CAIRO_OPERATOR_OVER);
    font = pango_font_description_from_string(priv->font);
    layout = pango_cairo_create_layout(cr);
    pango_layout_set_font_description(layout, font);
    pango_font_description_free(font);
    compact = height < 200;

    if (priv->sample.valid) {
        int max_text_width;
        int measured_width = 0;

        ram_used = priv->sample.total_kib -
                   MIN(priv->sample.available_kib, priv->sample.total_kib);
        if (priv->sample.swap_total_kib > 0)
            swap_used = priv->sample.swap_total_kib -
                        MIN(priv->sample.swap_free_kib,
                            priv->sample.swap_total_kib);
        ram_section = mm_section_values_text(ram_used, priv->sample.total_kib);
        swap_section = mm_section_values_text(swap_used,
                                              priv->sample.swap_total_kib);
        max_text_width = MAX(1, width - 2 * (int)MM_PAD - 20 - 6);
        pango_layout_set_width(layout, max_text_width * PANGO_SCALE);
        pango_layout_set_ellipsize(layout, PANGO_ELLIPSIZE_END);
        pango_layout_set_text(layout, ram_section, -1);
        pango_layout_get_pixel_size(layout, &measured_width, NULL);
        detail_width = MAX(detail_width, measured_width);
        pango_layout_set_text(layout, swap_section, -1);
        pango_layout_get_pixel_size(layout, &measured_width, NULL);
        detail_width = MAX(detail_width, measured_width);
    }
    if (compact) {
        header_width = mm_compact_content_width(width);
        graph_width = mm_compact_graph_width(width, detail_width);
        graph_height = mm_compact_graph_height(height);
        ram_y = 18.0;
        swap_y = graph_height + 37.0;
        section_hr_y = mm_compact_hr_y(height);
        swap_label_y = graph_height + 24.0;
        swap_text_y = graph_height + 40.0;
    } else {
        header_width = width;
        graph_width = mm_graph_width(width, detail_width, (int)MM_PAD, 6);
        graph_height = mm_graph_height(height);
        ram_y = 22.0;
        swap_y = ram_y + graph_height + 32.0;
        section_hr_y = ram_y + graph_height + 6.0;
        swap_label_y = ram_y + graph_height + 10.0;
        swap_text_y = swap_y + 4.0;
    }
    text_x = ram_x + graph_width + 6.0;
    text_width = MAX(1, width - (int)text_x - (int)MM_PAD);

    if (!priv->sample.valid) {
        pango_layout_set_text(layout, "Waiting for /proc/meminfo...", -1);
        pango_layout_set_width(layout, text_width * PANGO_SCALE);
        pango_layout_set_ellipsize(layout, PANGO_ELLIPSIZE_END);
        mm_set_text_color(cr, priv->text_color);
        cairo_move_to(cr, MM_PAD, MM_PAD);
        pango_cairo_show_layout(cr, layout);
        goto done;
    }

    ram_used = priv->sample.total_kib -
               MIN(priv->sample.available_kib, priv->sample.total_kib);
    if (priv->sample.swap_total_kib > 0)
        swap_used = priv->sample.swap_total_kib -
                    MIN(priv->sample.swap_free_kib, priv->sample.swap_total_kib);

    /* Both histories use the same responsive width and height. */
    {
        const gdouble ram_colors[MM_RAM_COMPONENTS][4] = {
            {priv->ram_color[0], priv->ram_color[1], priv->ram_color[2], priv->ram_color[3]},
            {priv->shared_color[0], priv->shared_color[1], priv->shared_color[2], priv->shared_color[3]},
            {priv->buffers_color[0], priv->buffers_color[1], priv->buffers_color[2], priv->buffers_color[3]},
            {priv->cache_color[0], priv->cache_color[1], priv->cache_color[2], priv->cache_color[3]},
        };

        mm_draw_ram_stack(cr, ram_x, ram_y, graph_width, graph_height,
                          priv->ram_history, priv->ram_head,
                          priv->ram_count, ram_colors,
                          priv->graph_background_color,
                          priv->graph_border_color);
    }
    mm_draw_graph(cr, ram_x, swap_y, graph_width, graph_height,
                  priv->swap_history, priv->swap_head,
                  priv->swap_count, priv->swap_color,
                  priv->graph_background_color,
                  priv->graph_border_color);

    pango_layout_set_width(layout, header_width * PANGO_SCALE);
    pango_layout_set_ellipsize(layout, PANGO_ELLIPSIZE_NONE);
    pango_layout_set_text(layout, "Ram:", -1);
    mm_set_text_color(cr, priv->text_color);
    cairo_move_to(cr, MM_PAD, 4.0);
    pango_cairo_show_layout(cr, layout);
    text = g_strdup_printf("%.0f%%", mm_ram_fraction(&priv->sample) * 100.0);
    pango_layout_set_width(layout, header_width * PANGO_SCALE);
    pango_layout_set_ellipsize(layout, PANGO_ELLIPSIZE_END);
    pango_layout_set_text(layout, text, -1);
    mm_set_text_color(cr, priv->ram_color);
    pango_layout_set_alignment(layout, PANGO_ALIGN_RIGHT);
    cairo_move_to(cr, compact ? mm_percent_anchor_x(width) : 0.0, 4.0);
    pango_cairo_show_layout(cr, layout);
    g_free(text);

    pango_layout_set_width(layout, text_width * PANGO_SCALE);
    pango_layout_set_ellipsize(layout, PANGO_ELLIPSIZE_END);
    pango_layout_set_alignment(layout, PANGO_ALIGN_LEFT);
    pango_layout_set_text(layout, ram_section, -1);
    mm_set_text_color(cr, priv->text_color);
    cairo_move_to(cr, text_x, compact ? 16.0 : 26.0);
    pango_cairo_show_layout(cr, layout);
    {
        int layout_height = 0;
        int values_width = 0;
        int line_count = MAX(pango_layout_get_line_count(layout), 1);
        int line_height;
        pango_layout_get_pixel_size(layout, &values_width, &layout_height);
        line_height = MAX(layout_height / line_count, 1);
        mm_draw_stippled_hr(cr, text_x,
                            (compact ? 16.0 : 26.0) + line_height - 1.0,
                            mm_stippled_width(values_width, text_width),
                            priv->text_color);
    }

    pango_layout_set_width(layout, header_width * PANGO_SCALE);
    pango_layout_set_ellipsize(layout, PANGO_ELLIPSIZE_NONE);
    pango_layout_set_alignment(layout, PANGO_ALIGN_LEFT);
    pango_layout_set_text(layout, "Swap:", -1);
    mm_set_text_color(cr, priv->text_color);
    cairo_move_to(cr, MM_PAD, swap_label_y);
    pango_cairo_show_layout(cr, layout);
    text = g_strdup_printf("%.0f%%", mm_swap_fraction(&priv->sample) * 100.0);
    pango_layout_set_width(layout, header_width * PANGO_SCALE);
    pango_layout_set_ellipsize(layout, PANGO_ELLIPSIZE_END);
    pango_layout_set_text(layout, text, -1);
    mm_set_text_color(cr, priv->swap_color);
    pango_layout_set_alignment(layout, PANGO_ALIGN_RIGHT);
    cairo_move_to(cr, compact ? mm_percent_anchor_x(width) : 0.0,
                  swap_label_y);
    pango_cairo_show_layout(cr, layout);
    g_free(text);

    pango_layout_set_width(layout, text_width * PANGO_SCALE);
    pango_layout_set_ellipsize(layout, PANGO_ELLIPSIZE_END);
    pango_layout_set_alignment(layout, PANGO_ALIGN_LEFT);
    pango_layout_set_text(layout, swap_section, -1);
    mm_set_text_color(cr, priv->text_color);
    cairo_move_to(cr, text_x, swap_text_y);
    pango_cairo_show_layout(cr, layout);
    {
        int layout_height = 0;
        int values_width = 0;
        int line_count = MAX(pango_layout_get_line_count(layout), 1);
        int line_height;
        pango_layout_get_pixel_size(layout, &values_width, &layout_height);
        line_height = MAX(layout_height / line_count, 1);
        mm_draw_stippled_hr(cr, text_x, swap_text_y + line_height - 1.0,
                            mm_stippled_width(values_width, text_width),
                            priv->text_color);
    }
    mm_draw_stippled_hr(cr, MM_PAD, section_hr_y,
                        mm_section_hr_width(width - 2 * (int)MM_PAD),
                        priv->text_color);

done:
    g_free(swap_section);
    g_free(ram_section);
    g_object_unref(layout);
    /* Закрываем clip, открытый в начале render. Без restore контекст уедет
     * с балансом save/restore, и следующий draw начнётся с лишним уровнем. */
    cairo_restore(cr);

    /* Рамка окна - ПОСЛЕ закрытия clip, и это не stylistic выбор.
     *
     * clip в render построен по контуру с отступом 1 px (mm_rounded_path
     * отступает внутрь на пиксель, чтобы дуга не уходила под срез маски).
     * Обводка идёт по контуру с отступом border_width/2. При толщине 1 это
     * 0.5, то есть обводка целиком ложилась бы в полосу, которую clip
     * отбрасывает, и рамка не рисовалась бы ВООБЩЕ. Та же ошибка была в
     * process_list и там же была найдена измерением: при толщине 4 в clip
     * попадали только 3 пикселя из 4.
     *
     * После restore клипа нет, но маска X-сервера по-прежнему режет углы,
     * поэтому за скругление рамка вылезти не может. */
    if (priv->border_width > 0.0) {
        double inset = priv->border_width / 2.0;
        double r = mm_corner_radius_value(priv->corner_radius);

        if (mm_corner_radius_is_rounded(r) && r > inset)
            mm_border_path(cr, width, height, r - inset, inset);
        else
            cairo_rectangle(cr, inset, inset, width - 2 * inset,
                            height - 2 * inset);
        cairo_set_source_rgba(cr, priv->graph_border_color[0],
                              priv->graph_border_color[1],
                              priv->graph_border_color[2],
                              priv->graph_border_color[3]);
        cairo_set_line_width(cr, priv->border_width);
        cairo_stroke(cr);
    }

    cairo_destroy(cr);
    cairo_surface_mark_dirty(surface);
    return surface;
}

static void mm_rebuild_cache(PrivData *priv)
{
    cairo_surface_t *replacement;

    if (!priv->plugin->win)
        return;
    replacement = mm_render(priv, priv->cache_width, priv->cache_height);
    if (priv->cache)
        cairo_surface_destroy(priv->cache);
    priv->cache = replacement;
}

static int mm_init(XsPlugin *p, GKeyFile *kf)
{
    PrivData *priv = g_new0(PrivData, 1);
    int x, y;
    gdouble opacity;

    p->priv = priv;
    priv->plugin = p;
    priv->kf = kf;
    priv->update_ms = (guint)CLAMP(xs_host_api()->conf_int(kf, p->name,
                                       "update_ms", 1000), 100, 60000);
    priv->window_width = CLAMP(xs_host_api()->conf_int(kf, p->name,
                                       "window_width", MM_DEFAULT_WINDOW_WIDTH),
                               100, 1600);
    priv->window_height = CLAMP(xs_host_api()->conf_int(kf, p->name,
                                        "window_height", MM_DEFAULT_WINDOW_HEIGHT),
                                100, 1200);
    priv->corner_radius = CLAMP(xs_host_api()->conf_int(kf, p->name,
                                       "corner_radius", 0), 0, 200);
    priv->font = xs_host_api()->conf_str(kf, p->name, "font", MM_DEFAULT_FONT);
    g_key_file_remove_key(priv->kf, p->name, "ram_graph_height", NULL);
    g_key_file_remove_key(priv->kf, p->name, "swap_graph_height", NULL);
    mm_read_color(priv, "background_color", mm_default_background, priv->background_color);
    mm_read_color(priv, "graph_background_color", mm_default_graph_background,
                  priv->graph_background_color);
    mm_read_color(priv, "text_color", mm_default_text, priv->text_color);
    /* Миграция: border_color раньше означал ЦВЕТ РАМКИ ГРАФИКА, теперь это
     * цвет рамки окна. Старое значение молча переехало бы в рамку окна и
     * выглядело бы как «плагин сам перекрасил окно», поэтому при первом
     * запуске переносим его под новое имя, если нового ключа ещё нет. */
    if (!g_key_file_has_key(priv->kf, p->name, "graph_border_color", NULL) &&
        g_key_file_has_key(priv->kf, p->name, "border_color", NULL)) {
        g_autofree char *old_border = g_key_file_get_string(
            priv->kf, p->name, "border_color", NULL);

        if (old_border) {
            g_key_file_set_string(priv->kf, p->name, "graph_border_color",
                                  old_border);
            g_key_file_remove_key(priv->kf, p->name, "border_color", NULL);
        }
    }
    mm_read_color(priv, "graph_border_color", mm_default_graph_border,
                  priv->graph_border_color);
    mm_read_color(priv, "border_color", mm_default_border, priv->border_color);
    priv->border_width = CLAMP(xs_host_api()->conf_dbl(
        kf, p->name, "border_width", 0.0), 0.0, 4.0);
    mm_read_color(priv, "ram_color", mm_default_ram, priv->ram_color);
    mm_read_color(priv, "shared_color", mm_default_shared, priv->shared_color);
    mm_read_color(priv, "buffers_color", mm_default_buffers, priv->buffers_color);
    mm_read_color(priv, "cache_color", mm_default_cache, priv->cache_color);
    mm_read_color(priv, "swap_color", mm_default_swap, priv->swap_color);

    g_key_file_set_integer(kf, p->name, "update_ms", priv->update_ms);
    g_key_file_set_integer(kf, p->name, "window_width", priv->window_width);
    g_key_file_set_integer(kf, p->name, "window_height", priv->window_height);
    g_key_file_set_integer(kf, p->name, "corner_radius", priv->corner_radius);
    if (!g_key_file_has_key(kf, p->name, "font", NULL))
        g_key_file_set_string(kf, p->name, "font", priv->font);
    xs_core_plugin_conf_flush(p->name);

    x = xs_host_api()->conf_int(kf, p->name, "x", 80);
    y = xs_host_api()->conf_int(kf, p->name, "y", 80);
    opacity = xs_host_api()->conf_dbl(kf, p->name, "opacity", 1.0);
    p->win = xs_host_api()->make_window(p, x, y, priv->window_width,
                                        priv->window_height);
    if (!p->win) {
        p->host->log("memory_monitor: failed to create window");
        g_free(priv->font);
        g_free(priv);
        p->priv = NULL;
        return -1;
    }
    priv->cache_width = priv->window_width;
    priv->cache_height = priv->window_height;
    if (mm_read_meminfo(&priv->sample)) {
        mm_push_ram_components(priv, &priv->sample);
        mm_push(priv->swap_history, &priv->swap_head, &priv->swap_count,
                mm_swap_fraction(&priv->sample));
    }
    priv->cache = mm_render(priv, priv->cache_width, priv->cache_height);
    xs_host_api()->set_opacity(p, CLAMP(opacity, 0.1, 1.0));
    xs_host_api()->set_tick(p, priv->update_ms);
    return 0;
}

static void mm_push_ram_components(PrivData *priv, const MemorySample *sample)
{
    gdouble parts[MM_RAM_COMPONENTS] = {0.0, 0.0, 0.0, 0.0};
    int component;

    mm_ram_components(sample, parts);
    for (component = 0; component < MM_RAM_COMPONENTS; component++)
        mm_push(priv->ram_history[component], &priv->ram_head[component],
                &priv->ram_count[component], parts[component]);
}

static guint mm_tick(XsPlugin *p)
{
    PrivData *priv = p ? p->priv : NULL;

    if (!priv || !p->win)
        return 0;
    if (mm_read_meminfo(&priv->sample)) {
        mm_push_ram_components(priv, &priv->sample);
        mm_push(priv->swap_history, &priv->swap_head, &priv->swap_count,
                mm_swap_fraction(&priv->sample));
    }
    mm_rebuild_cache(priv);
    xs_host_api()->invalidate(p);
    gtk_widget_queue_draw(p->win);
    return priv->update_ms;
}

static void mm_draw(XsPlugin *p, cairo_t *cr, int w, int h)
{
    PrivData *priv = p ? p->priv : NULL;

    if (!priv)
        return;
    /* Форму окна применяем здесь, а не в render.
     *
     * Стоило поставить вызов в render - и маска не применялась вовсе: на
     * первом draw кэш ещё NULL, mm_draw уходит на ранний return, до render
     * дело не доходит, а кэш перестраивается только при смене размера. То
     * есть ни при старте, ни при постоянном размере форма считалась бы
     * ровно ноль раз, и это видно было на сервере как shape из одного
     * прямоугольника.
     *
     * Здесь размеры актуальные, а повторы отсекает кэш формы внутри
     * mm_apply_shape, так что лишней работы на каждый кадр нет. */
    mm_apply_shape(priv, p, w, h);
    if (!priv->cache)
        return;
    if (priv->cache_width != w || priv->cache_height != h) {
        priv->cache_width = w;
        priv->cache_height = h;
        mm_rebuild_cache(priv);
    }
    cairo_set_source_surface(cr, priv->cache, 0, 0);
    cairo_paint(cr);
}

static void mm_shutdown(XsPlugin *p)
{
    PrivData *priv = p ? p->priv : NULL;

    if (!priv)
        return;
    if (priv->cache)
        cairo_surface_destroy(priv->cache);
    g_free(priv->font);
    g_free(priv);
    p->priv = NULL;
}

static void mm_int_changed(GtkSpinButton *spin, gpointer data)
{
    XsPlugin *p = data;
    PrivData *priv = p ? p->priv : NULL;
    const char *key;
    int value;

    if (!priv)
        return;
    key = g_object_get_data(G_OBJECT(spin), "xs-key");
    value = (int)gtk_spin_button_get_value(spin);
    if (strcmp(key, "update_ms") == 0) {
        priv->update_ms = (guint)CLAMP(value, 100, 60000);
        xs_host_api()->set_tick(p, priv->update_ms);
    } else if (strcmp(key, "window_width") == 0)
        priv->window_width = CLAMP(value, 100, 1600);
    else if (strcmp(key, "window_height") == 0)
        priv->window_height = CLAMP(value, 100, 1200);
    else if (strcmp(key, "corner_radius") == 0) {
        priv->corner_radius = CLAMP(value, 0, 200);
        /* Сбрасываем кэш формы, иначе mm_apply_shape увидит прежний
         * shape_radius и решит, что маску пересобирать не нужно. Размер
         * окна при этом не меняется, поэтому resize здесь лишний - форма
         * зависит только от радиуса и габаритов. */
        priv->shape_radius = -1;
        priv->shape_w = 0;
        priv->shape_h = 0;
        g_key_file_set_integer(priv->kf, p->name, key, priv->corner_radius);
        mm_rebuild_cache(priv);
        xs_core_plugin_conf_flush(p->name);
        gtk_widget_queue_draw(p->win);
        return;
    }
    g_key_file_set_integer(priv->kf, p->name, key, value);
    xs_host_api()->resize(p, priv->window_width, priv->window_height);
    priv->cache_width = priv->window_width;
    priv->cache_height = priv->window_height;
    mm_rebuild_cache(priv);
    xs_core_plugin_conf_flush(p->name);
    gtk_widget_queue_draw(p->win);
}

/* Толщина рамки окна из Properties.
 *
 * Кэш перестраивается целиком: рамка рисуется в mm_render, а не поверх
 * готовой surface, поэтому одного queue_draw мало. Форма X-окна при этом
 * не меняется (она зависит только от corner_radius и габаритов), так что
 * кэш формы сбрасывать незачем. */
static void mm_border_width_changed(GtkWidget *widget, gpointer data)
{
    XsPlugin *p = data;
    PrivData *priv = p ? p->priv : NULL;
    const char *key;
    gdouble value;

    if (!priv)
        return;
    key = g_object_get_data(G_OBJECT(widget), "xs-key");
    if (!key)
        return;
    value = gtk_spin_button_get_value(GTK_SPIN_BUTTON(widget));
    priv->border_width = CLAMP(value, 0.0, 4.0);
    g_key_file_set_double(priv->kf, p->name, key, priv->border_width);
    xs_core_plugin_conf_flush(p->name);
    mm_rebuild_cache(priv);
    if (p->win)
        gtk_widget_queue_draw(p->win);
}

static void mm_font_set(GtkFontButton *button, gpointer data)
{
    XsPlugin *p = data;
    PrivData *priv = p ? p->priv : NULL;
    const char *value;

    if (!priv)
        return;
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wdeprecated-declarations"
    value = gtk_font_button_get_font_name(button);
#pragma GCC diagnostic pop
    g_free(priv->font);
    priv->font = g_strdup(value ? value : MM_DEFAULT_FONT);
    g_key_file_set_string(priv->kf, p->name, "font", priv->font);
    mm_rebuild_cache(priv);
    xs_core_plugin_conf_flush(p->name);
    gtk_widget_queue_draw(p->win);
}

static void mm_color_set(GtkColorButton *button, gpointer data)
{
    XsPlugin *p = data;
    PrivData *priv = p ? p->priv : NULL;
    const char *key;
    gdouble *target;
    GdkRGBA color;
    char *value;

    if (!priv)
        return;
    key = g_object_get_data(G_OBJECT(button), "xs-key");
    if (strcmp(key, "background_color") == 0)
        target = priv->background_color;
    else if (strcmp(key, "graph_background_color") == 0)
        target = priv->graph_background_color;
    else if (strcmp(key, "text_color") == 0)
        target = priv->text_color;
    else if (strcmp(key, "graph_border_color") == 0)
        target = priv->graph_border_color;
    else if (strcmp(key, "border_color") == 0)
        target = priv->border_color;
    else if (strcmp(key, "ram_color") == 0)
        target = priv->ram_color;
    else if (strcmp(key, "shared_color") == 0)
        target = priv->shared_color;
    else if (strcmp(key, "buffers_color") == 0)
        target = priv->buffers_color;
    else if (strcmp(key, "cache_color") == 0)
        target = priv->cache_color;
    else
        target = priv->swap_color;
    gtk_color_chooser_get_rgba(GTK_COLOR_CHOOSER(button), &color);
    memcpy(target, &color.red, 3 * sizeof(gdouble));
    target[3] = color.alpha;
    value = mm_color_string(&color);
    g_key_file_set_string(priv->kf, p->name, key, value);
    g_free(value);
    mm_rebuild_cache(priv);
    xs_core_plugin_conf_flush(p->name);
    gtk_widget_queue_draw(p->win);
}

static void mm_add_int(GtkWidget *page, XsPlugin *p, const char *key,
                       const char *label, int value, int min, int max)
{
    GtkWidget *widget = xs_prop_add_int(GTK_BOX(page), label,
                                        "Memory monitor setting", value,
                                        min, max, 1);

    g_object_set_data_full(G_OBJECT(widget), "xs-key", g_strdup(key), g_free);
    g_signal_connect(widget, "value-changed", G_CALLBACK(mm_int_changed), p);
}

static void mm_add_color(GtkWidget *page, XsPlugin *p, const char *key,
                         const char *label, const gdouble color[4])
{
    GtkWidget *widget = xs_prop_add_color(GTK_BOX(page), label,
                                          "Memory monitor RGBA color", color[0],
                                          color[1], color[2], color[3]);

    g_object_set_data_full(G_OBJECT(widget), "xs-key", g_strdup(key), g_free);
    g_signal_connect(widget, "color-set", G_CALLBACK(mm_color_set), p);
}

static void mm_properties(XsPlugin *p, GtkNotebook *notebook)
{
    PrivData *priv = p ? p->priv : NULL;
    GtkWidget *page;
    GtkWidget *font;

    if (!priv)
        return;
    page = gtk_box_new(GTK_ORIENTATION_VERTICAL, 4);
    gtk_container_set_border_width(GTK_CONTAINER(page), 10);
    xs_prop_add_group_header(GTK_BOX(page),
                             "RAM and swap histories are sampled from /proc/meminfo; each tick scrolls both graphs right-to-left.");
    mm_add_int(page, p, "update_ms", "Update (ms)", priv->update_ms, 100, 60000);
    mm_add_int(page, p, "window_width", "Window width", priv->window_width, 100, 1600);
    mm_add_int(page, p, "window_height", "Window height", priv->window_height, 100, 1200);
    mm_add_int(page, p, "corner_radius", "Corner radius", priv->corner_radius, 0, 200);
    font = xs_prop_add_font(GTK_BOX(page), "Font", "Monitor text font", priv->font);
    g_signal_connect(font, "font-set", G_CALLBACK(mm_font_set), p);
    mm_add_color(page, p, "background_color", "Background", priv->background_color);
    mm_add_color(page, p, "graph_background_color", "Graph background",
                 priv->graph_background_color);
    mm_add_color(page, p, "text_color", "Text", priv->text_color);
    mm_add_color(page, p, "graph_border_color", "Graph border",
                 priv->graph_border_color);
    mm_add_color(page, p, "border_color", "Frame color", priv->border_color);
    {
        /* Ползунок с шагом 1 и нулём цифр: толщина рамки - целое число
         * пикселей, дробная часть мешает и визуально, и при записи в
         * конфиг. */
        GtkWidget *bw = xs_prop_add_float(GTK_BOX(page), "Frame width",
                                          "Window frame thickness in pixels. "
                                          "0 = no frame.",
                                          priv->border_width, 0.0, 4.0, 1.0, 0);

        g_object_set_data_full(G_OBJECT(bw), "xs-key",
                               g_strdup("border_width"), g_free);
        g_signal_connect(bw, "value-changed",
                         G_CALLBACK(mm_border_width_changed), p);
    }
    mm_add_color(page, p, "ram_color", "RAM user/apps", priv->ram_color);
    mm_add_color(page, p, "shared_color", "RAM shared", priv->shared_color);
    mm_add_color(page, p, "buffers_color", "RAM buffers", priv->buffers_color);
    mm_add_color(page, p, "cache_color", "RAM cache", priv->cache_color);
    mm_add_color(page, p, "swap_color", "Swap history", priv->swap_color);
    gtk_notebook_append_page(notebook, page, gtk_label_new(_("Memory Monitor")));
    gtk_widget_show_all(page);
}

static const XsPluginOps mm_ops = {
    .init = mm_init,
    .draw = mm_draw,
    .tick = mm_tick,
    .button = NULL,
    .motion = NULL,
    .shutdown = mm_shutdown,
    .menu = NULL,
    .menu_cmd = NULL,
    .properties = mm_properties,
    .fill_themes = NULL,
    .scroll = NULL,
    .enter = NULL,
    .leave = NULL,
    .guest_list_changed = NULL,
};

static XsPluginDesc mm_desc = {
    "memory_monitor",
    XS_API_VERSION,
    &mm_ops,
    N_("System RAM and swap monitor with independent scrolling histories"),
    "kosmik2001 <kosmik2001@gmail.com>",
    "1.0"
};

XS_PLUGIN_EXPORT(&mm_desc)
