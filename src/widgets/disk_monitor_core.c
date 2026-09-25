#include "disk_monitor_core.h"

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

void dm_device_state_clear(DmDeviceState *state)
{
    if (!state)
        return;
    g_free(state->by_id);
    g_free(state->device_name);
    g_free(state->device_path);
    g_free(state->temp_path);
    memset(state, 0, sizeof(*state));
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
    state->by_id = new_by_id;
    state->device_name = new_device_name;
    state->device_path = new_device_path;
    state->temp_path = NULL;
    state->resolved = new_by_id && new_device_name && new_device_path;
    if (device_name != old_device_name)
        g_free(device_name);
    if (device_path != old_device_path)
        g_free(device_path);
    g_free(old_by_id);
    g_free(old_device_name);
    g_free(old_device_path);
    g_free(old_temp_path);
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
    /* Linux always reports 512-byte sectors in the block layer, including
     * 4Kn devices: the count is normalized, not raw. */
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
