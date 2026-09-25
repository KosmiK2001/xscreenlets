#ifndef DISK_MONITOR_CORE_H
#define DISK_MONITOR_CORE_H

#include <glib.h>
#include <gio/gio.h>
#include <cairo.h>

typedef struct _GtkWidget GtkWidget;

#define DM_HISTORY_MAX 4096U
#define DM_DEFAULT_HEIGHT 220
/* Smallest usable applet. Below ~60 px of height the two temperature lines
 * overlap each other, and below ~80 px of width the graph has no room left for
 * columns, so this is the floor rather than an arbitrary number. */
#define DM_MIN_WINDOW_WIDTH 80
#define DM_MIN_WINDOW_HEIGHT 60

typedef struct {
    guint64 read_bytes;
    guint64 write_bytes;
    gboolean io_valid;
    gint temperature_milli;
    gboolean temperature_valid;
} DiskSample;

typedef struct {
    guint64 read[DM_HISTORY_MAX];
    guint64 write[DM_HISTORY_MAX];
    gint temp[DM_HISTORY_MAX];
    guint read_head, write_head, temp_head;
    guint read_count, write_count, temp_count;
    guint64 previous_read, previous_write;
    gint64 previous_time_us;
    gboolean previous_valid;
} DmHistoryState;
typedef struct {
    char *by_id;
    char *device_name;
    char *device_path;
    char *temp_path;
    /* Some NVMe drives expose a composite sensor plus a second one; both are
     * shown. The graph still uses the primary temp_path sensor. */
    char *temp_path_secondary;
    gint secondary_milli;
    gboolean resolved;
    DiskSample sample;
    DmHistoryState history;
    gint64 last_hddtemp_attempt_us;
    gint cached_hddtemp_milli;
    gboolean cached_hddtemp_valid;
    gint64 last_resolve_attempt_us;
} DmDeviceState;

typedef enum {
    DM_RESOLUTION_MISSING,
    DM_RESOLUTION_MATCH,
    DM_RESOLUTION_MOVED
} DmResolutionResult;

/* Refcounted Properties-dialog context. It carries the STABLE INSTANCE NAME
 * only — never an XsPlugin or PrivData pointer — because the dialog can outlive
 * a plugin restart; callbacks resolve the live plugin by name. */
typedef struct {
    gint refcount;
    char *instance_name;
} DmDialogContext;

typedef struct {
    const char *label;
    const char *x_key;
    const char *y_key;
    int x;
    int y;
    int x_max;
    int y_max;
} DmPositionPairSpec;

gboolean dm_position_pair_spec(const char *label,
                               const char *x_key,
                               const char *y_key,
                               int x, int y,
                               int x_max, int y_max,
                               DmPositionPairSpec *out);
GtkWidget *dm_position_pair_widget(const char *label,
                                   const char *x_key,
                                   const char *y_key,
                                   int x, int y,
                                   int x_max, int y_max,
                                   GtkWidget **x_spin,
                                   GtkWidget **y_spin);

typedef struct {
    const char *title;
    const char *font_key;
    const char *text_color_key;
    const char *x_key;
    const char *y_key;
    const char *history_color_key;
    const char *font;
    const char *history_label;
    gdouble text_color[4];
    gdouble history_color[4];
    int x;
    int y;
} DmSeriesBlockSpec;

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
                               DmSeriesBlockSpec *out);
GtkWidget *dm_series_block_widget(const DmSeriesBlockSpec *spec,
                                  GtkWidget **content);
GtkWidget *dm_disk_selector_widget(GtkWidget *combo);
char *dm_format_temperature(const gint *milli, guint count);
char **dm_format_temperature_lines(const gint *milli, guint count,
                                   guint *out_lines);
GtkWidget *dm_properties_scroller(GtkWidget *page, int width, int height);
void dm_properties_size(const char *plugin_type, int *width, int *height);

DmDialogContext *dm_dialog_context_new(const char *instance_name);
DmDialogContext *dm_dialog_context_ref(DmDialogContext *ctx);
void dm_dialog_context_unref(DmDialogContext *ctx);
const char *dm_dialog_context_name(const DmDialogContext *ctx);
gint dm_dialog_context_refcount(const DmDialogContext *ctx);

typedef struct {
    char *instance_name;
    char *by_id;
    char *device_path;
    guint64 generation;
    guint64 request_id;
} DmTemperatureRequest;

gboolean dm_temperature_request_matches(const DmTemperatureRequest *request,
                                       const char *instance_name,
                                       guint64 generation,
                                       const char *by_id,
                                       const char *device_path,
                                       gboolean resolved);
gboolean dm_temperature_completion_apply(DiskSample *sample,
                                        const DmTemperatureRequest *request,
                                        gint temperature_milli,
                                        const char *instance_name,
                                        guint64 generation,
                                        const char *by_id,
                                        const char *device_path,
                                        gboolean resolved);
void dm_temperature_request_clear(DmTemperatureRequest *request);

int dm_scale_position(int value, int design_size, int live_size, int max);
int dm_fit_text_coordinate(int anchor, int text_extent, int limit);
/* Normalise a corner-radius setting and report the shape to clip with.
 * A radius of 0 means "square corners" and is returned as such. */
double dm_corner_radius_value(int value);
gboolean dm_corner_radius_is_rounded(double radius);

/* Build the rounded window outline as a cairo region. The plugin needs this
 * twice: once to clip its own drawing, and once to hand the SAME outline to
 * the X server as the window's visible shape. A cairo clip alone is invisible
 * — it only limits what the plugin paints and does not make the rectangular X
 * window transparent at the corners. Returns a region the caller owns, or
 * NULL when the radius is 0 (square corners; the caller must then clear any
 * shape it set earlier). */
cairo_region_t *dm_rounded_region(int width, int height, int radius);
void dm_device_state_secondary_reset(DmDeviceState *state);
void dm_device_state_clear(DmDeviceState *state);
void dm_device_state_replace_owned(DmDeviceState *state,
                                   const char *by_id,
                                   char *device_name,
                                   char *device_path);
gboolean dm_device_state_prepare(DmDeviceState *out, const char *by_id);
gboolean dm_device_state_is_whole_disk(const DmDeviceState *state);
char *dm_switch_config_by_id(const DmDeviceState *state);

gint64 dm_retry_interval_us(void);
gboolean dm_retry_due(gint64 last_attempt_us, gint64 now_us);
char *dm_identity_label(const char *device_name, const char *by_id);
gboolean dm_by_id_path_is_direct_symlink(const char *path, const char *dir);
gboolean dm_by_id_path_is_valid_configured(const char *path);
DmResolutionResult dm_resolved_target_matches(const char *path,
                                              const char *expected_real);

void dm_sample_mark_invalid(DiskSample *sample);
gboolean dm_sample_current_rates(const DiskSample *sample, guint64 *read, guint64 *write);
long dm_hwmon_candidate_key(int driver_rank, int label_rank);
gboolean dm_hwmon_candidate_preferred(int driver_rank, int label_rank,
                                      int best_driver_rank, int best_label_rank);
gboolean dm_temp_history_is_valid(const gint *ring, guint head, guint count,
                                  guint age_from_newest);
guint dm_temp_history_valid_run(const gint *ring, guint head, guint count);
gboolean dm_temp_history_observes(const gint *ring, guint head, guint count,
                                  gint value);
void dm_history_clear(DmHistoryState *state);

gboolean dm_parse_diskstats_named(const char *text, const char *device_name,
                                  guint64 *sectors_read, guint64 *sectors_written);
guint64 dm_sectors_to_bytes(guint64 sectors);
gint64 dm_rate_bytes_per_second(guint64 previous, guint64 current,
                                guint64 elapsed_us);
void dm_diskstats_sample_failed(DmHistoryState *history, gint64 now_us);
gint dm_parse_temperature(const char *text);
gint dm_parse_hddtemp_response(const char *response, const char *selected_real_device);
gint dm_hddtemp_query(const char *host, guint16 port,
                      const char *selected_real_device, guint timeout_ms);
gboolean dm_hddtemp_start_allowed(gpointer current_task, gboolean resolved,
                                  gboolean has_identity, gboolean due);
gboolean dm_hddtemp_completion_is_current(gpointer current_task,
                                          gpointer completion_task);
gint64 dm_deadline_remaining_us(gint64 deadline_us, gint64 now_us);
gboolean dm_hddtemp_due(gint64 last_attempt_us, gint64 now_us);
int dm_history_origin_x(int graph_width, guint columns);
gboolean dm_resolved_device_seen(GHashTable *seen, const char *resolved_real_device);
gboolean dm_combo_identity_needs_row(const char *identity,
                                    GPtrArray *rows,
                                    gboolean identity_resolved,
                                    gboolean target_already_listed);
char *dm_format_rate(guint64 bytes_per_second);
gboolean dm_is_whole_disk_name(const char *name);
gboolean dm_by_id_is_whole_disk(const char *by_id_path);
void dm_push_history(guint64 *ring, guint *head, guint *count,
                     guint64 bytes_per_second);
void dm_push_temp_history(gint *ring, guint *head, guint *count,
                          gint temperature_milli);
gint dm_temp_history_value(const gint *ring, guint head, guint count,
                           guint age_from_newest);
guint64 dm_history_value(const guint64 *ring, guint head, guint count,
                         guint age_from_newest);
gboolean dm_parse_rgba(const char *text, gdouble rgba[4]);
char *dm_format_rgba(const gdouble rgba[4]);
gboolean dm_temperature_history_enabled(const char *value);
void dm_fill_rgba(const gdouble input[4], gdouble fill_alpha, gdouble output[4]);
void dm_chart_geometry(int graph_height, int *graph_y, int *graph_h);
gboolean dm_temp_scale(gint observed_min, gint observed_max, gint *scale_min,
                       gint *scale_max);
guint dm_history_columns(int width, guint available);
gboolean dm_by_id_link_name_is_candidate(const char *link_name);
int dm_hwmon_temp_priority(const char *driver_name, const char *label);
void dm_graph_background_rgba(const gdouble input[4], gdouble output[4]);

#endif
