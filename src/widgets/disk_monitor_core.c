#include "disk_monitor_core.h"

#include <gtk/gtk.h>
#include <ctype.h>
#include <errno.h>
#include <stdlib.h>
#include <math.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

#define DM_BY_ID_DIR "/dev/disk/by-id"
#define DM_RETRY_INTERVAL_US (60 * G_TIME_SPAN_SECOND)

/* ---- Blocker 6: refcounted dialog context (name only, never a plugin) ---- */

gboolean dm_position_pair_spec(const char *label,
                               const char *x_key,
                               const char *y_key,
                               int x, int y,
                               int x_max, int y_max,
                               DmPositionPairSpec *out)
{
    if (!label || !*label || !x_key || !*x_key || !y_key || !*y_key || !out)
        return FALSE;
    out->label = label;
    out->x_key = x_key;
    out->y_key = y_key;
    out->x = x;
    out->y = y;
    out->x_max = x_max;
    out->y_max = y_max;
    return TRUE;
}

GtkWidget *dm_position_pair_widget(const char *label,
                                   const char *x_key,
                                   const char *y_key,
                                   int x, int y,
                                   int x_max, int y_max,
                                   GtkWidget **x_spin,
                                   GtkWidget **y_spin)
{
    GtkWidget *row, *widget;
    DmPositionPairSpec spec;

    if (!x_spin || !y_spin ||
        !dm_position_pair_spec(label, x_key, y_key, x, y,
                               x_max, y_max, &spec))
        return NULL;
    row = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 6);
    gtk_box_pack_start(GTK_BOX(row), gtk_label_new("X"), FALSE, FALSE, 0);
    widget = gtk_spin_button_new_with_range(0, x_max, 1);
    gtk_spin_button_set_value(GTK_SPIN_BUTTON(widget), x);
    gtk_widget_set_size_request(widget, 76, -1);
    g_object_set_data_full(G_OBJECT(widget), "xs-key", g_strdup(x_key), g_free);
    gtk_box_pack_start(GTK_BOX(row), widget, FALSE, FALSE, 0);
    *x_spin = widget;
    gtk_box_pack_start(GTK_BOX(row), gtk_label_new("Y"), FALSE, FALSE, 0);
    widget = gtk_spin_button_new_with_range(0, y_max, 1);
    gtk_spin_button_set_value(GTK_SPIN_BUTTON(widget), y);
    gtk_widget_set_size_request(widget, 76, -1);
    g_object_set_data_full(G_OBJECT(widget), "xs-key", g_strdup(y_key), g_free);
    gtk_box_pack_start(GTK_BOX(row), widget, FALSE, FALSE, 0);
    *y_spin = widget;
    return row;
}

gboolean dm_series_block_spec(const char *title,
                              const char *font_key,
                              const char *text_color_key,
                              const char *x_key,
                              const char *y_key,
                              const char *history_color_key,
                              const char *font,
                              const char *history_label,
                              const gdouble text_color[4],
                              const gdouble history_color[4],
                              int x, int y,
                              int x_max, int y_max,
                              DmSeriesBlockSpec *out)
{
    /* The history row is OPTIONAL: the Graph label block has no fill of its
     * own, so an empty history key/label must not drop the whole block. */
    if (!title || !*title || !font_key || !*font_key ||
        !text_color_key || !*text_color_key || !x_key || !*x_key ||
        !y_key || !*y_key ||
        !font || !*font || !text_color || !out)
        return FALSE;
    out->title = title;
    out->font_key = font_key;
    out->text_color_key = text_color_key;
    out->x_key = x_key;
    out->y_key = y_key;
    out->font = font;
    /* Normalise the optional pair: half a history row is a caller bug. */
    if (history_color_key && *history_color_key && history_label &&
        *history_label && history_color) {
        out->history_color_key = history_color_key;
        out->history_label = history_label;
        memcpy(out->history_color, history_color, 4 * sizeof(gdouble));
    } else {
        out->history_color_key = NULL;
        out->history_label = NULL;
        memset(out->history_color, 0, sizeof(out->history_color));
    }
    memcpy(out->text_color, text_color, 4 * sizeof(gdouble));
    out->x = x;
    out->y = y;
    /* An anchor past the design base can only scale off-window, so cap the
     * spinner at the design size and keep at least the whole range usable. */
    out->x_max = x_max > 0 ? x_max : x;
    out->y_max = y_max > 0 ? y_max : y;
    return TRUE;
}

static GtkWidget *dm_series_compact_row(GtkWidget *content,
                                        const char *label,
                                        GtkWidget *control)
{
    GtkWidget *row = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 6);
    GtkWidget *text = gtk_label_new(label);

    gtk_widget_set_halign(text, GTK_ALIGN_START);
    gtk_box_pack_start(GTK_BOX(row), text, FALSE, TRUE, 0);
    gtk_box_pack_start(GTK_BOX(row), control, FALSE, TRUE, 0);
    gtk_box_pack_start(GTK_BOX(content), row, FALSE, TRUE, 0);
    return row;
}

GtkWidget *dm_series_block_widget(const DmSeriesBlockSpec *spec,
                                  GtkWidget **content)
{
    GtkWidget *frame, *box, *appearance, *font_holder, *font, *text_color;
    GtkWidget *history_color;
    GtkWidget *position, *x_spin, *y_spin;
    GdkRGBA rgba;

    if (!spec || !content)
        return NULL;
    frame = gtk_frame_new(spec->title);
    box = gtk_box_new(GTK_ORIENTATION_VERTICAL, 5);
    gtk_container_set_border_width(GTK_CONTAINER(box), 8);
    gtk_container_add(GTK_CONTAINER(frame), box);
    *content = box;

    font = gtk_font_button_new_with_font(spec->font);
    gtk_widget_set_size_request(font, 180, -1);
    font_holder = gtk_fixed_new();
    gtk_widget_set_size_request(font_holder, 180, -1);
    gtk_widget_set_hexpand(font_holder, FALSE);
    gtk_fixed_put(GTK_FIXED(font_holder), font, 0, 0);
    g_object_set_data_full(G_OBJECT(font), "xs-key",
                           g_strdup(spec->font_key), g_free);
    text_color = gtk_color_button_new_with_rgba(
        &(GdkRGBA){spec->text_color[0], spec->text_color[1],
                   spec->text_color[2], spec->text_color[3]});
    /* The user asked for an RGB-only picker: no transparency scale. The
     * configured alpha is still passed in and still drives the drawing. */
    gtk_color_chooser_set_use_alpha(GTK_COLOR_CHOOSER(text_color), FALSE);
    g_object_set_data_full(G_OBJECT(text_color), "xs-key",
                           g_strdup(spec->text_color_key), g_free);
    appearance = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 6);
    gtk_box_pack_start(GTK_BOX(appearance), font_holder, FALSE, TRUE, 0);
    gtk_box_pack_start(GTK_BOX(appearance), text_color, FALSE, TRUE, 0);
    gtk_box_pack_start(GTK_BOX(box), appearance, FALSE, TRUE, 0);

    position = dm_position_pair_widget("Position", spec->x_key, spec->y_key,
                                       spec->x, spec->y,
                                       spec->x_max, spec->y_max,
                                       &x_spin, &y_spin);
    dm_series_compact_row(box, "Position", position);

    /* The history row is optional: the Graph label block draws no fill of its
     * own, so it passes no history colour key and must not grow a button
     * for one. An empty key would also be written into the config as junk. */
    if (!spec->history_color_key || !spec->history_label)
        return frame;
    rgba.red = spec->history_color[0];
    rgba.green = spec->history_color[1];
    rgba.blue = spec->history_color[2];
    rgba.alpha = spec->history_color[3];
    /* RGB-only, like the text color button above: the stored alpha still
     * drives the fill, it is just not editable in the picker. */
    history_color = gtk_color_button_new_with_rgba(&rgba);
    gtk_color_chooser_set_use_alpha(GTK_COLOR_CHOOSER(history_color), FALSE);
    g_object_set_data_full(G_OBJECT(history_color), "xs-key",
                           g_strdup(spec->history_color_key), g_free);
    dm_series_compact_row(box, spec->history_label, history_color);
    return frame;
}

GtkWidget *dm_disk_selector_widget(GtkWidget *combo)
{
    GtkWidget *selector, *label, *combo_row;

    g_return_val_if_fail(GTK_IS_COMBO_BOX(combo), NULL);
    selector = gtk_box_new(GTK_ORIENTATION_VERTICAL, 4);
    label = gtk_label_new("Disk");
    gtk_widget_set_halign(label, GTK_ALIGN_START);
    /* The combo keeps its own natural width — GTK already sizes it to the
     * longest entry in the list — and never expands or stretches. */
    gtk_widget_set_hexpand(combo, FALSE);
    gtk_widget_set_valign(combo, GTK_ALIGN_START);
    combo_row = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 0);
    gtk_box_pack_start(GTK_BOX(combo_row), combo, FALSE, FALSE, 0);
    gtk_box_pack_start(GTK_BOX(selector), label, FALSE, FALSE, 0);
    gtk_box_pack_start(GTK_BOX(selector), combo_row, FALSE, FALSE, 0);
    return selector;
}

char *dm_format_temperature(const gint *milli, guint count)
{
    GString *out;
    guint i;
    gboolean any = FALSE;

    if (!milli || !count)
        return g_strdup("N/A");
    out = g_string_new(NULL);
    for (i = 0; i < count; i++) {
        char buf[G_ASCII_DTOSTR_BUF_SIZE];
        if (milli[i] == G_MININT)
            continue;
        if (any)
            g_string_append(out, " ");
        /* ASCII decimals: a comma decimal separator must never reach the UI. */
        g_ascii_formatd(buf, sizeof(buf), "%.1f", milli[i] / 1000.0);
        g_string_append(out, buf);
        g_string_append(out, " C");
        any = TRUE;
    }
    if (!any) {
        g_string_free(out, TRUE);
        return g_strdup("N/A");
    }
    return g_string_free(out, FALSE);
}

char **dm_format_temperature_lines(const gint *milli, guint count,
                                   guint *out_lines)
{
    GPtrArray *lines = g_ptr_array_new();
    guint i;

    for (i = 0; i < count; i++) {
        char buf[G_ASCII_DTOSTR_BUF_SIZE];
        if (!milli || milli[i] == G_MININT)
            continue;
        /* ASCII decimals: a comma decimal separator must never reach the UI. */
        g_ascii_formatd(buf, sizeof(buf), "%.1f", milli[i] / 1000.0);
        g_ptr_array_add(lines, g_strconcat(buf, " C", NULL));
    }
    if (lines->len == 0)
        g_ptr_array_add(lines, g_strdup("N/A"));
    if (out_lines)
        *out_lines = lines->len;
    /* NULL-terminate: the caller frees the result with g_strfreev(). */
    g_ptr_array_add(lines, NULL);
    return (char **)g_ptr_array_free(lines, FALSE);
}

GtkWidget *dm_properties_scroller(GtkWidget *page, int width, int height)
{
    GtkWidget *scroller, *viewport;

    if (!GTK_IS_WIDGET(page))
        return NULL;
    scroller = gtk_scrolled_window_new(NULL, NULL);
    gtk_scrolled_window_set_policy(GTK_SCROLLED_WINDOW(scroller),
                                   GTK_POLICY_NEVER, GTK_POLICY_AUTOMATIC);
    /* Without this the scroller still reports the page's full natural size and
     * the Properties window grows to fit every row instead of scrolling. */
    gtk_scrolled_window_set_propagate_natural_width(GTK_SCROLLED_WINDOW(scroller),
                                                    FALSE);
    gtk_scrolled_window_set_propagate_natural_height(GTK_SCROLLED_WINDOW(scroller),
                                                     FALSE);
    /* One call: a second set_size_request would reset the first dimension. */
    if (width > 0 && height > 0)
        gtk_widget_set_size_request(scroller, width, height);
    else if (width > 0)
        gtk_widget_set_size_request(scroller, width, -1);
    else if (height > 0)
        gtk_widget_set_size_request(scroller, -1, height);
    viewport = gtk_viewport_new(NULL, NULL);
    gtk_container_add(GTK_CONTAINER(viewport), page);
    gtk_container_add(GTK_CONTAINER(scroller), viewport);
    return scroller;
}

void dm_properties_size(const char *plugin_type, int *width, int *height)
{
    if (plugin_type && strcmp(plugin_type, "disk_monitor") == 0) {
        if (width)
            *width = 620;
        if (height)
            *height = 780;
    } else {
        if (width)
            *width = 0;
        if (height)
            *height = 0;
    }
}

DmDialogContext *dm_dialog_context_new(const char *instance_name)
{
    DmDialogContext *ctx = g_new0(DmDialogContext, 1);

    ctx->refcount = 1;
    ctx->instance_name = g_strdup(instance_name ? instance_name : "");
    return ctx;
}

DmDialogContext *dm_dialog_context_ref(DmDialogContext *ctx)
{
    if (ctx)
        g_atomic_int_inc(&ctx->refcount);
    return ctx;
}

void dm_dialog_context_unref(DmDialogContext *ctx)
{
    if (!ctx)
        return;
    if (!g_atomic_int_dec_and_test(&ctx->refcount))
        return;
    g_free(ctx->instance_name);
    g_free(ctx);
}

const char *dm_dialog_context_name(const DmDialogContext *ctx)
{
    return ctx ? ctx->instance_name : NULL;
}

gint dm_dialog_context_refcount(const DmDialogContext *ctx)
{
    return ctx ? ctx->refcount : 0;
}

gboolean dm_temperature_request_matches(const DmTemperatureRequest *request,
                                       const char *instance_name,
                                       guint64 generation,
                                       const char *by_id,
                                       const char *device_path,
                                       gboolean resolved)
{
    return request && resolved && instance_name && by_id && device_path &&
           request->instance_name && request->by_id && request->device_path &&
           request->generation == generation &&
           g_str_equal(request->instance_name, instance_name) &&
           g_str_equal(request->by_id, by_id) &&
           g_str_equal(request->device_path, device_path);
}

gboolean dm_temperature_completion_apply(DiskSample *sample,
                                        const DmTemperatureRequest *request,
                                        gint temperature_milli,
                                        const char *instance_name,
                                        guint64 generation,
                                        const char *by_id,
                                        const char *device_path,
                                        gboolean resolved)
{
    if (!sample || !dm_temperature_request_matches(request, instance_name,
                                                     generation, by_id,
                                                     device_path, resolved))
        return FALSE;
    if (temperature_milli == G_MININT) {
        sample->temperature_valid = FALSE;
        sample->temperature_milli = G_MININT;
    } else {
        sample->temperature_valid = TRUE;
        sample->temperature_milli = temperature_milli;
    }
    return TRUE;
}

void dm_temperature_request_clear(DmTemperatureRequest *request)
{
    if (!request)
        return;
    g_free(request->instance_name);
    g_free(request->by_id);
    g_free(request->device_path);
    memset(request, 0, sizeof(*request));
}

/* ---- Blocker 4: bounded re-resolution, identity preserved ----------------- */

gint64 dm_retry_interval_us(void)
{
    return DM_RETRY_INTERVAL_US;
}

gboolean dm_retry_due(gint64 last_attempt_us, gint64 now_us)
{
    if (last_attempt_us == 0)
        return TRUE;
    if (now_us < last_attempt_us)
        return TRUE;  /* clock moved backwards: retry instead of stalling */
    return now_us - last_attempt_us >= DM_RETRY_INTERVAL_US;
}

char *dm_identity_label(const char *device_name, const char *by_id)
{
    if (device_name && *device_name)
        return g_strdup(device_name);
    if (by_id && *by_id) {
        const char *base = strrchr(by_id, '/');
        return g_strdup(base ? base + 1 : by_id);
    }
    return g_strdup("no disk");
}

/* ---- Blocker 5: the configured path must be an immediate child symlink ---- */

gboolean dm_by_id_path_is_direct_symlink(const char *path, const char *dir)
{
    struct stat st;
    size_t dir_length;
    const char *base;

    if (!path || !*path || !dir || !*dir)
        return FALSE;
    dir_length = strlen(dir);
    /* No traversal component and no nested separator after the directory. */
    if (strstr(path, ".."))
        return FALSE;
    if (strncmp(path, dir, dir_length) != 0 || path[dir_length] != '/')
        return FALSE;
    base = path + dir_length + 1;
    if (!*base || strchr(base, '/'))
        return FALSE;
    if (lstat(path, &st) != 0)
        return FALSE;
    return S_ISLNK(st.st_mode);
}

gboolean dm_by_id_path_is_valid_configured(const char *path)
{
    struct stat st;
    const char *base;

    if (!path || !g_str_has_prefix(path, DM_BY_ID_DIR "/"))
        return FALSE;
    base = path + strlen(DM_BY_ID_DIR) + 1;
    if (!*base || strchr(base, '/') || strstr(base, ".."))
        return FALSE;
    if (lstat(path, &st) == 0)
        return S_ISLNK(st.st_mode);
    /* A udev by-id link may temporarily disappear with an unplugged disk. Its
     * immediate-child identity is still safe to preserve for bounded retry. */
    return errno == ENOENT;
}

DmResolutionResult dm_resolved_target_matches(const char *path,
                                              const char *expected_real)
{
    char *real;
    DmResolutionResult result;

    if (!path || !expected_real)
        return DM_RESOLUTION_MISSING;
    real = realpath(path, NULL);
    if (!real)
        return DM_RESOLUTION_MISSING;
    result = g_str_equal(real, expected_real)
        ? DM_RESOLUTION_MATCH : DM_RESOLUTION_MOVED;
    free(real);
    return result;
}

/* ---- Blocker 3: an invalid sample clears the current display values ------- */

void dm_sample_mark_invalid(DiskSample *sample)
{
    if (!sample)
        return;
    sample->io_valid = FALSE;
    sample->read_bytes = 0;
    sample->write_bytes = 0;
    sample->temperature_valid = FALSE;
    sample->temperature_milli = G_MININT;
}

gboolean dm_sample_current_rates(const DiskSample *sample, guint64 *read, guint64 *write)
{
    if (!sample)
        return FALSE;
    if (read)
        *read = sample->io_valid ? sample->read_bytes : 0;
    if (write)
        *write = sample->io_valid ? sample->write_bytes : 0;
    return sample->io_valid;
}

/* ---- Blocker 1: order-independent (driver rank, label rank) selection ---- */

long dm_hwmon_candidate_key(int driver_rank, int label_rank)
{
    return (long)driver_rank * 100L + (long)label_rank;
}

gboolean dm_hwmon_candidate_preferred(int driver_rank, int label_rank,
                                      int best_driver_rank, int best_label_rank)
{
    return dm_hwmon_candidate_key(driver_rank, label_rank) <
           dm_hwmon_candidate_key(best_driver_rank, best_label_rank);
}

/* ---- Blocker 2: lockstep temperature history with missing markers ------- */

gboolean dm_temp_history_is_valid(const gint *ring, guint head, guint count,
                                  guint age_from_newest)
{
    if (!ring || age_from_newest >= count)
        return FALSE;
    return dm_temp_history_value(ring, head, count, age_from_newest) != G_MININT;
}

guint dm_temp_history_valid_run(const gint *ring, guint head, guint count)
{
    guint age;

    for (age = 0; age < count; age++)
        if (!dm_temp_history_is_valid(ring, head, count, age))
            break;
    return age;
}

gboolean dm_temp_history_observes(const gint *ring, guint head, guint count,
                                  gint value)
{
    guint age;

    for (age = 0; age < count; age++) {
        gint v = dm_temp_history_value(ring, head, count, age);
        if (v != G_MININT && v == value)
            return TRUE;
    }
    return FALSE;
}

/* ---- Blocker 7: temporary device state, atomic commit -------------------- */

/* Text anchors are stored in DESIGN coordinates, tied to the window size they
 * were laid out for. The renderer maps them to the live window every frame,
 * so repeated resizes can never accumulate a rounding drift — mutating the
 * stored value on each resize would (100 * 419/420 rounds back to 100, and
 * forty 1 px steps would drift ten pixels). */
int dm_scale_position(int value, int design_size, int live_size, int max)
{
    gint64 scaled;

    if (design_size <= 0 || live_size <= 0)
        return CLAMP(value, 0, MAX(0, max));
    scaled = ((gint64)value * live_size + (gint64)design_size / 2) /
             (gint64)design_size;
    if (max > 0 && scaled > max)
        scaled = max;
    if (scaled < 0)
        scaled = 0;
    return (int)scaled;
}

/* Keep a drawn string inside the frame: the scaled anchor alone is not enough,
 * because a long value at a legal x still runs off the right edge. */
int dm_fit_text_coordinate(int anchor, int text_extent, int limit)
{
    int x = anchor;

    if (x < 0)
        x = 0;
    if (limit > 0 && x + text_extent > limit)
        x = limit - text_extent;
    if (x < 0)
        x = 0;
    return x;
}

/* Corner radius: negative input and 0 both mean "square corners". cairo cannot
 * draw an arc larger than half the side, so a radius past that must be clamped
 * or it silently produces a malformed path. */
double dm_corner_radius_value(int value)
{
    return value > 0 ? (double)value : 0.0;
}

cairo_region_t *dm_rounded_region(int width, int height, int radius)
{
    const double r = dm_corner_radius_value(radius);
    cairo_region_t *region;
    cairo_rectangle_int_t box;
    double scaled;

    if (width <= 0 || height <= 0)
        return NULL;
    if (!dm_corner_radius_is_rounded(r))
        return NULL;

    /* cairo_region only holds integer rectangles, so a rounded outline has to
     * be approximated by one vertical slice per column. Each slice spans from
     * the top arc down to the bottom arc, which is exactly what the X server
     * needs for a shape mask. */
    scaled = MIN(r, MIN(width, height) / 2.0);
    region = cairo_region_create();
    if (!region)
        return NULL;

    for (int i = 0; i <= (int) ceil(scaled); i++) {
        /* How far this column is cut back from the top edge. The circle is
         * centred at (scaled, scaled) with radius `scaled`, so column i meets
         * it at y = scaled - sqrt(scaled^2 - (i-scaled)^2): the cut is DEEPEST
         * at the corner column (i=0) and vanishes at i=scaled. The earlier form
         * computed this the other way round, which made the mask bite hardest
         * near the corner and swallowed the border's own arc. */
        double d = fabs(i - scaled);
        int cut = 0;

        if (d <= scaled)
            /* floor, not ceil: the mask is what removes the corner, and ceil
             * cuts a full pixel deeper than the true arc all the way round.
             * That extra pixel is exactly where the border's anti-aliased
             * stroke lives, so ceil ate the visible part of the frame. floor
             * keeps the cut inside the arc, so the smooth cairo border shows
             * through and only a thin step remains at the very edge. */
            cut = (int) floor(scaled - sqrt(scaled * scaled - d * d));
        box.x = i;
        box.y = cut;
        box.width = 1;
        box.height = height - 2 * cut;
        if (box.height > 0)
            cairo_region_union_rectangle(region, &box);
        /* mirrored on the right edge */
        box.x = width - 1 - i;
        if (box.height > 0)
            cairo_region_union_rectangle(region, &box);
    }

    /* the middle band between the two corner arcs is a full-height slice */
    {
        int mid = (int) ceil(scaled);

        box.x = mid;
        box.y = 0;
        box.width = width - 2 * mid;
        box.height = height;
        if (box.width > 0)
            cairo_region_union_rectangle(region, &box);
    }

    return region;
}

gboolean dm_corner_radius_is_rounded(double radius)
{
    return radius > 0.5;
}

/* The second sensor has no place in DiskSample, so its own reset is explicit:
 * the G_MININT sentinel must always be set, because g_new0 leaves 0 there and
 * 0 is a perfectly valid temperature. */
void dm_device_state_secondary_reset(DmDeviceState *state)
{
    if (!state)
        return;
    g_clear_pointer(&state->temp_path_secondary, g_free);
    state->secondary_milli = G_MININT;
}

void dm_device_state_clear(DmDeviceState *state)
{
    if (!state)
        return;
    g_free(state->by_id);
    g_free(state->device_name);
    g_free(state->device_path);
    g_free(state->temp_path);
    g_free(state->temp_path_secondary);
    memset(state, 0, sizeof(*state));
    /* memset leaves 0 in secondary_milli, and 0 is a perfectly valid
     * temperature — a cleared state must read as "no reading", not 0.0 C. */
    state->secondary_milli = G_MININT;
}

void dm_device_state_replace_owned(DmDeviceState *state,
                                   const char *by_id,
                                   char *device_name,
                                   char *device_path)
{
    char *new_by_id;
    char *new_device_name;
    char *new_device_path;
    char *old_by_id;
    char *old_device_name;
    char *old_device_path;
    char *old_temp_path;
    char *old_temp_path_secondary;

    if (!state)
        return;
    /* Duplicate every incoming pointer BEFORE releasing any old owned field. */
    new_by_id = g_strdup(by_id);
    new_device_name = g_strdup(device_name);
    new_device_path = g_strdup(device_path);
    old_by_id = state->by_id;
    old_device_name = state->device_name;
    old_device_path = state->device_path;
    old_temp_path = state->temp_path;
    old_temp_path_secondary = state->temp_path_secondary;
    state->by_id = new_by_id;
    state->device_name = new_device_name;
    state->device_path = new_device_path;
    state->temp_path = NULL;
    state->temp_path_secondary = NULL;
    state->secondary_milli = G_MININT;
    state->resolved = new_by_id && new_device_name && new_device_path;
    if (device_name != old_device_name)
        g_free(device_name);
    if (device_path != old_device_path)
        g_free(device_path);
    g_free(old_by_id);
    g_free(old_device_name);
    g_free(old_device_path);
    g_free(old_temp_path);
    g_free(old_temp_path_secondary);
}

gboolean dm_device_state_prepare(DmDeviceState *out, const char *by_id)
{
    char *real;
    struct stat st;
    gboolean whole;

    if (!out || !by_id || !dm_by_id_path_is_valid_configured(by_id))
        return FALSE;
    /* Resolve the symlink only to obtain the current kernel name; the config
     * keeps the stable by-id identity. */
    real = realpath(by_id, NULL);
    if (!real)
        return FALSE;
    if (stat(real, &st) != 0 || !S_ISBLK(st.st_mode)) {
        free(real);
        return FALSE;
    }
    {
        char *base = g_path_get_basename(real);
        whole = dm_is_whole_disk_name(base);
        g_free(base);
    }
    if (!whole) {
        free(real);
        return FALSE;
    }
    dm_device_state_replace_owned(out, by_id,
                                  g_path_get_basename(real), g_strdup(real));
    free(real);
    return TRUE;
}

gboolean dm_device_state_is_whole_disk(const DmDeviceState *state)
{
    return state && state->resolved && state->by_id && *state->by_id &&
           state->device_name && *state->device_name &&
           dm_is_whole_disk_name(state->device_name);
}

char *dm_switch_config_by_id(const DmDeviceState *state)
{
    if (!state || !state->resolved || !state->by_id || !*state->by_id)
        return NULL;
    return g_strdup(state->by_id);
}

static gboolean dm_parse_u64(const char *text, guint64 *out)
{
    char *end = NULL;
    guint64 value;

    /* Only the leading integer matters: /proc/diskstats fields are followed by
     * more " key=" pairs, and the temperature file ends with a newline. */
    if (!text || !g_ascii_isdigit(*text))
        return FALSE;
    errno = 0;
    value = g_ascii_strtoull(text, &end, 10);
    if (errno || end == text)
        return FALSE;
    *out = value;
    return TRUE;
}

gboolean dm_parse_diskstats_named(const char *text, const char *device_name,
                                  guint64 *sectors_read, guint64 *sectors_written)
{
    char **lines;
    guint i;
    gboolean found = FALSE;

    if (!text || !device_name)
        return FALSE;
    lines = g_strsplit(text, "\n", -1);
    for (i = 0; lines[i] && !found; i++) {
        char **raw = g_strsplit_set(lines[i], " \t", -1);
        char *fields[16];
        int n = 0, k;

        /* /proc/diskstats pads columns with runs of spaces, so empty tokens
         * must be dropped before positional indexing. */
        for (k = 0; raw[k] && n < 15; k++)
            if (raw[k][0])
                fields[n++] = raw[k];
        fields[n] = NULL;
        /* major minor name | reads reads_merged sectors_read ms reading_ms
         * | writes writes_merged sectors_written writing_ms ... */
        if (n >= 14 && !strcmp(fields[2], device_name) &&
            dm_parse_u64(fields[5], sectors_read) &&
            dm_parse_u64(fields[10], sectors_written))
            found = TRUE;
        g_strfreev(raw);
    }
    g_strfreev(lines);
    return found;
}

guint64 dm_sectors_to_bytes(guint64 sectors)
{
    /* /proc/diskstats sector counters are ALWAYS in fixed 512-byte units,
     * independent of the device's logical_block_size and of its physical
     * sector size. See Documentation/admin-guide/iostats.rst. Do NOT multiply
     * by logical_block_size: on a 512e drive (physical 4096) that inflates the
     * result 8x, and on a 4Kn device the kernel has already normalised the
     * counters, so it inflates 8x the other way. dm_disk_logical_sector_size()
     * exists for logging this value only. */
    if (sectors > G_MAXUINT64 / 512)
        return G_MAXUINT64;
    return sectors * 512;
}

gint64 dm_rate_bytes_per_second(guint64 previous, guint64 current,
                                guint64 elapsed_us)
{
    guint64 delta;

    if (elapsed_us == 0 || current < previous)
        return 0;
    delta = current - previous;
    if (delta > (guint64)G_MAXINT64)
        return G_MAXINT64;
    {
        guint64 whole = delta / elapsed_us;
        guint64 remainder = delta % elapsed_us;
        guint64 fraction;

        if (whole > (guint64)G_MAXINT64 / 1000000)
            return G_MAXINT64;
        fraction = (guint64)((long double)remainder * 1000000.0L / elapsed_us);
        if (whole * 1000000 > (guint64)G_MAXINT64 - fraction)
            return G_MAXINT64;
        return (gint64)(whole * 1000000 + fraction);
    }
}

void dm_diskstats_sample_failed(DmHistoryState *history, gint64 now_us)
{
    (void)now_us;
    if (!history)
        return;
    history->previous_read = 0;
    history->previous_write = 0;
    history->previous_time_us = 0;
    history->previous_valid = FALSE;
}

gint dm_parse_temperature(const char *text)
{
    char *end = NULL;
    gint64 value;

    if (!text || (!g_ascii_isdigit(*text) && *text != '-'))
        return G_MININT;
    errno = 0;
    value = g_ascii_strtoll(text, &end, 10);
    if (errno || end == text || value < G_MININT || value > G_MAXINT)
        return G_MININT;
    return (gint)value;
}

gint dm_parse_hddtemp_response(const char *response, const char *selected_real_device)
{
    char **fields;
    gint result = G_MININT;

    if (!response || !selected_real_device)
        return result;
    fields = g_strsplit(response, "|", -1);
    if (g_strv_length(fields) >= 6) {
        int i;
        for (i = 0; fields[i] && i + 4 < (int)g_strv_length(fields); i += 5) {
            char *device = g_strdup(fields[i + 1]);
            char *temperature = g_strdup(fields[i + 3]);
            char *unit = g_strdup(fields[i + 4]);

            g_strstrip(device); g_strstrip(temperature); g_strstrip(unit);
            if (!strcmp(unit, "C") && !strcmp(device, selected_real_device))
                result = dm_parse_temperature(temperature);
            g_free(device); g_free(temperature); g_free(unit);
            if (result != G_MININT)
                break;
        }
    }
    g_strfreev(fields);
    return result;
}

gboolean dm_hddtemp_start_allowed(gpointer current_task, gboolean resolved,
                                  gboolean has_identity, gboolean due)
{
    return !current_task && resolved && has_identity && due;
}

gboolean dm_hddtemp_completion_is_current(gpointer current_task,
                                          gpointer completion_task)
{
    return current_task && completion_task && current_task == completion_task;
}

gint64 dm_deadline_remaining_us(gint64 deadline_us, gint64 now_us)
{
    if (now_us >= deadline_us)
        return 0;
    return deadline_us - now_us;
}

gint dm_hddtemp_query(const char *host, guint16 port,
                      const char *selected_real_device, guint timeout_ms)
{
    GSocketClient *client;
    GSocketConnection *connection;
    GSocket *socket;
    GError *error = NULL;
    char buffer[4096];
    gsize length = 0;
    gint result = G_MININT;
    gint64 deadline_us;
    gssize sent;

    if (!host || !selected_real_device || timeout_ms == 0)
        return G_MININT;
    deadline_us = g_get_monotonic_time() + (gint64)timeout_ms * 1000;
    client = g_socket_client_new();
    g_socket_client_set_timeout(client, timeout_ms);
    connection = g_socket_client_connect_to_host(client, host, port, NULL, &error);
    if (!connection) {
        g_clear_error(&error);
        g_object_unref(client);
        return G_MININT;
    }
    socket = g_socket_connection_get_socket(connection);
    g_socket_set_blocking(socket, FALSE);
    if (g_socket_condition_timed_wait(socket, G_IO_OUT,
                                        dm_deadline_remaining_us(deadline_us,
                                                                  g_get_monotonic_time()),
                                        NULL, &error))
        sent = g_socket_send_with_blocking(socket, "d", 1, FALSE, NULL, &error);
    else
        sent = -1;
    if (sent == 1 &&
        g_socket_condition_timed_wait(socket, G_IO_IN,
                                       dm_deadline_remaining_us(deadline_us,
                                                                 g_get_monotonic_time()),
                                       NULL, &error)) {
        while (length < sizeof(buffer) - 1) {
            gint64 remaining = dm_deadline_remaining_us(deadline_us,
                                                         g_get_monotonic_time());
            gssize count;
            if (remaining == 0)
                break;
            count = g_socket_receive_with_blocking(
                socket, buffer + length, sizeof(buffer) - 1 - length,
                FALSE, NULL, &error);
            if (count <= 0)
                break;
            length += (gsize)count;
            if (!g_socket_condition_timed_wait(socket, G_IO_IN, remaining,
                                                NULL, &error))
                break;
        }
    }
    if (length > 0) {
        buffer[length] = '\0';
        result = dm_parse_hddtemp_response(buffer, selected_real_device);
    }
    g_clear_error(&error);
    g_io_stream_close(G_IO_STREAM(connection), NULL, NULL);
    g_object_unref(connection);
    g_object_unref(client);
    return result;
}

gboolean dm_hddtemp_due(gint64 last_attempt_us, gint64 now_us)
{
    return last_attempt_us == 0 || now_us < last_attempt_us ||
           now_us - last_attempt_us >= 60 * G_TIME_SPAN_SECOND;
}

int dm_history_origin_x(int graph_width, guint columns)
{
    if (graph_width <= 0 || columns == 0)
        return 0;
    return MAX(0, graph_width - (int)MIN(columns, (guint)graph_width));
}

gboolean dm_resolved_device_seen(GHashTable *seen, const char *resolved_real_device)
{
    if (!seen || !resolved_real_device || !*resolved_real_device)
        return FALSE;
    if (g_hash_table_contains(seen, resolved_real_device))
        return FALSE;
    g_hash_table_add(seen, g_strdup(resolved_real_device));
    return TRUE;
}

gboolean dm_combo_identity_needs_row(const char *identity,
                                    GPtrArray *rows,
                                    gboolean identity_resolved,
                                    gboolean target_already_listed)
{
    guint i;

    if (!identity || !*identity)
        return FALSE;
    if (rows) {
        for (i = 0; i < rows->len; i++)
            if (g_strcmp0(g_ptr_array_index(rows, i), identity) == 0)
                return FALSE;
    }
    return !(identity_resolved && target_already_listed);
}

char *dm_format_rate(guint64 value)
{
    static const char *units[] = {"B/s", "KiB/s", "MiB/s", "GiB/s", "TiB/s"};
    double number = (double)value;
    guint unit = 0;

    while (number >= 1024.0 && unit < G_N_ELEMENTS(units) - 1) {
        number /= 1024.0;
        unit++;
    }
    if (unit == 0)
        return g_strdup_printf("%.0f %s", number, units[unit]);
    return g_strdup_printf(unit <= 2 ? "%.1f %s" : "%.2f %s",
                          number, units[unit]);
}

gboolean dm_is_whole_disk_name(const char *name)
{
    size_t length;

    if (!name || !name[0])
        return FALSE;
    length = strlen(name);
    /* by-id links for whole disks are <model>-<serial> only after symlink
     * resolution; the kernel name itself never contains a dash, which is what
     * excludes partitions (sda1, nvme0n1p1), dm (dm-0) and md (md0). */
    if (strchr(name, '-'))
        return FALSE;
    if (!g_str_has_prefix(name, "nvme") && !g_str_has_prefix(name, "sd") &&
        !g_str_has_prefix(name, "vd") && !g_str_has_prefix(name, "hd") &&
        !g_str_has_prefix(name, "xvd"))
        return FALSE;
    if (g_str_has_prefix(name, "nvme")) {
        const char *p;
        /* nvme0n1: the namespace digit terminates the kernel name; a trailing
         * "pN" marks a partition (nvme0n1p1) and is rejected. */
        for (p = name; p < name + length; p++)
            if (*p == 'p' && p > name && g_ascii_isdigit(p[1]))
                return FALSE;
        return g_ascii_isdigit(name[length - 1]);
    }
    /* sda / sdaa / vdb: letter suffixes, no digits allowed after them. */
    {
        const char *p;
        for (p = name; p < name + length; p++)
            if (g_ascii_isdigit(*p))
                return FALSE;
    }
    return TRUE;
}

gboolean dm_by_id_is_whole_disk(const char *by_id_path)
{
    char *real = NULL;
    char *base;
    gboolean ok;

    if (!by_id_path || !g_str_has_prefix(by_id_path, "/dev/disk/by-id/"))
        return FALSE;
    real = realpath(by_id_path, NULL);
    if (!real)
        return FALSE;
    base = g_path_get_basename(real);
    ok = dm_is_whole_disk_name(base);
    g_free(base);
    free(real);
    return ok;
}

void dm_push_history(guint64 *ring, guint *head, guint *count,
                     guint64 bytes_per_second)
{
    ring[*head] = bytes_per_second;
    *head = (*head + 1) % DM_HISTORY_MAX;
    *count = MIN(*count + 1, DM_HISTORY_MAX);
}

void dm_push_temp_history(gint *ring, guint *head, guint *count,
                          gint temperature_milli)
{
    ring[*head] = temperature_milli;
    *head = (*head + 1) % DM_HISTORY_MAX;
    *count = MIN(*count + 1, DM_HISTORY_MAX);
}

gint dm_temp_history_value(const gint *ring, guint head, guint count,
                           guint age_from_newest)
{
    guint index;

    if (age_from_newest >= count)
        return 0;
    index = (head + DM_HISTORY_MAX - 1 - age_from_newest) % DM_HISTORY_MAX;
    return ring[index];
}

guint64 dm_history_value(const guint64 *ring, guint head, guint count,
                         guint age_from_newest)
{
    guint index;

    if (age_from_newest >= count)
        return 0;
    index = (head + DM_HISTORY_MAX - 1 - age_from_newest) % DM_HISTORY_MAX;
    return ring[index];
}

gboolean dm_parse_rgba(const char *text, gdouble rgba[4])
{
    char *end = NULL;
    gdouble values[4];
    int i;

    if (!text || !*text || (!g_ascii_isdigit(*text) && *text != '.'))
        return FALSE;
    for (i = 0; i < 4; i++) {
        errno = 0;
        values[i] = g_ascii_strtod(text, &end);
        if (errno || end == text || !isfinite(values[i]) ||
            values[i] < 0.0 || values[i] > 1.0)
            return FALSE;
        if (i < 3) {
            if (*end != ',' || end[1] == ',' || end[1] == ' ' || end[1] == '\t')
                return FALSE;
            text = end + 1;
        } else if (*end)
            return FALSE;
    }
    memcpy(rgba, values, sizeof(values));
    return TRUE;
}

char *dm_format_rgba(const gdouble rgba[4])
{
    char values[4][G_ASCII_DTOSTR_BUF_SIZE];
    int i;

    for (i = 0; i < 4; i++)
        g_ascii_formatd(values[i], sizeof(values[i]), "%.6f",
                        CLAMP(rgba[i], 0.0, 1.0));
    return g_strdup_printf("%s,%s,%s,%s", values[0], values[1], values[2], values[3]);
}

gboolean dm_temperature_history_enabled(const char *value)
{
    return value && (g_ascii_strcasecmp(value, "true") == 0 ||
                     strcmp(value, "1") == 0 ||
                     g_ascii_strcasecmp(value, "yes") == 0);
}

void dm_fill_rgba(const gdouble input[4], gdouble fill_alpha, gdouble output[4])
{
    output[0] = input[0];
    output[1] = input[1];
    output[2] = input[2];
    output[3] = CLAMP(fill_alpha, 0.0, 1.0);
}

void dm_history_clear(DmHistoryState *state)
{
    if (state)
        memset(state, 0, sizeof(*state));
}

void dm_chart_geometry(int graph_height, int *graph_y, int *graph_h)
{
    *graph_y = 0;
    *graph_h = MAX(1, graph_height);
}

gboolean dm_temp_scale(gint observed_min, gint observed_max, gint *scale_min,
                       gint *scale_max)
{
    gint span, pad;

    if (observed_min == G_MININT || observed_max == G_MININT)
        return FALSE;
    if (observed_max < observed_min)
        return FALSE;
    if (observed_max == 0 && observed_min == 0)
        return FALSE;
    span = observed_max - observed_min;
    pad = MAX(1000, span / 2);
    *scale_min = observed_min - pad;
    *scale_max = observed_max + pad;
    return *scale_max > *scale_min;
}

guint dm_history_columns(int width, guint available)
{
    if (available == 0 || width <= 0)
        return 0;
    return MIN(available, (guint)width);
}

/* Blocker 1: order-independent selection by the (driver rank, label rank)
 * tuple — directory order can never make Sensor 1 beat NVMe Composite.
 * Locate the hwmon attribute directory for a block device and return the best
 * and second-best temperature input paths.
 *
 * The sysfs depth differs by transport: NVMe exposes its attributes directly in
 * device/hwmonN, while a SATA/SAS drive behind the ata_piix bridge inserts an
 * intermediate device/hwmon/hwmonN directory. The directory is probed first
 * and only descended into when it holds no "name" file, so both layouts resolve
 * to the same flat attribute directory. */
char *dm_find_hwmon_temp(const char *block_root, const char *device_name,
                         char **secondary)
{
    GDir *dir;
    const char *entry;
    char *device_dir, *best = NULL, *second = NULL;
    int best_driver = 9, best_label = 9;
    int second_driver = 9, second_label = 9;

    if (secondary)
        *secondary = NULL;
    if (!block_root || !device_name)
        return NULL;
    device_dir = g_build_filename(block_root, device_name, "device", NULL);
    dir = g_dir_open(device_dir, 0, NULL);
    g_free(device_dir);
    if (!dir)
        return NULL;
    while ((entry = g_dir_read_name(dir))) {
        char *child, *name_path, *hwmon_name = NULL;
        GDir *hdir;
        const char *file;
        int driver_rank;
        char *probe = NULL;
        if (!g_str_has_prefix(entry, "hwmon"))
            continue;
        child = g_build_filename(block_root, device_name, "device", entry, NULL);
        name_path = g_build_filename(child, "name", NULL);
        if (g_file_test(name_path, G_FILE_TEST_EXISTS)) {
            probe = child;
        } else {
            GDir *probe_dir = g_dir_open(child, 0, NULL);
            const char *sub;
            if (probe_dir) {
                while ((sub = g_dir_read_name(probe_dir))) {
                    char *cand, *cand_name;
                    if (!g_str_has_prefix(sub, "hwmon"))
                        continue;
                    cand = g_build_filename(child, sub, NULL);
                    cand_name = g_build_filename(cand, "name", NULL);
                    if (g_file_test(cand_name, G_FILE_TEST_EXISTS)) {
                        g_free(probe);
                        probe = cand;
                    } else {
                        g_free(cand);
                    }
                    g_free(cand_name);
                }
                g_dir_close(probe_dir);
            }
            if (probe) {
                g_free(name_path);
                name_path = g_build_filename(probe, "name", NULL);
            }
        }
        if (g_file_get_contents(name_path, &hwmon_name, NULL, NULL))
            g_strstrip(hwmon_name);
        if (hwmon_name && !strcmp(hwmon_name, "drivetemp")) driver_rank = 0;
        else if (hwmon_name && !strcmp(hwmon_name, "nvme")) driver_rank = 1;
        else driver_rank = 9;
        /* probe may be NULL when the entry is neither a hwmon dir nor holds a
         * nested one; rank 9 then skips the open below. */
        hdir = (driver_rank < 9 && probe) ? g_dir_open(probe, 0, NULL) : NULL;
        if (hdir) {
            while ((file = g_dir_read_name(hdir))) {
                char *label_path, *label = NULL, *label_base, *label_name;
                int label_rank;
                if (!g_str_has_prefix(file, "temp") || !strstr(file, "_input"))
                    continue;
                label_base = g_strndup(file, strlen(file) - strlen("_input"));
                label_name = g_strconcat(label_base, "_label", NULL);
                g_free(label_base);
                label_path = g_build_filename(probe, label_name, NULL);
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
                    best = g_build_filename(probe, file, NULL);
                    best_driver = driver_rank;
                    best_label = label_rank;
                } else if (!second ||
                           dm_hwmon_candidate_preferred(driver_rank, label_rank,
                                                        second_driver,
                                                        second_label)) {
                    g_free(second);
                    second = g_build_filename(probe, file, NULL);
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
        if (probe != child)
            g_free(probe);
    }
    g_dir_close(dir);
    if (secondary)
        *secondary = second;
    else
        g_free(second);
    return best;
}

/* Report the block device's LOGICAL block size for diagnostics only.
 *
 * This value MUST NOT be used to convert /proc/diskstats sector counters:
 * those are always expressed in fixed 512-byte units regardless of
 * logical_block_size (see Documentation/admin-guide/iostats.rst), and
 * multiplying by logical_block_size instead is the classic way to report
 * 8x or 100x the real throughput on 4Kn devices. It is logged so an operator
 * comparing against another monitor can see the two are unrelated. */
int dm_disk_logical_sector_size(const char *device_name)
{
    char *path;
    char *text = NULL;
    int value = 0;

    if (!device_name || !*device_name)
        return 0;
    path = g_strdup_printf("/sys/class/block/%s/queue/logical_block_size",
                           device_name);
    if (g_file_get_contents(path, &text, NULL, NULL) && text) {
        gint64 parsed;
        char *end = NULL;
        parsed = g_ascii_strtoll(text, &end, 10);
        if (end != text && parsed > 0 && parsed <= 65536)
            value = (int)parsed;
    }
    g_free(text);
    g_free(path);
    return value;
}

int dm_hwmon_temp_priority(const char *driver_name, const char *label)
{
    if (driver_name && !strcmp(driver_name, "drivetemp"))
        return 0;
    if (driver_name && !strcmp(driver_name, "nvme")) {
        if (label && !strcmp(label, "Composite"))
            return 0;
        if (label && !strcmp(label, "Sensor 1"))
            return 1;
        return 2;
    }
    return 9;
}

void dm_graph_background_rgba(const gdouble input[4], gdouble output[4])
{
    if (!input || !output)
        return;
    memcpy(output, input, 4 * sizeof(gdouble));
}

gboolean dm_by_id_link_name_is_candidate(const char *link_name)
{
    const char *p;

    if (!link_name || !link_name[0])
        return FALSE;
    /* Only the exact lowercase udev partition suffix "-part" + decimal N is
     * rejected. Names merely containing/ending in "part" remain candidates. */
    p = link_name;
    while ((p = strstr(p, "-part")) != NULL) {
        const char *digits = p + 5;
        p = digits;
        if (g_ascii_isdigit(*digits)) {
            while (g_ascii_isdigit(*digits))
                digits++;
            if (*digits == '\0')
                return FALSE;
        }
    }
    return !g_str_has_prefix(link_name, "dm-") &&
           !g_str_has_prefix(link_name, "md-");
}
