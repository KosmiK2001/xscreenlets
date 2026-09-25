/* disk_monitor.c — one stable /dev/disk/by-id whole disk per applet. */
#include <gtk/gtk.h>
#include <gio/gio.h>
#include <limits.h>
#include <sys/stat.h>
#include <sys/sysmacros.h>
#include <stdlib.h>
#include <string.h>
#include "xs_api.h"
#include "common.h"
#include "disk_monitor_core.h"

#define DM_DEFAULT_WIDTH 420
#define DM_DEFAULT_FONT "Sans 8"
#define DM_BY_ID_DIR "/dev/disk/by-id"
/* The graph fills the whole window: the label and the current values are
 * drawn on top of it, so nothing is reserved for them. */
#define DM_GRAPH_TOP 0
/* how far the border arc is pulled inside the quantised X shape cut */
#define DM_BORDER_PATH_INSET 0.5
#define DM_DISKSTATS "/proc/diskstats"
#define DM_CTX_KEY "xs-disk-monitor-ctx"

typedef struct {
    XsPlugin *plugin;
    GKeyFile *kf;
    char *graph_label;
    char *font;
    char *label_font;
    char *read_font, *write_font, *temp_font;
    int width, height;
    int corner_radius;
    /* last (radius,w,h) pushed to the X server as the window shape; the
     * shape call is not free, so skip it when nothing changed */
    int shape_radius, shape_w, shape_h;
    /* The window size the text anchors were laid out for: anchors are stored
     * against it and mapped to the live size on every frame. */
    int design_width, design_height;
    guint update_ms;
    int read_x, read_y, write_x, write_y, temp_x, temp_y;
    int label_x, label_y;
    gboolean show_temperature_history;
    gdouble graph_bg[4], border[4], text_color[4];
    gdouble read_color[4], write_color[4], temp_color[4];
    gdouble read_text_color[4], write_text_color[4], temp_text_color[4];
    /* The monitored device and all of its sampled state live in ONE struct so
     * a disk switch can be prepared in a temporary copy and committed whole. */
    DmDeviceState device;
    cairo_surface_t *cache;
    int cache_width, cache_height;
    guint64 generation;
    GTask *hddtemp_task;
    GCancellable *hddtemp_cancellable;
} PrivData;

static const gdouble dm_graph_bg_default[4] = {0.02, 0.03, 0.05, 1.0};
static const gdouble dm_border_default[4] = {0.55, 0.58, 0.62, 1.0};
static const gdouble dm_text_default[4] = {1, 1, 1, 1};
static const gdouble dm_read_default[4] = {0.20, 0.75, 1.0, 1.0};
static const gdouble dm_write_default[4] = {0.25, 0.85, 0.35, 1.0};
static const gdouble dm_temp_default[4] = {1.0, 0.35, 0.12, 1.0};

/* Text values are drawn over the history fill, so they carry a dark shadow. */
#define DM_TEXT_SHADOW_RADIUS 3
#define DM_TEXT_SHADOW_ALPHA 0.9

static void dm_shutdown(XsPlugin *p);
static void dm_rebuild(PrivData *priv);

/* Resolve the LIVE plugin from the dialog context's stable instance name. A
 * Properties dialog can outlive a plugin restart, so no callback may hold an
 * XsPlugin * or PrivData * captured when the dialog was built. */
static PrivData *dm_live_priv(DmDialogContext *ctx)
{
    XsPlugin *live = ctx ? xs_core_find_instance(dm_dialog_context_name(ctx)) : NULL;
    return live ? live->priv : NULL;
}

static char *dm_color_text(const gdouble rgba[4])
{
    return dm_format_rgba(rgba);
}

static void dm_read_color(PrivData *priv, const char *key,
                          const gdouble fallback[4], gdouble out[4])
{
    char *text = xs_host_api()->conf_str(priv->kf, priv->plugin->name, key, NULL);
    char *replacement;
    gboolean migrated = FALSE;

    memcpy(out, fallback, 4 * sizeof(gdouble));
    if (!text || !dm_parse_rgba(text, out)) {
        if (text)
            priv->plugin->host->log("disk_monitor %s: malformed %s migrated to default",
                                    priv->plugin->name, key);
        replacement = dm_color_text(fallback);
        g_key_file_set_string(priv->kf, priv->plugin->name, key, replacement);
        g_free(replacement);
        memcpy(out, fallback, 4 * sizeof(gdouble));
        migrated = TRUE;
    }
    g_free(text);
    if (migrated)
        xs_core_plugin_conf_flush(priv->plugin->name);
}

static GPtrArray *dm_discover_disks(void)
{
    GPtrArray *result = g_ptr_array_new_with_free_func(g_free);
    GDir *dir = g_dir_open(DM_BY_ID_DIR, 0, NULL);
    const char *name;
    GPtrArray *names = g_ptr_array_new_with_free_func(g_free);
    GHashTable *seen = g_hash_table_new_full(g_str_hash, g_str_equal, g_free, NULL);
    guint i;

    if (!dir) {
        g_hash_table_destroy(seen);
        return result;
    }
    while ((name = g_dir_read_name(dir))) {
        char *full;
        if (!dm_by_id_link_name_is_candidate(name))
            continue;
        full = g_build_filename(DM_BY_ID_DIR, name, NULL);
        if (dm_by_id_path_is_valid_configured(full) && dm_by_id_is_whole_disk(full))
            g_ptr_array_add(names, g_strdup(name));
        g_free(full);
    }
    g_dir_close(dir);
    g_ptr_array_sort(names, (GCompareFunc)g_strcmp0);
    for (i = 0; i < names->len; i++) {
        char *full = g_build_filename(DM_BY_ID_DIR, g_ptr_array_index(names, i), NULL);
        char *real = realpath(full, NULL);
        if (real && dm_resolved_device_seen(seen, real))
            g_ptr_array_add(result, full);
        else
            g_free(full);
        free(real);
    }
    g_ptr_array_free(names, TRUE);
    g_hash_table_destroy(seen);
    return result;
}

/* Blocker 1: order-independent selection by the (driver rank, label rank)
 * tuple — directory order can never make Sensor 1 beat NVMe Composite. */
static char *dm_find_hwmon_temp(DmDeviceState *device, char **secondary)
{
    GDir *dir;
    const char *entry;
    char *device_dir, *best = NULL, *second = NULL;
    int best_driver = 9, best_label = 9;
    int second_driver = 9, second_label = 9;

    if (secondary)
        *secondary = NULL;
    if (!device->device_name)
        return NULL;
    device_dir = g_strdup_printf("/sys/class/block/%s/device", device->device_name);
    dir = g_dir_open(device_dir, 0, NULL);
    g_free(device_dir);
    if (!dir)
        return NULL;
    while ((entry = g_dir_read_name(dir))) {
        char *child, *name_path, *hwmon_name = NULL;
        GDir *hdir;
        const char *file;
        int driver_rank;
        if (!g_str_has_prefix(entry, "hwmon"))
            continue;
        child = g_build_filename("/sys/class/block", device->device_name,
                                 "device", entry, NULL);
        name_path = g_build_filename(child, "name", NULL);
        if (g_file_get_contents(name_path, &hwmon_name, NULL, NULL))
            g_strstrip(hwmon_name);
        if (hwmon_name && !strcmp(hwmon_name, "drivetemp")) driver_rank = 0;
        else if (hwmon_name && !strcmp(hwmon_name, "nvme")) driver_rank = 1;
        else driver_rank = 9;
        hdir = driver_rank < 9 ? g_dir_open(child, 0, NULL) : NULL;
        if (hdir) {
            while ((file = g_dir_read_name(hdir))) {
                char *label_path, *label = NULL, *label_base, *label_name;
                int label_rank;
                if (!g_str_has_prefix(file, "temp") || !strstr(file, "_input"))
                    continue;
                label_base = g_strndup(file, strlen(file) - strlen("_input"));
                label_name = g_strconcat(label_base, "_label", NULL);
                g_free(label_base);
                label_path = g_build_filename(child, label_name, NULL);
                g_free(label_name);
                if (g_file_get_contents(label_path, &label, NULL, NULL))
                    g_strstrip(label);
                label_rank = dm_hwmon_temp_priority(hwmon_name, label);
                if (dm_hwmon_candidate_preferred(driver_rank, label_rank,
                                                 best_driver, best_label)) {
                    /* The previous best becomes the second reading, so a drive
                     * with a composite and a second sensor reports both. */
                    g_free(second);
                    second = best;
                    second_driver = best_driver;
                    second_label = best_label;
                    best = g_build_filename(child, file, NULL);
                    best_driver = driver_rank;
                    best_label = label_rank;
                } else if (!second ||
                           dm_hwmon_candidate_preferred(driver_rank, label_rank,
                                                        second_driver,
                                                        second_label)) {
                    g_free(second);
                    second = g_build_filename(child, file, NULL);
                    second_driver = driver_rank;
                    second_label = label_rank;
                }
                g_free(label_path);
                g_free(label);
            }
            g_dir_close(hdir);
        }
        g_free(name_path);
        g_free(hwmon_name);
        g_free(child);
    }
    g_dir_close(dir);
    if (secondary)
        *secondary = second;
    else
        g_free(second);
    return best;
}

static void dm_hddtemp_thread(GTask *task, gpointer source,
                              gpointer task_data, GCancellable *cancellable)
{
    DmTemperatureRequest *request = task_data;
    gint result = G_MININT;
    (void)source;
    (void)cancellable;
    if (!g_task_return_error_if_cancelled(task))
        result = dm_hddtemp_query("127.0.0.1", 7634,
                                  request->device_path, 500);
    g_task_return_int(task, result);
}

static void dm_hddtemp_done(GObject *source, GAsyncResult *result,
                            gpointer user_data)
{
    DmTemperatureRequest *request = user_data;
    gint temperature = G_MININT;
    GError *error = NULL;
    XsPlugin *live;
    PrivData *priv;

    (void)source;
    if (g_task_is_valid(result, NULL))
        temperature = g_task_propagate_int(G_TASK(result), &error);
    g_clear_error(&error);

    live = request->instance_name ? xs_core_find_instance(request->instance_name) : NULL;
    priv = live ? live->priv : NULL;
    if (priv && dm_hddtemp_completion_is_current(
                    priv->hddtemp_task, G_TASK(result))) {
        priv->hddtemp_task = NULL;
        g_clear_object(&priv->hddtemp_cancellable);
        if (dm_temperature_completion_apply(&priv->device.sample, request,
                                            temperature, priv->plugin->name,
                                            priv->generation,
                                            priv->device.by_id,
                                            priv->device.device_path,
                                            priv->device.resolved)) {
            priv->device.cached_hddtemp_valid = temperature != G_MININT;
            priv->device.cached_hddtemp_milli = temperature;
            dm_rebuild(priv);
            if (priv->plugin->win)
                gtk_widget_queue_draw(priv->plugin->win);
        }
    }
    dm_temperature_request_clear(request);
    g_object_unref(request);
}

static void dm_start_hddtemp(PrivData *priv, gint64 now)
{
    DmTemperatureRequest *request;
    DmDeviceState *device = &priv->device;

    if (!dm_hddtemp_start_allowed(priv->hddtemp_task, device->resolved,
                                  device->by_id && device->device_path,
                                  dm_hddtemp_due(device->last_hddtemp_attempt_us, now)))
        return;
    device->last_hddtemp_attempt_us = now;
    request = g_new0(DmTemperatureRequest, 1);
    request->instance_name = g_strdup(priv->plugin->name);
    request->by_id = g_strdup(device->by_id);
    request->device_path = g_strdup(device->device_path);
    request->generation = priv->generation;
    request->request_id = (guint64)g_random_int();
    priv->hddtemp_cancellable = g_cancellable_new();
    priv->hddtemp_task = g_task_new(NULL, priv->hddtemp_cancellable,
                                    dm_hddtemp_done, request);
    g_task_set_task_data(priv->hddtemp_task, request, NULL);
    g_task_run_in_thread(priv->hddtemp_task, dm_hddtemp_thread);
}

static void dm_sample(PrivData *priv)
{
    DmDeviceState *device = &priv->device;
    gint64 now = g_get_monotonic_time();
    char *text = NULL, *temp_path;
    guint64 read, write;
    gboolean diskstats_sampled = FALSE;
    guint64 elapsed = now > device->history.previous_time_us
                          ? (guint64)(now - device->history.previous_time_us) : 0;

    /* Blocker 3: an unusable sample starts from a clean slate, so a failed
     * read shows N/A instead of the previous tick's rate. */
    dm_sample_mark_invalid(&device->sample);
    if (!device->resolved) {
        /* Symmetric with temp_path: the second sensor must not outlive an
         * unusable sample, otherwise a vanished disk keeps a stale reading. */
        dm_device_state_secondary_reset(device);
        goto histories;
    }
    if (g_file_get_contents(DM_DISKSTATS, &text, NULL, NULL) &&
        dm_parse_diskstats_named(text, device->device_name, &read, &write)) {
        read = dm_sectors_to_bytes(read);
        write = dm_sectors_to_bytes(write);
        if (device->history.previous_valid && elapsed > 0) {
            gint64 read_rate = dm_rate_bytes_per_second(
                device->history.previous_read, read, elapsed);
            gint64 write_rate = dm_rate_bytes_per_second(
                device->history.previous_write, write, elapsed);
            if (read_rate > 0 || device->history.previous_read == read)
                device->sample.read_bytes = (guint64)read_rate;
            if (write_rate > 0 || device->history.previous_write == write)
                device->sample.write_bytes = (guint64)write_rate;
            device->sample.io_valid = TRUE;
        }
        device->history.previous_read = read;
        device->history.previous_write = write;
        device->history.previous_valid = TRUE;
        diskstats_sampled = TRUE;
    } else {
        dm_diskstats_sample_failed(&device->history, now);
    }
    g_clear_pointer(&text, g_free);
    temp_path = device->temp_path;
    if (!temp_path || !g_file_test(temp_path, G_FILE_TEST_EXISTS)) {
        g_clear_pointer(&device->temp_path, g_free);
        g_clear_pointer(&device->temp_path_secondary, g_free);
        device->secondary_milli = G_MININT;
        temp_path = device->temp_path = dm_find_hwmon_temp(device,
                                                          &device->temp_path_secondary);
    }
    if (temp_path && g_file_get_contents(temp_path, &text, NULL, NULL)) {
        device->sample.temperature_milli = dm_parse_temperature(text);
        device->sample.temperature_valid = device->sample.temperature_milli != G_MININT;
    }
    g_clear_pointer(&text, g_free);
    if (device->temp_path_secondary &&
        g_file_get_contents(device->temp_path_secondary, &text, NULL, NULL))
        device->secondary_milli = dm_parse_temperature(text);
    else
        device->secondary_milli = G_MININT;
    g_clear_pointer(&text, g_free);
    if (!device->sample.temperature_valid)
        dm_start_hddtemp(priv, now);
    else if (device->cached_hddtemp_valid) {
        device->sample.temperature_valid = TRUE;
        device->sample.temperature_milli = device->cached_hddtemp_milli;
    }
    if (diskstats_sampled)
        device->history.previous_time_us = now;

histories:
    /* Blocker 2: EVERY sample advances all three rings, so the shared
     * bytes/s x-axis and the temperature overlay stay in lockstep. A missing
     * temperature is the G_MININT marker, never a skipped column. */
    dm_push_history(device->history.read, &device->history.read_head,
                    &device->history.read_count, device->sample.read_bytes);
    dm_push_history(device->history.write, &device->history.write_head,
                    &device->history.write_count, device->sample.write_bytes);
    dm_push_temp_history(device->history.temp, &device->history.temp_head,
                         &device->history.temp_count,
                         device->sample.temperature_valid
                             ? device->sample.temperature_milli : G_MININT);
}

static void dm_show_text(cairo_t *cr, PangoLayout *layout,
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
    /* Rescaling the anchor keeps the value in the same relative spot, but a
     * long string at a legal x can still run past the edge: fit it by its own
     * measured width. The two axes clamp independently — sharing one limit
     * would leave the vertical guard dead on any window taller than it is
     * wide. */
    /* Fit against the SHADOW extent, not the glyph box: the outline is drawn
     * DM_TEXT_SHADOW_RADIUS px beyond the glyphs in all eight directions, so a
     * fit computed on text_w alone lets the value sit legally inside the window
     * while its halo already reaches the frame. Inflating the extent by the
     * radius twice on each side makes the configured anchor the last legal
     * position, and the rounded clip still eats anything the user forced past
     * it by hand. */
    x = dm_fit_text_coordinate(x, text_w + 2 * DM_TEXT_SHADOW_RADIUS, width);
    /* Same shadow margin on the vertical axis, and the two axes are clamped
     * independently (sharing one limit would leave the vertical guard dead on
     * any window taller than it is wide). */
    if (height > 0 && y > height - 1 - DM_TEXT_SHADOW_RADIUS)
        y = height - 1 - DM_TEXT_SHADOW_RADIUS;
    if (y < DM_TEXT_SHADOW_RADIUS)
        y = DM_TEXT_SHADOW_RADIUS;
    /* The history fill can sit right behind the values, so a dark shadow is
     * laid down first in eight directions and the value is drawn on top. A
     * cairo_stroke over the glyph path would only put half its width inside
     * the glyph — invisible at 8 pt, and it would hollow the letter out. */
    {
        static const int offsets[8][2] = {{-1, -1}, {0, -1}, {1, -1}, {-1, 0},
                                          {1, 0}, {-1, 1}, {0, 1}, {1, 1}};
        int i;
        cairo_set_source_rgba(cr, 0.0, 0.0, 0.0, DM_TEXT_SHADOW_ALPHA);
        for (i = 0; i < 8; i++) {
            cairo_move_to(cr, x + offsets[i][0] * DM_TEXT_SHADOW_RADIUS,
                          y + offsets[i][1] * DM_TEXT_SHADOW_RADIUS);
            pango_cairo_show_layout(cr, layout);
        }
    }
    cairo_set_source_rgba(cr, color[0], color[1], color[2], color[3]);
    cairo_move_to(cr, x, y);
    pango_cairo_show_layout(cr, layout);
}

static void dm_draw_series(cairo_t *cr, const guint64 *history, guint head,
                           guint count, guint columns, int plot_width,
                           guint64 scale_max, double x, double y, int height,
                           const gdouble color[4])
{
    guint i;
    double origin = x + dm_history_origin_x(plot_width, columns);
    double baseline = y + height;
    gdouble fill[4];

    /* The user's own alpha is honoured: the colour button exposes an alpha
     * scale, so a fixed 0.55 here would make that control a lie. */
    dm_fill_rgba(color, color[3], fill);
    cairo_set_source_rgba(cr, fill[0], fill[1], fill[2], fill[3]);
    if (columns == 0)
        return;
    /* The area must be closed by two verticals and a baseline. cairo_close_path
     * alone would join the last point straight back to the first, drawing a
     * diagonal across the plot — and where the polyline crosses it, the
     * winding rule punches black holes through the fill. */
    {
        double right_x = origin + (double)(columns - 1);
        double first_y = y + height - height * (double)MIN(dm_history_value(history, head, count, 0), scale_max) / (double)scale_max;
        cairo_new_sub_path(cr);
        cairo_move_to(cr, right_x, baseline);
        cairo_line_to(cr, right_x, first_y);
        for (i = 0; i < columns; i++) {
            guint64 value = dm_history_value(history, head, count, i);
            double point_x = origin + (double)(columns - 1 - i);
            double point_y = y + height - height * (double)MIN(value, scale_max) / (double)scale_max;

            cairo_line_to(cr, point_x, point_y);
        }
        cairo_line_to(cr, origin, baseline);
        cairo_close_path(cr);
    }
    cairo_fill(cr);
}

/* Build the rounded outline of the window into cr as a path (not filled).
 * The X window itself is already cut to this shape by the shape mask, so this
 * path is only what makes the CUT look deliberate: the graph background and the
 * border are painted along the arc instead of being sliced off square. */
static void dm_rounded_path(cairo_t *cr, int width, int height, int radius,
                          double border_inset)
{
    /* Inset by one pixel: the shape mask cuts exactly at the window edge, so a
     * path drawn ON the edge loses half its stroke to the cut and the border
     * comes out half as thick as the one-pixel line we ask for. */
    const double inset = 1.0;
    double w = width - 2 * inset, h = height - 2 * inset;
    double r = dm_corner_radius_value(radius);

    if (w <= 0 || h <= 0) {
        cairo_rectangle(cr, 0, 0, width, height);
        return;
    }
    if (!dm_corner_radius_is_rounded(r)) {
        cairo_rectangle(cr, inset, inset, w, h);
        return;
    }
    r = MIN(r, MIN(w, h) / 2.0);
    /* The border arc must stay just inside the shape cut. dm_rounded_region()
     * cuts with floor(), i.e. never deeper than the true arc, so a small
     * inset is enough to keep the stroke out of the removed band while leaving
     * the smooth cairo edge visible. */
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

static cairo_surface_t *dm_render(PrivData *priv, int width, int height)
{
    DmDeviceState *device = &priv->device;
    cairo_surface_t *surface = cairo_image_surface_create(CAIRO_FORMAT_ARGB32, width, height);
    cairo_t *cr = cairo_create(surface);
    PangoLayout *layout = pango_cairo_create_layout(cr);
    guint available = MAX(device->history.read_count,
                          MAX(device->history.write_count, device->history.temp_count));
    guint columns = dm_history_columns(width - 4, available);
    gint min_temp = G_MAXINT, max_temp = G_MININT, t_lo = 0, t_hi = 0;
    guint64 scale_max = 1;
    guint i;
    char *read_text, *write_text;
    int graph_y, graph_h, relative_y, relative_h;
    double temp_span;

    pango_layout_set_font_description(layout, NULL);
    /* Rounded corners: clip the whole window, so the base fill, the graph and
     * the border all stop at the rounded outline instead of the fill spilling
     * into square corners. A radius of 0 leaves the path unclipped. */
    {
        const double radius = dm_corner_radius_value(priv->corner_radius);

        if (dm_corner_radius_is_rounded(radius)) {
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
    /* No separate base layer: the graph covers the whole window, so a second
     * background behind it could only show up where the graph does not reach
     * and was a source of a stray colour nobody could configure any more. */
    cairo_set_operator(cr, CAIRO_OPERATOR_SOURCE);
    cairo_set_source_rgba(cr, 0.0, 0.0, 0.0, 0.0);
    cairo_paint(cr);
    cairo_set_operator(cr, CAIRO_OPERATOR_OVER);

    /* gkrellm contract: both series share ONE bytes/s scale taken from the
     * max of the two visible histories, so the two lines stay comparable. */
    for (i = 0; i < columns; i++) {
        guint64 r = dm_history_value(device->history.read, device->history.read_head,
                                     device->history.read_count, i);
        guint64 w = dm_history_value(device->history.write, device->history.write_head,
                                     device->history.write_count, i);
        if (r > scale_max) scale_max = r;
        if (w > scale_max) scale_max = w;
        /* G_MININT markers never reach the temperature scale. */
        if (dm_temp_history_is_valid(device->history.temp, device->history.temp_head,
                                     device->history.temp_count, i)) {
            gint t = dm_temp_history_value(device->history.temp,
                                           device->history.temp_head,
                                           device->history.temp_count, i);
            if (t < min_temp) min_temp = t;
            if (t > max_temp) max_temp = t;
        }
    }
    graph_y = DM_GRAPH_TOP;
    graph_h = MAX(1, height - graph_y - 2);
    dm_chart_geometry(graph_h, &relative_y, &relative_h);
    graph_y += relative_y;
    graph_h = relative_h;

    cairo_set_source_rgba(cr, priv->graph_bg[0], priv->graph_bg[1], priv->graph_bg[2],
                          priv->graph_bg[3]);
    /* Fill inside the rounded outline, so the cut corners show the graph
     * background reaching the arc rather than a raw square slice. */
    cairo_save(cr);
    dm_rounded_path(cr, width, height, priv->corner_radius, 0.0);
    cairo_set_source_rgba(cr, priv->graph_bg[0], priv->graph_bg[1], priv->graph_bg[2],
                          priv->graph_bg[3]);
    cairo_fill(cr);
    cairo_restore(cr);
    cairo_save(cr);
    cairo_rectangle(cr, 2, graph_y + 1, width - 4, graph_h - 2);
    cairo_clip(cr);
    /* Intersect with the rounded outline: without this the series are painted
     * right up to the window edge and stick out over the cut corners as square
     * blocks, which is exactly the leftover "raw transparency" artefact. */
    dm_rounded_path(cr, width, height, priv->corner_radius, 0.0);
    cairo_clip(cr);
    dm_draw_series(cr, device->history.write, device->history.write_head,
                   device->history.write_count, columns, width - 4, scale_max,
                   2.0, graph_y + 1.0, graph_h - 2.0, priv->write_color);
    dm_draw_series(cr, device->history.read, device->history.read_head,
                   device->history.read_count, columns, width - 4, scale_max,
                   2.0, graph_y + 1.0, graph_h - 2.0, priv->read_color);
    /* Temperature overlay: the line is drawn on the SAME columns, and a
     * missing sample breaks the polyline instead of bridging the gap. */
    if (dm_temp_scale(min_temp, max_temp, &t_lo, &t_hi) &&
        priv->show_temperature_history) {
        gboolean started = FALSE;
        temp_span = (double)(t_hi - t_lo);
        cairo_set_source_rgba(cr, priv->temp_color[0], priv->temp_color[1],
                              priv->temp_color[2], priv->temp_color[3]);
        cairo_set_line_width(cr, 1.0);
        for (i = 0; i < columns; i++) {
            gint t;
            double x, y;
            if (!dm_temp_history_is_valid(device->history.temp,
                                          device->history.temp_head,
                                          device->history.temp_count, i)) {
                started = FALSE;
                continue;
            }
            t = dm_temp_history_value(device->history.temp, device->history.temp_head,
                                      device->history.temp_count, i);
            x = 2.0 + dm_history_origin_x(width - 4, columns) + (double)(columns - 1 - i);
            y = graph_y + 1 + (t_hi - t) / temp_span * (graph_h - 2);
            if (started) cairo_line_to(cr, x, y);
            else { cairo_move_to(cr, x, y); started = TRUE; }
        }
        cairo_stroke(cr);
    }
    cairo_restore(cr);
    cairo_set_source_rgba(cr, priv->border[0], priv->border[1], priv->border[2], priv->border[3]);
    cairo_set_line_width(cr, 1.0);
    cairo_save(cr);
    dm_rounded_path(cr, width, height, priv->corner_radius, DM_BORDER_PATH_INSET);
    cairo_stroke(cr);
    cairo_restore(cr);

    /* Label and the three current values, drawn ON the graph. The anchors are
     * design coordinates mapped to the live window, so a resize keeps every
     * value in the same relative spot instead of pushing it out of frame. */
    {
        /* Text near a corner must not spill over the rounded frame. Clip the
         * whole text pass to the same outline the border and fill use, so any
         * glyph that reaches into the cut is simply not painted — no manual
         * per-label bounds to keep in sync. */
        cairo_save(cr);
        dm_rounded_path(cr, width, height, priv->corner_radius, 0.0);
        cairo_clip(cr);
        const int dw = priv->design_width > 0 ? priv->design_width : width;
        const int dh = priv->design_height > 0 ? priv->design_height : height;
        const int label_x = dm_scale_position(priv->label_x, dw, width,
                                                width - 1);
        const int label_y = dm_scale_position(priv->label_y, dh, height,
                                                height - 1);
        const int read_x = dm_scale_position(priv->read_x, dw, width,
                                               width - 1);
        const int read_y = dm_scale_position(priv->read_y, dh, height,
                                               height - 1);
        const int write_x = dm_scale_position(priv->write_x, dw, width,
                                                 width - 1);
        const int write_y = dm_scale_position(priv->write_y, dh, height,
                                                 height - 1);
        const int temp_x = dm_scale_position(priv->temp_x, dw, width,
                                                width - 1);
        const int temp_y = dm_scale_position(priv->temp_y, dh, height,
                                                height - 1);
        read_text = priv->device.sample.io_valid
                        ? dm_format_rate(priv->device.sample.read_bytes)
                        : g_strdup("N/A");
        write_text = priv->device.sample.io_valid
                         ? dm_format_rate(priv->device.sample.write_bytes)
                         : g_strdup("N/A");
        /* Two sensors stack one under the other at the same anchor, so the
         * value column never grows sideways. */
        {
            gint readings[2] = {priv->device.sample.temperature_valid
                                    ? priv->device.sample.temperature_milli
                                    : G_MININT,
                                priv->device.secondary_milli};
            guint lines = 0, li;
            char **temp_lines = dm_format_temperature_lines(readings, 2, &lines);
            PangoContext *pctx = pango_cairo_create_context(cr);
            PangoLayout *probe = pango_layout_new(pctx);
            PangoFontDescription *font_desc =
                pango_font_description_from_string(priv->temp_font);
            int step;
            int th = 0;

            pango_layout_set_font_description(probe, font_desc);
            pango_layout_set_text(probe, "0", -1);
            pango_layout_get_pixel_size(probe, NULL, &th);
            /* Stack by the real font height, with a floor for tiny sizes. */
            step = MAX(11, th);
            pango_font_description_free(font_desc);
            g_object_unref(probe);
            g_object_unref(pctx);
            for (li = 0; li < lines; li++) {
                int line_y = temp_y + (int)li * step;
                if (line_y > height - 1)
                    line_y = height - 1;
                dm_show_text(cr, layout, priv->temp_font, temp_x, line_y,
                             temp_lines[li], priv->temp_text_color, width, height);
            }
            g_strfreev(temp_lines);
        }
        dm_show_text(cr, layout, priv->label_font, label_x, label_y,
                     priv->graph_label, priv->text_color, width, height);
        dm_show_text(cr, layout, priv->read_font, read_x, read_y,
                     read_text, priv->read_text_color, width, height);
        dm_show_text(cr, layout, priv->write_font, write_x, write_y,
                     write_text, priv->write_text_color, width, height);
        g_free(read_text); g_free(write_text);
        cairo_restore(cr);
    }
    g_object_unref(layout); cairo_destroy(cr); cairo_surface_mark_dirty(surface);
    return surface;
}

static void dm_rebuild(PrivData *priv)
{
    cairo_surface_t *replacement;
    if (!priv->plugin->win) return;
    replacement = dm_render(priv, priv->cache_width, priv->cache_height);
    if (priv->cache) cairo_surface_destroy(priv->cache);
    priv->cache = replacement;
}

/* Blocker 4: a persisted by-id path that does not resolve (temporarily
 * unplugged, renamed by udev, absent at boot) must NOT fail init. The
 * identity stays in the config, the display shows N/A, and the symlink is
 * re-resolved at a bounded interval until the device comes back. */
static void dm_try_resolve(PrivData *priv, gboolean force)
{
    DmDeviceState *device = &priv->device;
    gint64 now = g_get_monotonic_time();

    if (!force && !dm_retry_due(device->last_resolve_attempt_us, now))
        return;
    device->last_resolve_attempt_us = now;
    if (!force && device->resolved &&
        dm_resolved_target_matches(device->by_id, device->device_path) == DM_RESOLUTION_MATCH)
        return;
    if (device->resolved) {
        /* The stable identity moved or vanished. Make the old kernel target
         * unavailable immediately and clear its counters before re-resolution. */
        device->resolved = FALSE;
        g_clear_pointer(&device->device_name, g_free);
        g_clear_pointer(&device->device_path, g_free);
        g_clear_pointer(&device->temp_path, g_free);
        dm_device_state_secondary_reset(device);
        dm_sample_mark_invalid(&device->sample);
        dm_history_clear(&device->history);
    }
    if (device->by_id && dm_device_state_prepare(device, device->by_id)) {
        priv->plugin->host->log("disk_monitor %s: resolved %s -> %s",
                                priv->plugin->name, device->by_id, device->device_name);
        dm_history_clear(&device->history);
        device->last_hddtemp_attempt_us = 0;
        device->cached_hddtemp_valid = FALSE;
        device->cached_hddtemp_milli = G_MININT;
    } else if (!device->resolved) {
        priv->plugin->host->log("disk_monitor %s: %s unresolved, retrying every 60s",
                                priv->plugin->name, device->by_id ? device->by_id : "(none)");
    }
}

static int dm_init(XsPlugin *p, GKeyFile *kf)
{
    PrivData *priv = g_new0(PrivData, 1);
    GPtrArray *disks = dm_discover_disks();
    int x, y; gdouble opacity;
    char *label;
    p->priv = priv; priv->plugin = p; priv->kf = kf;
    /* g_new0 would leave secondary_milli at 0, which is a valid temperature. */
    priv->device.secondary_milli = G_MININT;
    priv->generation = (guint64)g_get_monotonic_time() ^ (guint64)(guintptr)p;
    priv->width = CLAMP(xs_host_api()->conf_int(kf,p->name,"window_width",DM_DEFAULT_WIDTH),160,1600);
    priv->height = CLAMP(xs_host_api()->conf_int(kf,p->name,"window_height",DM_DEFAULT_HEIGHT),120,1200);
    priv->corner_radius = CLAMP(xs_host_api()->conf_int(kf,p->name,"corner_radius",0),0,200);
    priv->design_width = priv->width;
    priv->design_height = priv->height;
    priv->update_ms = CLAMP(xs_host_api()->conf_int(kf,p->name,"update_ms",1000),100,60000);
    priv->read_x=xs_host_api()->conf_int(kf,p->name,"read_x",8); priv->read_y=xs_host_api()->conf_int(kf,p->name,"read_y",12);
    priv->write_x=xs_host_api()->conf_int(kf,p->name,"write_x",150); priv->write_y=xs_host_api()->conf_int(kf,p->name,"write_y",12);
    priv->temp_x=xs_host_api()->conf_int(kf,p->name,"temp_x",310); priv->temp_y=xs_host_api()->conf_int(kf,p->name,"temp_y",12);
    /* The label sits inside the graph now, so its default Y moved up too. */
    priv->label_x=xs_host_api()->conf_int(kf,p->name,"label_x",4); priv->label_y=xs_host_api()->conf_int(kf,p->name,"label_y",3);
    {
        char *show_temp = xs_host_api()->conf_str(kf, p->name,
                                                 "show_temperature_history", NULL);
        priv->show_temperature_history = dm_temperature_history_enabled(show_temp);
        g_free(show_temp);
    }
    priv->device.by_id = xs_host_api()->conf_str(kf,p->name,"by_id",NULL);
    if (!priv->device.by_id || !*priv->device.by_id) {
        g_free(priv->device.by_id);
        priv->device.by_id = disks->len ? g_strdup(g_ptr_array_index(disks,0)) : NULL;
    }
    /* Blocker 5: reject a configured path that is not an immediate child
     * symlink of /dev/disk/by-id (traversal, plain file, other directory). */
    if (priv->device.by_id && *priv->device.by_id &&
        !dm_by_id_path_is_valid_configured(priv->device.by_id)) {
        p->host->log("disk_monitor %s: by_id '%s' is not a /dev/disk/by-id symlink",
                     p->name, priv->device.by_id);
        g_ptr_array_free(disks,TRUE); dm_device_state_clear(&priv->device);
        g_free(priv); p->priv=NULL; return -1;
    }
    if (!priv->device.by_id || !*priv->device.by_id) {
        p->host->log("disk_monitor %s: no whole-disk by-id path available",p->name);
        g_ptr_array_free(disks,TRUE); dm_device_state_clear(&priv->device);
        g_free(priv); p->priv=NULL; return -1;
    }
    /* Resolve now, but an unresolved device is NOT a fatal error. */
    dm_try_resolve(priv, TRUE);
    label = dm_identity_label(priv->device.device_name, priv->device.by_id);
    priv->graph_label=xs_host_api()->conf_str(kf,p->name,"graph_label",label);
    g_free(label);
    priv->font=xs_host_api()->conf_str(kf,p->name,"font",DM_DEFAULT_FONT);
    /* An old config keeps the shared "font" for the graph label. */
    priv->label_font=xs_host_api()->conf_str(kf,p->name,"label_font",priv->font);
    priv->read_font=xs_host_api()->conf_str(kf,p->name,"read_font",priv->font);
    priv->write_font=xs_host_api()->conf_str(kf,p->name,"write_font",priv->font);
    priv->temp_font=xs_host_api()->conf_str(kf,p->name,"temp_font",priv->font);
    dm_read_color(priv,"graph_background_color",dm_graph_bg_default,priv->graph_bg); dm_read_color(priv,"border_color",dm_border_default,priv->border);
    dm_read_color(priv,"text_color",dm_text_default,priv->text_color); dm_read_color(priv,"read_color",dm_read_default,priv->read_color);
    dm_read_color(priv,"write_color",dm_write_default,priv->write_color); dm_read_color(priv,"temp_color",dm_temp_default,priv->temp_color);
    dm_read_color(priv,"read_text_color",priv->read_color,priv->read_text_color);
    dm_read_color(priv,"write_text_color",priv->write_color,priv->write_text_color);
    dm_read_color(priv,"temp_text_color",priv->temp_color,priv->temp_text_color);
    /* by_id identity and all defaults are persisted for the new instance. */
    g_key_file_set_string(kf,p->name,"by_id",priv->device.by_id); g_key_file_set_string(kf,p->name,"graph_label",priv->graph_label); g_key_file_set_string(kf,p->name,"font",priv->font);
    if (!g_key_file_has_key(kf, p->name, "label_font", NULL)) g_key_file_set_string(kf,p->name,"label_font",priv->label_font);
    if (!g_key_file_has_key(kf, p->name, "read_font", NULL)) g_key_file_set_string(kf,p->name,"read_font",priv->read_font);
    if (!g_key_file_has_key(kf, p->name, "write_font", NULL)) g_key_file_set_string(kf,p->name,"write_font",priv->write_font);
    if (!g_key_file_has_key(kf, p->name, "temp_font", NULL)) g_key_file_set_string(kf,p->name,"temp_font",priv->temp_font);
    g_key_file_set_boolean(kf,p->name,"show_temperature_history",priv->show_temperature_history);
    g_key_file_set_integer(kf,p->name,"window_width",priv->width); g_key_file_set_integer(kf,p->name,"window_height",priv->height); g_key_file_set_integer(kf,p->name,"update_ms",priv->update_ms);
    g_key_file_set_integer(kf,p->name,"corner_radius",priv->corner_radius);
    xs_core_plugin_conf_flush(p->name); g_ptr_array_free(disks,TRUE);
    x=xs_host_api()->conf_int(kf,p->name,"x",80); y=xs_host_api()->conf_int(kf,p->name,"y",80); opacity=xs_host_api()->conf_dbl(kf,p->name,"opacity",1.0);
    p->win=xs_host_api()->make_window(p,x,y,priv->width,priv->height);
    if (!p->win) { p->host->log("disk_monitor: failed to create window"); dm_shutdown(p); return -1; }
    priv->cache_width=priv->width; priv->cache_height=priv->height; dm_sample(priv); priv->cache=dm_render(priv,priv->width,priv->height);
    xs_host_api()->set_opacity(p,CLAMP(opacity,0.1,1.0));
    xs_host_api()->set_tick(p,priv->update_ms); return 0;
}

static guint dm_tick(XsPlugin *p)
{
    PrivData *priv=p?p->priv:NULL;
    if (!priv || !p->win) return 0;
    /* Re-resolve a previously missing device at a bounded interval. */
    dm_try_resolve(priv, FALSE);
    dm_sample(priv);
    dm_rebuild(priv); xs_host_api()->invalidate(p); gtk_widget_queue_draw(p->win); return priv->update_ms;
}

/* Push the rounded outline to the X server as the window's VISIBLE shape.
 * The cairo clip in dm_render() only limits what the plugin paints; the X
 * window itself stays a full rectangle unless a shape is set, which is why the
 * corners were square on screen. gdk_window_shape_combine_region() (as opposed
 * to its input_shape_ twin, which core already uses) is what makes the corner
 * pixels genuinely not exist. Passing NULL clears the shape again, which is
 * what corner_radius=0 needs. */
static void dm_apply_shape(XsPlugin *p, int w, int h)
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
    region = dm_rounded_region(w, h, priv->corner_radius);
    gdk_window_shape_combine_region(window, region, 0, 0);
    if (region)
        cairo_region_destroy(region);
    priv->shape_radius = priv->corner_radius;
    priv->shape_w = w;
    priv->shape_h = h;
}

static void dm_draw(XsPlugin *p, cairo_t *cr, int w, int h)
{
    PrivData *priv=p?p->priv:NULL;
    if (!priv || !priv->cache) return;
    if (w!=priv->cache_width || h!=priv->cache_height) { priv->cache_width=w; priv->cache_height=h; dm_rebuild(priv); }
    dm_apply_shape(p, w, h);
    cairo_set_source_surface(cr,priv->cache,0,0); cairo_paint(cr);
}

static void dm_shutdown(XsPlugin *p)
{
    PrivData *priv=p?p->priv:NULL; if (!priv) return;
    priv->generation++;
    if (priv->hddtemp_task) {
        GTask *task = priv->hddtemp_task;
        priv->hddtemp_task = NULL;
        g_cancellable_cancel(priv->hddtemp_cancellable);
        g_object_unref(task);
        g_clear_object(&priv->hddtemp_cancellable);
    }
        /* Drop the shape we installed, otherwise a later plugin reusing this
     * window would inherit our rounded outline. */
    if (p->win) {
        GdkWindow *win = gtk_widget_get_window(p->win);
        if (win)
            gdk_window_shape_combine_region(win, NULL, 0, 0);
    }
if (priv->cache) cairo_surface_destroy(priv->cache);
    dm_device_state_clear(&priv->device);
    g_free(priv->graph_label); g_free(priv->font); g_free(priv->label_font);
    g_free(priv->read_font); g_free(priv->write_font); g_free(priv->temp_font);
    g_free(priv); p->priv=NULL;
}

static gint dm_combo_find_text(GtkComboBox *combo, const char *text)
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
            gint index;
            g_free(value);
            index = path ? gtk_tree_path_get_indices(path)[0] : -1;
            if (path)
                gtk_tree_path_free(path);
            return index;
        }
        g_free(value);
        valid = gtk_tree_model_iter_next(model, &iter);
    }
    return -1;
}

static void dm_position_changed(GtkSpinButton *spin, gpointer data)
{
    DmDialogContext *ctx=data; PrivData *priv=dm_live_priv(ctx); const char *key; int value;
    XsPlugin *p;
    if (!priv) return;
    p = priv->plugin;
    key = g_object_get_data(G_OBJECT(spin), "xs-key");
    value = (int)gtk_spin_button_get_value(spin);
    if (!strcmp(key,"read_x")) priv->read_x=value; else if (!strcmp(key,"read_y")) priv->read_y=value;
    else if (!strcmp(key,"write_x")) priv->write_x=value; else if (!strcmp(key,"write_y")) priv->write_y=value;
    else if (!strcmp(key,"temp_x")) priv->temp_x=value; else if (!strcmp(key,"temp_y")) priv->temp_y=value;
    else if (!strcmp(key,"label_x")) priv->label_x=value; else if (!strcmp(key,"label_y")) priv->label_y=value;
    else if (!strcmp(key,"window_width")) priv->width=value; else if (!strcmp(key,"window_height")) priv->height=value;
    else if (!strcmp(key,"corner_radius")) priv->corner_radius=value;
    else if (!strcmp(key,"update_ms")) { priv->update_ms=value; xs_host_api()->set_tick(p,value); }
    g_key_file_set_integer(priv->kf,p->name,key,value); xs_core_plugin_conf_flush(p->name);
    xs_host_api()->resize(p,priv->width,priv->height); priv->cache_width=priv->width; priv->cache_height=priv->height; dm_rebuild(priv); gtk_widget_queue_draw(p->win);
}

static void dm_label_changed(GtkEditable *entry, gpointer data)
{
    DmDialogContext *ctx=data; PrivData *priv=dm_live_priv(ctx); const char *text; XsPlugin *p;
    if (!priv) return;
    p = priv->plugin;
    text = gtk_entry_get_text(GTK_ENTRY(entry));
    g_free(priv->graph_label); priv->graph_label=g_strdup(text);
    g_key_file_set_string(priv->kf,p->name,"graph_label",text); xs_core_plugin_conf_flush(p->name); dm_rebuild(priv); gtk_widget_queue_draw(p->win);
}

static void dm_temperature_history_toggled(GtkToggleButton *button, gpointer data)
{
    DmDialogContext *ctx = data;
    PrivData *priv = dm_live_priv(ctx);
    if (!priv) return;
    priv->show_temperature_history = gtk_toggle_button_get_active(button);
    g_key_file_set_boolean(priv->kf, priv->plugin->name,
                           "show_temperature_history",
                           priv->show_temperature_history);
    xs_core_plugin_conf_flush(priv->plugin->name);
    dm_rebuild(priv);
    gtk_widget_queue_draw(priv->plugin->win);
}

/* Blocker 7: a disk switch PREPARES the replacement in a temporary state and
 * commits state + config atomically. If the new path cannot be resolved the
 * old device, its histories and the config are left untouched, and the combo
 * is restored to the still-current entry. */
static void dm_disk_changed(GtkComboBox *combo, gpointer data)
{
    DmDialogContext *ctx=data; PrivData *priv=dm_live_priv(ctx);
    DmDeviceState prepared;
    char *selected, *commit_value;
    GtkComboBoxText *text_combo;
    if (!priv) return;
    selected = gtk_combo_box_text_get_active_text(GTK_COMBO_BOX_TEXT(combo));
    if (!selected) return;
    memset(&prepared, 0, sizeof(prepared));
    if (!dm_device_state_prepare(&prepared, selected) ||
        !dm_device_state_is_whole_disk(&prepared)) {
        priv->plugin->host->log("disk_monitor %s: rejected disk switch to %s",
                                priv->plugin->name, selected);
        dm_device_state_clear(&prepared);
        g_free(selected);
        /* Rollback: put the combo back on the device that is still live, with
         * the handler blocked so restoring the selection is not a new switch. */
        text_combo = GTK_COMBO_BOX_TEXT(combo);
        if (priv->device.by_id) {
            gint index = dm_combo_find_text(GTK_COMBO_BOX(text_combo), priv->device.by_id);
            if (index >= 0) {
                g_signal_handlers_block_matched(combo, G_SIGNAL_MATCH_DATA,
                                                0, 0, NULL,
                                                dm_disk_changed, ctx);
                gtk_combo_box_set_active(GTK_COMBO_BOX(combo), index);
                g_signal_handlers_unblock_matched(combo, G_SIGNAL_MATCH_DATA,
                                                  0, 0, NULL,
                                                  dm_disk_changed, ctx);
            }
        }
        return;
    }
    commit_value = dm_switch_config_by_id(&prepared);
    if (!commit_value) {
        dm_device_state_clear(&prepared);
        g_free(selected);
        return;
    }
    /* Atomic commit: prepared state replaces the live state in one step, and
     * the config is written only for the state that actually took effect. */
    dm_device_state_clear(&priv->device);
    priv->device = prepared;
    memset(&prepared, 0, sizeof(prepared));
    priv->device.sample.io_valid = FALSE;
    priv->device.sample.temperature_valid = FALSE;
    priv->device.last_resolve_attempt_us = 0;
    g_key_file_set_string(priv->kf, priv->plugin->name, "by_id", commit_value);
    xs_core_plugin_conf_flush(priv->plugin->name);
    g_free(commit_value);
    g_free(selected);
    dm_sample(priv);
    dm_rebuild(priv);
    gtk_widget_queue_draw(priv->plugin->win);
}

static void dm_color_set(GtkColorButton *button, gpointer data)
{
    DmDialogContext *ctx=data; PrivData *priv=dm_live_priv(ctx); const char *key; gdouble *target; GdkRGBA color; char *value; XsPlugin *p;
    if (!priv) return;
    p = priv->plugin;
    key = g_object_get_data(G_OBJECT(button), "xs-key");
    if (!strcmp(key, "graph_background_color")) target = priv->graph_bg;
    else if (!strcmp(key, "border_color")) target = priv->border;
    else if (!strcmp(key, "text_color")) target = priv->text_color;
    else if (!strcmp(key, "read_color")) target = priv->read_color;
    else if (!strcmp(key, "write_color")) target = priv->write_color;
    else if (!strcmp(key, "temp_color")) target = priv->temp_color;
    else if (!strcmp(key, "read_text_color")) target = priv->read_text_color;
    else if (!strcmp(key, "write_text_color")) target = priv->write_text_color;
    else if (!strcmp(key, "temp_text_color")) target = priv->temp_text_color;
    else return;
    gtk_color_chooser_get_rgba(GTK_COLOR_CHOOSER(button), &color);
    target[0] = color.red; target[1] = color.green; target[2] = color.blue;
    /* The series pickers are RGB-only by user request, so GTK drops the alpha
     * channel on any pick and hands back 1.0 — writing that back would destroy
     * the configured transparency the first time the user touches a colour,
     * and that alpha is exactly what the fill is drawn with. The pickers are
     * built with the stored value, so the safe source is the current target.
     *
     * Background and border keep a real alpha scale (they come from the shared
     * core row builder, which enables it), so for those the picker's alpha is
     * the user's actual choice and must be stored. */
    if (!strcmp(key, "graph_background_color") || !strcmp(key, "border_color"))
        target[3] = color.alpha;
    value = dm_format_rgba(target);
    g_key_file_set_string(priv->kf, p->name, key, value);
    g_free(value);
    xs_core_plugin_conf_flush(p->name); dm_rebuild(priv); gtk_widget_queue_draw(p->win);
}

static void dm_bind(GtkWidget *widget, DmDialogContext *ctx, const char *signal,
                    GCallback handler)
{
    /* Each widget holds its OWN reference, released on destroy by
     * g_object_set_data_full — closing the dialog drops every callback's
     * context, and a restart mid-dialog leaves the old plugin untouched. */
    g_object_set_data_full(G_OBJECT(widget), DM_CTX_KEY,
                           dm_dialog_context_ref(ctx),
                           (GDestroyNotify)dm_dialog_context_unref);
    g_signal_connect(widget, signal, handler, ctx);
}

static void dm_add_color(GtkWidget *page, DmDialogContext *ctx, const char *key, const char *label, const gdouble color[4])
{
    GtkWidget *w = xs_prop_add_color(GTK_BOX(page), label, "Disk monitor RGBA color", color[0], color[1], color[2], color[3]);
    g_object_set_data_full(G_OBJECT(w), "xs-key", g_strdup(key), g_free);
    dm_bind(w, ctx, "color-set", G_CALLBACK(dm_color_set));
}

static void dm_add_int(GtkWidget *page, DmDialogContext *ctx, const char *key, const char *label, int value, int min, int max)
{
    GtkWidget *w=xs_prop_add_int(GTK_BOX(page),label,"Disk monitor position or size",value,min,max,1);
    g_object_set_data_full(G_OBJECT(w),"xs-key",g_strdup(key),g_free);
    dm_bind(w, ctx, "value-changed", G_CALLBACK(dm_position_changed));
}

static void dm_series_font_set(GtkFontButton *button, gpointer data)
{
    DmDialogContext *ctx = data;
    PrivData *priv = dm_live_priv(ctx);
    const char *key, *value = NULL;
    char **target;

    if (!priv)
        return;
    key = g_object_get_data(G_OBJECT(button), "xs-key");
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wdeprecated-declarations"
    value = gtk_font_button_get_font_name(button);
#pragma GCC diagnostic pop
    if (!key || !value || !*value)
        return;
    if (!strcmp(key, "label_font")) target = &priv->label_font;
    else if (!strcmp(key, "read_font")) target = &priv->read_font;
    else if (!strcmp(key, "write_font")) target = &priv->write_font;
    else if (!strcmp(key, "temp_font")) target = &priv->temp_font;
    else return;
    g_free(*target);
    *target = g_strdup(value);
    g_key_file_set_string(priv->kf, priv->plugin->name, key, value);
    xs_core_plugin_conf_flush(priv->plugin->name);
    dm_rebuild(priv);
    gtk_widget_queue_draw(priv->plugin->win);
}

static void dm_bind_keyed_descendants(GtkWidget *widget, DmDialogContext *ctx)
{
    if (GTK_IS_FONT_BUTTON(widget))
        dm_bind(widget, ctx, "font-set", G_CALLBACK(dm_series_font_set));
    else if (GTK_IS_COLOR_BUTTON(widget))
        dm_bind(widget, ctx, "color-set", G_CALLBACK(dm_color_set));
    else if (GTK_IS_SPIN_BUTTON(widget))
        dm_bind(widget, ctx, "value-changed", G_CALLBACK(dm_position_changed));
    if (GTK_IS_CONTAINER(widget)) {
        GList *children = gtk_container_get_children(GTK_CONTAINER(widget));
        GList *item;
        for (item = children; item; item = item->next)
            dm_bind_keyed_descendants(item->data, ctx);
        g_list_free(children);
    }
}

static void dm_add_separator(GtkWidget *page)
{
    GtkWidget *separator = gtk_separator_new(GTK_ORIENTATION_HORIZONTAL);
    gtk_widget_set_margin_top(separator, 6);
    gtk_widget_set_margin_bottom(separator, 6);
    gtk_box_pack_start(GTK_BOX(page), separator, FALSE, TRUE, 0);
}

static void dm_add_series_block(GtkWidget *page, DmDialogContext *ctx,
                                const char *title, const char *font_key,
                                const char *text_color_key, const char *x_key,
                                const char *y_key, const char *history_key,
                                const char *history_label, const char *font,
                                const gdouble text_color[4],
                                const gdouble history_color[4],
                                int x, int y)
{
    DmSeriesBlockSpec spec;
    GtkWidget *block, *content;

    if (!dm_series_block_spec(title, font_key, text_color_key, x_key, y_key,
                              history_key, font, history_label,
                              text_color, history_color, x, y, &spec))
        return;
    block = dm_series_block_widget(&spec, &content);
    if (!block)
        return;
    gtk_box_pack_start(GTK_BOX(page), block, FALSE, TRUE, 0);
    dm_bind_keyed_descendants(content, ctx);
}

/* GTK3 has no gtk_combo_box_text_find_text(); dm_combo_find_text() above does
 * the model walk. */
static void dm_properties(XsPlugin *p, GtkNotebook *notebook)
{
    PrivData *priv=p?p->priv:NULL;
    DmDialogContext *ctx;
    GtkWidget *page,*combo,*entry,*selector; GPtrArray *disks; guint i;
    if (!priv)
        return;
    ctx = dm_dialog_context_new(p->name);
    page = gtk_box_new(GTK_ORIENTATION_VERTICAL,4);
    gtk_container_set_border_width(GTK_CONTAINER(page),10);
    /* The page owns the last reference: destroying the dialog frees it. */
    g_object_set_data_full(G_OBJECT(page), DM_CTX_KEY, ctx,
                           (GDestroyNotify)dm_dialog_context_unref);
    disks=dm_discover_disks(); combo=gtk_combo_box_text_new();
    for(i=0;i<disks->len;i++) gtk_combo_box_text_append_text(GTK_COMBO_BOX_TEXT(combo),g_ptr_array_index(disks,i));
    gtk_combo_box_set_active(GTK_COMBO_BOX(combo),0);
    for(i=0;i<disks->len;i++) if(!strcmp(g_ptr_array_index(disks,i),priv->device.by_id)) gtk_combo_box_set_active(GTK_COMBO_BOX(combo),i);
    if (priv->device.by_id) {
        gint current = dm_combo_find_text(GTK_COMBO_BOX(combo), priv->device.by_id);
        gboolean identity_resolved = dm_by_id_is_whole_disk(priv->device.by_id);
        gboolean target_already_listed = FALSE;
        char *identity_real = identity_resolved ? realpath(priv->device.by_id, NULL) : NULL;
        for (i = 0; identity_real && i < disks->len; i++) {
            char *listed_real = realpath(g_ptr_array_index(disks, i), NULL);
            if (listed_real && g_strcmp0(identity_real, listed_real) == 0)
                target_already_listed = TRUE;
            free(listed_real);
        }
        if (current < 0 && dm_combo_identity_needs_row(priv->device.by_id, disks,
                                                        identity_resolved,
                                                        target_already_listed))
            gtk_combo_box_text_append_text(GTK_COMBO_BOX_TEXT(combo), priv->device.by_id);
        free(identity_real);
        if (current < 0)
            current = dm_combo_find_text(GTK_COMBO_BOX(combo), priv->device.by_id);
        if (current >= 0)
            gtk_combo_box_set_active(GTK_COMBO_BOX(combo), current);
    }
    if (combo) {
        selector = dm_disk_selector_widget(combo);
        gtk_box_pack_start(GTK_BOX(page), selector, FALSE, FALSE, 0);
    }
    dm_bind(combo, ctx, "changed", G_CALLBACK(dm_disk_changed));
    entry=xs_prop_add_bool(GTK_BOX(page),"Show temperature graph","Show the temperature line in the disk graph",priv->show_temperature_history);
    dm_bind(entry, ctx, "toggled", G_CALLBACK(dm_temperature_history_toggled));
    entry=xs_prop_add_string(GTK_BOX(page),"Graph label","Text shown over the graph",priv->graph_label);
    dm_bind(entry, ctx, "changed", G_CALLBACK(dm_label_changed));
    dm_add_int(page,ctx,"window_width","Window width",priv->width,160,1600); dm_add_int(page,ctx,"window_height","Window height",priv->height,120,1200); dm_add_int(page,ctx,"corner_radius","Corner radius",priv->corner_radius,0,200); dm_add_int(page,ctx,"update_ms","Update (ms)",priv->update_ms,100,60000);
    dm_add_separator(page);
    /* No history row: the label draws no fill, so the block must not offer a
     * second colour button for one. */
    dm_add_series_block(page,ctx,"Graph label","label_font","text_color",
                        "label_x","label_y",NULL,NULL,
                        priv->label_font,priv->text_color,priv->text_color,
                        priv->label_x,priv->label_y);
    dm_add_separator(page);
    dm_add_series_block(page,ctx,"Read text","read_font","read_text_color",
                        "read_x","read_y","read_color","Read history",
                        priv->read_font,priv->read_text_color,priv->read_color,
                        priv->read_x,priv->read_y);
    dm_add_separator(page);
    dm_add_series_block(page,ctx,"Write text","write_font","write_text_color",
                        "write_x","write_y","write_color","Write history",
                        priv->write_font,priv->write_text_color,priv->write_color,
                        priv->write_x,priv->write_y);
    dm_add_separator(page);
    dm_add_series_block(page,ctx,"Temperature text","temp_font","temp_text_color",
                        "temp_x","temp_y","temp_color","Temperature history",
                        priv->temp_font,priv->temp_text_color,priv->temp_color,
                        priv->temp_x,priv->temp_y);
    /* No "Background" row: since DM_GRAPH_TOP became 0 the graph covers the whole
     * window, so there is no separate base layer to configure any more. */
    dm_add_color(page,ctx,"graph_background_color","Graph background",priv->graph_bg);
    dm_add_color(page,ctx,"text_color","Text",priv->text_color);
    dm_add_color(page,ctx,"border_color","Graph border",priv->border);
    {
        GtkWidget *tab = page;
        int width, height;
        dm_properties_size(p->type, &width, &height);
        if (width > 0 && height > 0) {
            GtkWidget *scroller = dm_properties_scroller(page, width, height);
            if (scroller)
                tab = scroller;
        }
        g_ptr_array_free(disks,TRUE);
        gtk_notebook_append_page(notebook, tab, gtk_label_new("Disk Monitor"));
        gtk_widget_show_all(tab);
    }
}

static const XsPluginOps dm_ops={.init=dm_init,.draw=dm_draw,.tick=dm_tick,.shutdown=dm_shutdown,.properties=dm_properties};
static XsPluginDesc dm_desc={"disk_monitor",XS_API_VERSION,&dm_ops,"Per-disk I/O and temperature history monitor","xscreenlets","1.0"};
XS_PLUGIN_EXPORT(&dm_desc)
