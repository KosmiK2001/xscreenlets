/* cpu_monitor.c — per-socket, topology-aware C/GTK3 CPU monitor.
 *
 * All sampling is performed by tick().  draw() only paints the cached Cairo
 * surface, so GTK draw notifications never cause file I/O. */
#include <gtk/gtk.h>
#include <glib.h>
#include <glib/gstdio.h>
#include <pango/pangocairo.h>
#include <cairo.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <inttypes.h>
#ifdef __linux__
#include <dirent.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <unistd.h>
#endif
#include "xs_api.h"
#include "common.h"

#define CM_SYS_CPU_DIR "/sys/devices/system/cpu"
#define CM_HWMON_DIR "/sys/class/hwmon"
#define CM_MAX_CPUS 4096
#define CM_DEFAULT_FONT "Ubuntu 7"
#define CM_COLUMNS_DEFAULT 4
#define CM_ROWS_DEFAULT 2
#define CM_CELL_WIDTH 50
#define CM_CELL_HEIGHT 76
#define CM_BLOCK_HEIGHT 9.0
#define CM_TEXT_HEIGHT 24.0
#define CM_DEFAULT_WINDOW_WIDTH 200
#define CM_DEFAULT_WINDOW_HEIGHT 152
#define CM_HISTORY_MIN_POINTS 1
#define CM_HISTORY_MAX_POINTS 4096
#define CM_LOAD_COMPONENTS 4

typedef struct {
    guint64 user;
    guint64 nice;
    guint64 system;
    guint64 iowait;
    guint64 total;         /* all jiffies, including idle/irq/softirq/steal */
    gboolean valid;
} CpuTimes;

typedef struct {
    gdouble thread_load[2][4];
} CpuHistorySlot;

typedef struct {
    gint core_id;
    GArray *siblings;       /* logical CPU numbers, ascending */
    CpuHistorySlot *history; /* circular per-thread samples */
    guint history_points;
    gdouble load[4];        /* combined physical-core load, normalized 0..1 */
    gdouble thread_load[2][4]; /* first sibling top, second sibling bottom */
    gdouble frequency_mhz;  /* maximum current frequency across siblings */
    gdouble temperature;    /* degrees C, NAN if unavailable */
} CoreData;

typedef struct {
    XsPlugin *plugin;
    gboolean has_sample;
    GKeyFile *kf;
    gint socket_id;
    guint update_ms;
    gint columns;
    gint rows;
    int window_width;
    int window_height;
    char *font;
    gdouble background_color[4];
    gdouble text_color[4];
    gdouble temp_color[4];
    gdouble load_colors[4][4];
    CpuTimes previous[CM_MAX_CPUS];
    gboolean stat_valid[CM_MAX_CPUS];
    GPtrArray *cores;
    GPtrArray *hwmon_temps; /* char * paths, one per core; may contain NULL */
    guint history_points;
    guint history_head;
    guint history_count;
    cairo_surface_t *cache;
    int cache_width;
    int cache_height;
} PrivData;

static const gdouble cm_color_background[4] = {0.04, 0.04, 0.05, 0.88};
static const gdouble cm_color_text[4] = {0.88, 0.90, 0.94, 1.0};
static const gdouble cm_color_temp[4] = {1.0, 0.38, 0.18, 1.0};
static const gdouble cm_color_system[4] = {0.16, 0.58, 0.95, 1.0};
static const gdouble cm_color_user[4] = {0.25, 0.82, 0.35, 1.0};
static const gdouble cm_color_nice[4] = {0.95, 0.75, 0.18, 1.0};
static const gdouble cm_color_io[4] = {0.80, 0.24, 0.24, 1.0};
static GHashTable *cm_socket_claims;

static gboolean cm_parse_color(const char *text, gdouble out[4])
{
    const char *p = text;
    int i;

    if (!text || !text[0])
        return FALSE;
    for (i = 0; i < 4; i++) {
        char *end = NULL;
        gdouble value = g_ascii_strtod(p, &end);

        if (end == p || !isfinite(value))
            return FALSE;
        out[i] = CLAMP(value, 0.0, 1.0);
        p = end;
        while (g_ascii_isspace(*p))
            p++;
        if (i < 3) {
            if (*p != ',')
                return FALSE;
            p++;
        } else if (*p != '\0') {
            return FALSE;
        }
    }
    return TRUE;
}

static char *cm_color_string(const GdkRGBA *c)
{
    char r[32], g[32], b[32], a[32];
    char *out;

    g_ascii_formatd(r, sizeof(r), "%.9g", c->red);
    g_ascii_formatd(g, sizeof(g), "%.9g", c->green);
    g_ascii_formatd(b, sizeof(b), "%.9g", c->blue);
    g_ascii_formatd(a, sizeof(a), "%.9g", c->alpha);
    out = g_strdup_printf("%s,%s,%s,%s", r, g, b, a);
    return out;
}

static void cm_read_color(PrivData *priv, const char *key,
                          const gdouble def[4], gdouble out[4])
{
    char *s = xs_host_api()->conf_str(priv->kf, priv->plugin->name, key, NULL);

    if (!s) {
        GdkRGBA c = {def[0], def[1], def[2], def[3]};
        char *fallback = cm_color_string(&c);

        memcpy(out, def, 4 * sizeof(gdouble));
        g_key_file_set_string(priv->kf, priv->plugin->name, key, fallback);
        g_free(fallback);
    } else {
        memcpy(out, def, 4 * sizeof(gdouble));
        if (!cm_parse_color(s, out))
            priv->plugin->host->log("cpu_monitor %s: invalid %s, using defaults",
                                    priv->plugin->name, key);
    }
    g_free(s);
}

static gint cm_read_int_file(const char *path, gint *value)
{
    char *text = NULL;
    gchar *end = NULL;
    gint64 v;

    if (!g_file_get_contents(path, &text, NULL, NULL))
        return FALSE;
    {
        gchar *start = g_strstrip(text);
        v = g_ascii_strtoll(start, &end, 10);
        if (end == start) {
            g_free(text);
            return FALSE;
        }
    }
    g_free(text);
    *value = (gint)v;
    return TRUE;
}

static gint64 cm_read_frequency_khz(const char *path)
{
    char *text = NULL;
    gchar *end = NULL;
    gint64 value;

    if (!g_file_get_contents(path, &text, NULL, NULL))
        return -1;
    {
        gchar *start = g_strstrip(text);
        value = g_ascii_strtoll(start, &end, 10);
        if (end == start || *end != '\0' || value < 0) {
            g_free(text);
            return -1;
        }
    }
    g_free(text);
    return value;
}

static double cm_read_temperature(const char *path)
{
    char *text = NULL;
    gchar *end = NULL;
    gint64 value;

    if (!path || !g_file_get_contents(path, &text, NULL, NULL))
        return NAN;
    {
        gchar *start = g_strstrip(text);
        value = g_ascii_strtoll(start, &end, 10);
        if (end == start || *end != '\0') {
            g_free(text);
            return NAN;
        }
    }
    g_free(text);
    return (double)value / 1000.0;
}

static gboolean cm_parse_cpu_number(const char *name, gint *cpu)
{
    const char *s = name;
    gchar *end = NULL;
    gint64 value;

    if (!g_str_has_prefix(s, "cpu") || !g_ascii_isdigit(s[3]))
        return FALSE;
    value = g_ascii_strtoll(s + 3, &end, 10);
    if (end == s + 3 || *end != '\0' || value < 0 || value >= CM_MAX_CPUS)
        return FALSE;
    *cpu = (gint)value;
    return TRUE;
}

static gboolean cm_parse_cpu_list(const char *text, GArray *cpus)
{
    gchar **parts;
    gboolean ok = TRUE;
    guint i;

    if (!text)
        return FALSE;
    parts = g_strsplit_set(text, ",\n\r ", -1);
    for (i = 0; parts[i]; i++) {
        gchar *dash;
        gchar *left;
        gchar *end = NULL;
        gint64 lo, hi, n;

        if (!parts[i][0])
            continue;
        dash = strchr(parts[i], '-');
        left = dash ? g_strdup(parts[i]) : g_strdup(parts[i]);
        if (dash)
            *dash = '\0';
        lo = g_ascii_strtoll(g_strstrip(left), &end, 10);
        if (end == left || *end != '\0' || lo < 0 || lo >= CM_MAX_CPUS) {
            ok = FALSE;
        } else {
            hi = lo;
            if (dash) {
                hi = g_ascii_strtoll(dash + 1, &end, 10);
                if (end == dash + 1 || *end != '\0' || hi < lo ||
                    hi >= CM_MAX_CPUS)
                    ok = FALSE;
            }
            for (n = lo; ok && n <= hi; n++) {
                gint cpu = (gint)n;
                g_array_append_val(cpus, cpu);
            }
        }
        g_free(left);
    }
    g_strfreev(parts);
    return ok;
}

static GArray *cm_siblings_for_cpu(gint cpu)
{
    char *cpu_name = g_strdup_printf("cpu%d", cpu);
    char *topology = g_build_filename(CM_SYS_CPU_DIR, cpu_name, "topology", NULL);
    char *path = g_build_filename(topology, "thread_siblings_list", NULL);
    char *text = NULL;
    GArray *cpus = g_array_new(FALSE, FALSE, sizeof(gint));

    if (!g_file_get_contents(path, &text, NULL, NULL) ||
        !cm_parse_cpu_list(text, cpus)) {
        g_array_unref(cpus);
        cpus = NULL;
    }
    g_free(text);
    g_free(path);
    g_free(topology);
    g_free(cpu_name);
    return cpus;
}

static gint cm_core_cmp(gconstpointer a, gconstpointer b)
{
    const CoreData *ca = *(const CoreData * const *)a;
    const CoreData *cb = *(const CoreData * const *)b;

    if (ca->core_id != cb->core_id)
        return ca->core_id < cb->core_id ? -1 : 1;
    return 0;
}

static void cm_discover_topology(PrivData *priv)
{
    GDir *dir = g_dir_open(CM_SYS_CPU_DIR, 0, NULL);
    const char *name;
    GHashTable *seen = g_hash_table_new(g_direct_hash, g_direct_equal);

    g_ptr_array_set_size(priv->cores, 0);
    if (dir) {
        while ((name = g_dir_read_name(dir))) {
            gint cpu;
            char *base, *package_path, *core_path;
            gint package = -1, core_id = -1;
            GArray *siblings;
            CoreData *core;
            guint i;
            gboolean found = FALSE;

            if (!cm_parse_cpu_number(name, &cpu))
                continue;
            base = g_build_filename(CM_SYS_CPU_DIR, name, NULL);
            package_path = g_build_filename(base, "topology",
                                            "physical_package_id", NULL);
            core_path = g_build_filename(base, "topology", "core_id", NULL);
            if (cm_read_int_file(package_path, &package) &&
                cm_read_int_file(core_path, &core_id) && package == priv->socket_id) {
                siblings = cm_siblings_for_cpu(cpu);
                if (siblings) {
                    for (i = 0; i < priv->cores->len; i++) {
                        CoreData *old = g_ptr_array_index(priv->cores, i);
                        if (old->core_id == core_id) {
                            found = TRUE;
                            break;
                        }
                    }
                    if (!found) {
                        core = g_new0(CoreData, 1);
                        core->core_id = core_id;
                        core->siblings = siblings;
                        core->temperature = NAN;
                        g_ptr_array_add(priv->cores, core);
                    } else {
                        g_array_unref(siblings);
                    }
                }
            }
            g_free(core_path);
            g_free(package_path);
            g_free(base);
            if (g_hash_table_contains(seen, GINT_TO_POINTER(cpu + 1)))
                continue;
            g_hash_table_add(seen, GINT_TO_POINTER(cpu + 1));
        }
        g_dir_close(dir);
    }
    g_hash_table_destroy(seen);
    g_ptr_array_sort(priv->cores, cm_core_cmp);
}

static void cm_discover_temperatures(PrivData *priv)
{
    guint core_count = priv->cores ? priv->cores->len : 0;
    guint i;
    GDir *dir;

    while (priv->hwmon_temps->len > 0)
        g_ptr_array_remove_index(priv->hwmon_temps,
                                 priv->hwmon_temps->len - 1);
    for (i = 0; i < core_count; i++)
        g_ptr_array_add(priv->hwmon_temps, NULL);

    dir = g_dir_open(CM_HWMON_DIR, 0, NULL);
    if (!dir)
        return;
    {
        const char *hwmon;
        while ((hwmon = g_dir_read_name(dir))) {
            char *name_path, *name_text = NULL;
            gint input;
            gboolean package_match = FALSE;

            if (strncmp(hwmon, "hwmon", 5) != 0)
                continue;
            name_path = g_build_filename(CM_HWMON_DIR, hwmon, "name", NULL);
            if (g_file_get_contents(name_path, &name_text, NULL, NULL))
                g_strstrip(name_text);
            g_free(name_path);
            if (!name_text || g_ascii_strcasecmp(name_text, "coretemp") != 0) {
                g_free(name_text);
                continue;
            }
            g_free(name_text);
            for (input = 1; input < 10000; input++) {
                char suffix[32], *label_path, *label = NULL;
                snprintf(suffix, sizeof(suffix), "temp%d_label", input);
                label_path = g_build_filename(CM_HWMON_DIR, hwmon, suffix, NULL);
                if (g_file_get_contents(label_path, &label, NULL, NULL))
                    g_strstrip(label);
                g_free(label_path);
                if (!label)
                    continue;
                if (g_str_has_prefix(label, "Package id ")) {
                    gint package = (gint)g_ascii_strtoull(label + 11, NULL, 10);
                    package_match = package == priv->socket_id;
                }
                g_free(label);
                if (package_match)
                    break;
            }
            if (!package_match)
                continue;
            for (input = 1; input < 10000; input++) {
                char suffix[32], *label_path, *label = NULL;
                gint core_id;
                guint c;

                snprintf(suffix, sizeof(suffix), "temp%d_label", input);
                label_path = g_build_filename(CM_HWMON_DIR, hwmon, suffix, NULL);
                if (g_file_get_contents(label_path, &label, NULL, NULL))
                    g_strstrip(label);
                g_free(label_path);
                if (!label || sscanf(label, "Core %d", &core_id) != 1) {
                    g_free(label);
                    continue;
                }
                g_free(label);
                for (c = 0; c < core_count; c++) {
                    CoreData *core = g_ptr_array_index(priv->cores, c);
                    if (core->core_id == core_id) {
                        char *input_path;
                        snprintf(suffix, sizeof(suffix), "temp%d_input", input);
                        input_path = g_build_filename(CM_HWMON_DIR, hwmon,
                                                      suffix, NULL);
                        if (c < priv->hwmon_temps->len)
                            g_ptr_array_index(priv->hwmon_temps, c) = input_path;
                        else
                            g_free(input_path);
                    }
                }
            }
            break;
        }
        g_dir_close(dir);
    }
}

static void cm_free_core(gpointer data)
{
    CoreData *core = data;

    if (!core)
        return;
    g_array_unref(core->siblings);
    g_free(core->history);
    g_free(core);
}

static gboolean cm_ensure_history(PrivData *priv, guint points)
{
    guint old_count = priv->history_count;
    guint old_head = priv->history_head;
    guint c;

    points = CLAMP(points, CM_HISTORY_MIN_POINTS, CM_HISTORY_MAX_POINTS);
    priv->history_points = points;
    for (c = 0; c < priv->cores->len; c++) {
        CoreData *core = g_ptr_array_index(priv->cores, c);
        CpuHistorySlot *old_history = core->history;
        guint old_core_points = core->history_points;
        guint keep;

        if (old_core_points == points) {
            if (old_core_points == 0)
                core->history = g_new0(CpuHistorySlot, points);
            continue;
        }
        core->history = g_new0(CpuHistorySlot, points);
        core->history_points = points;
        keep = MIN(old_count, MIN(points, old_core_points));
        if (old_history && keep) {
            guint start = (old_head + old_core_points - keep) % old_core_points;
            guint i;
            for (i = 0; i < keep; i++)
                memcpy(&core->history[i],
                       &old_history[(start + i) % old_core_points],
                       sizeof(CpuHistorySlot));
        }
        g_free(old_history);
    }
    priv->history_head = MIN(old_count, points);
    priv->history_count = MIN(old_count, points);
    return TRUE;
}

static void cm_push_history(PrivData *priv)
{
    guint c;

    if (!priv->history_points)
        return;
    for (c = 0; c < priv->cores->len; c++) {
        CoreData *core = g_ptr_array_index(priv->cores, c);
        CpuHistorySlot *slot;

        if (!core->history || core->history_points == 0)
            continue;
        slot = &core->history[priv->history_head];
        memcpy(slot->thread_load, core->thread_load, sizeof(slot->thread_load));
    }
    if (priv->history_points == 0)
        return;
    priv->history_head = (priv->history_head + 1) % priv->history_points;
    if (priv->history_count < priv->history_points)
        priv->history_count++;
}

static void cm_free_temp(gpointer data)
{
    g_free(data);
}

static gboolean cm_read_proc_stat(CpuTimes *current)
{
    FILE *fp = fopen("/proc/stat", "r");
    char line[512];

    if (!fp)
        return FALSE;
    while (fgets(line, sizeof(line), fp)) {
        CpuTimes value = {0, 0, 0, 0, 0, TRUE};
        char label[32];
        guint64 user, nice, system, idle, iowait, irq = 0, softirq = 0;
        guint64 steal = 0, guest = 0, guest_nice = 0, total;
        int matched, cpu;

        matched = sscanf(line, "%31s %" SCNu64 " %" SCNu64 " %" SCNu64
                         " %" SCNu64 " %" SCNu64 " %" SCNu64 " %" SCNu64
                         " %" SCNu64 " %" SCNu64 " %" SCNu64, label, &user,
                         &nice, &system, &idle, &iowait, &irq, &softirq,
                         &steal, &guest, &guest_nice);
        if (matched < 5)
            continue;
        if (g_str_has_prefix(label, "cpu")) {
            gint64 n = g_ascii_strtoll(label + 3, NULL, 10);
            cpu = (gint)n;
        } else {
            continue;
        }
        if (cpu < 0 || cpu >= CM_MAX_CPUS)
            continue;
        value.user = user;
        value.nice = nice;
        value.system = system;
        value.iowait = matched >= 6 ? iowait : 0;
        total = user + nice + system + idle + value.iowait + irq + softirq +
                steal;
        value.total = total;
        current[cpu] = value;
    }
    fclose(fp);
    return TRUE;
}

static gboolean cm_sample_cpu(PrivData *priv)
{
    CpuTimes current[CM_MAX_CPUS];
    guint c;

    memset(current, 0, sizeof(current));
    if (!cm_read_proc_stat(current))
        return FALSE;
    if (priv->has_sample) {
        for (c = 0; c < priv->cores->len; c++) {
        CoreData *core = g_ptr_array_index(priv->cores, c);
        guint64 d[4] = {0, 0, 0, 0};
        guint64 total = 0;
        guint k;
        gint64 best_freq = -1;
        double temperature = NAN;

        for (k = 0; k < core->siblings->len && k < 2; k++) {
            gint cpu = g_array_index(core->siblings, gint, k);
            guint64 td[4] = {0, 0, 0, 0};
            guint64 tt = 0;
            guint j;

            if (!priv->stat_valid[cpu] || !current[cpu].valid) {
                memset(core->thread_load[k], 0, sizeof(core->thread_load[k]));
                continue;
            }
            td[0] = current[cpu].system - priv->previous[cpu].system;
            td[1] = current[cpu].user - priv->previous[cpu].user;
            td[2] = current[cpu].nice - priv->previous[cpu].nice;
            td[3] = current[cpu].iowait - priv->previous[cpu].iowait;
            tt = current[cpu].total - priv->previous[cpu].total;
            for (j = 0; j < 4; j++)
                d[j] += td[j];
            total += tt;
            if (tt > 0)
                for (j = 0; j < 4; j++)
                    core->thread_load[k][j] = (double)td[j] / (double)tt;
        }
        if (total > 0) {
            for (k = 0; k < 4; k++)
                core->load[k] = (double)d[k] / (double)total;
        }
        for (k = 0; k < core->siblings->len; k++) {
            gint cpu = g_array_index(core->siblings, gint, k);
            char *cpu_name = g_strdup_printf("cpu%d", cpu);
            char *cpufreq = g_build_filename(CM_SYS_CPU_DIR, cpu_name,
                                             "cpufreq", NULL);
            char *path = g_build_filename(cpufreq, "scaling_cur_freq", NULL);
            gint64 frequency = cm_read_frequency_khz(path);
            if (frequency > best_freq)
                best_freq = frequency;
            g_free(path);
            g_free(cpufreq);
            g_free(cpu_name);
        }
        core->frequency_mhz = best_freq > 0 ? best_freq / 1000.0 : NAN;
        if (c < priv->hwmon_temps->len)
            temperature = cm_read_temperature(g_ptr_array_index(priv->hwmon_temps, c));
        core->temperature = temperature;
        }
    }
    for (c = 0; c < CM_MAX_CPUS; c++) {
        priv->previous[c] = current[c];
        priv->stat_valid[c] = current[c].valid;
    }
    priv->has_sample = TRUE;
    return TRUE;
}

static void cm_draw_history(cairo_t *cr, double x, double y, double width,
                            double height, CoreData *core, guint thread,
                            guint points, guint head, guint count,
                            const gdouble colors[4][4])
{
    guint slot_width = MAX(width / MAX(points, 1), 1.0);
    guint used = MIN(count, points);
    guint start = (head + points - used) % points;
    guint sample;

    cairo_rectangle(cr, x, y, width, height);
    cairo_set_source_rgba(cr, 0.12, 0.12, 0.14, 0.75);
    cairo_fill(cr);
    for (sample = 0; sample < used; sample++) {
        const gdouble *load;
        double base_y;
        guint component;

        if (points == 0)
            break;
        load = core->history[(start + sample) % points].thread_load[thread];
        base_y = y + height;
        for (component = 0; component < CM_LOAD_COMPONENTS; component++) {
            double bar_height = height * CLAMP(load[component], 0.0, 1.0);
            if (bar_height <= 0.0)
                continue;
            base_y -= bar_height;
            cairo_rectangle(cr,
                            x + (width - used * slot_width) +
                                sample * slot_width,
                            base_y, slot_width, bar_height + 0.5);
            cairo_set_source_rgba(cr, colors[component][0],
                                  colors[component][1], colors[component][2],
                                  colors[component][3]);
            cairo_fill(cr);
        }
    }
    cairo_set_source_rgba(cr, 0.75, 0.75, 0.80, 0.65);
    cairo_set_line_width(cr, 0.5);
    cairo_rectangle(cr, x + 0.25, y + 0.25, width - 0.5, height - 0.5);
    cairo_stroke(cr);
}

static cairo_surface_t *cm_render(PrivData *priv, int width, int height)
{
    cairo_surface_t *surface = cairo_image_surface_create(CAIRO_FORMAT_ARGB32,
                                                           width, height);
    cairo_t *cr = cairo_create(surface);
    PangoFontDescription *font;
    PangoLayout *layout;
    int i;

    cairo_set_operator(cr, CAIRO_OPERATOR_SOURCE);
    cairo_set_source_rgba(cr, priv->background_color[0], priv->background_color[1],
                          priv->background_color[2], priv->background_color[3]);
    cairo_paint(cr);
    cairo_set_operator(cr, CAIRO_OPERATOR_OVER);
    font = pango_font_description_from_string(priv->font);
    layout = pango_cairo_create_layout(cr);
    pango_layout_set_font_description(layout, font);
    pango_font_description_free(font);
    pango_layout_set_single_paragraph_mode(layout, TRUE);

    for (i = 0; i < (int)priv->cores->len; i++) {
        CoreData *core = g_ptr_array_index(priv->cores, i);
        int row = i / MAX(priv->columns, 1);
        int col = i % MAX(priv->columns, 1);
        double cell_left = (double)width * col / MAX(priv->columns, 1);
        double cell_right = (double)width * (col + 1) / MAX(priv->columns, 1);
        double cell_top = (double)height * row / MAX(priv->rows, 1);
        double cell_bottom = (double)height * (row + 1) / MAX(priv->rows, 1);
        double x = cell_left + 1.0;
        double y = cell_top + 1.0;
        double block_w = cell_right - cell_left - 2.0;
        double block_h = MAX(1.0, (cell_bottom - cell_top - 2.0) * 0.30);
        double middle_h = MAX(1.0, cell_bottom - cell_top -
                                      2.0 - 2.0 * block_h);
        double text_y;
        int text_height;
        char *text;

        if (row >= priv->rows)
            break;
        cairo_set_source_rgba(cr, 0.72, 0.76, 0.84, 0.90);
        cairo_set_line_width(cr, 1.0);
        cairo_rectangle(cr, cell_left + 0.5, cell_top + 0.5,
                        cell_right - cell_left - 1.0,
                        cell_bottom - cell_top - 1.0);
        cairo_stroke(cr);

        cm_draw_history(cr, x, y, block_w, block_h, core, 0,
                        core->history_points, priv->history_head,
                        priv->history_count, priv->load_colors);
        text_y = y + block_h + 1.0;
        text = isfinite(core->frequency_mhz) ?
               g_strdup_printf("%.2fG", core->frequency_mhz / 1000.0) : g_strdup("---");
        pango_layout_set_text(layout, text, -1);
        g_free(text);
        pango_layout_set_width(layout, block_w * PANGO_SCALE / 2);
        pango_layout_set_alignment(layout, PANGO_ALIGN_LEFT);
        pango_layout_set_ellipsize(layout, PANGO_ELLIPSIZE_END);
        pango_layout_get_pixel_size(layout, NULL, &text_height);
        cairo_set_source_rgba(cr, priv->text_color[0], priv->text_color[1],
                              priv->text_color[2], priv->text_color[3]);
        cairo_move_to(cr, x, text_y + MAX(0.0, (middle_h - text_height) / 2.0));
        pango_cairo_show_layout(cr, layout);

        text = isfinite(core->temperature) ?
               g_strdup_printf("%.0f°", core->temperature) : g_strdup("---");
        pango_layout_set_text(layout, text, -1);
        pango_layout_set_width(layout, block_w * PANGO_SCALE / 2);
        pango_layout_set_alignment(layout, PANGO_ALIGN_RIGHT);
        pango_layout_get_pixel_size(layout, NULL, &text_height);
        cairo_set_source_rgba(cr, priv->temp_color[0], priv->temp_color[1],
                              priv->temp_color[2], priv->temp_color[3]);
        cairo_move_to(cr, x + block_w / 2.0,
                      text_y + MAX(0.0, (middle_h - text_height) / 2.0));
        pango_cairo_show_layout(cr, layout);
        g_free(text);

        cm_draw_history(cr, x, text_y + middle_h, block_w, block_h,
                        core, 1, core->history_points, priv->history_head,
                        priv->history_count, priv->load_colors);
    }
    g_object_unref(layout);
    cairo_destroy(cr);
    cairo_surface_mark_dirty(surface);
    return surface;
}

static int cm_init(XsPlugin *p, GKeyFile *kf)
{
    PrivData *priv = g_new0(PrivData, 1);
    int x = xs_host_api()->conf_int(kf, p->name, "x", 80);
    int y = xs_host_api()->conf_int(kf, p->name, "y", 80);
    gdouble opacity = xs_host_api()->conf_dbl(kf, p->name, "opacity", 1.0);
    int width, height;

    p->priv = priv;
    priv->plugin = p;
    priv->kf = kf;
    priv->cores = g_ptr_array_new_with_free_func(cm_free_core);
    priv->hwmon_temps = g_ptr_array_new_with_free_func(cm_free_temp);
    priv->socket_id = xs_host_api()->conf_int(kf, p->name, "socket_id", 0);
    priv->update_ms = (guint)xs_host_api()->conf_int(kf, p->name, "update_ms", 1000);
    priv->columns = xs_host_api()->conf_int(kf, p->name, "columns", CM_COLUMNS_DEFAULT);
    priv->rows = xs_host_api()->conf_int(kf, p->name, "rows", CM_ROWS_DEFAULT);
    priv->window_width = xs_host_api()->conf_int(kf, p->name, "window_width",
                                                  CM_DEFAULT_WINDOW_WIDTH);
    priv->window_height = xs_host_api()->conf_int(kf, p->name, "window_height",
                                                   CM_DEFAULT_WINDOW_HEIGHT);
    priv->update_ms = CLAMP(priv->update_ms, 100U, 60000U);
    priv->columns = CLAMP(priv->columns, 1, 16);
    priv->rows = CLAMP(priv->rows, 1, 16);
    priv->window_width = CLAMP(priv->window_width, 100, 1600);
    priv->window_height = CLAMP(priv->window_height, 80, 1200);
    priv->font = xs_host_api()->conf_str(kf, p->name, "font", CM_DEFAULT_FONT);
    if (!g_key_file_has_key(kf, p->name, "font", NULL))
        g_key_file_set_string(kf, p->name, "font", priv->font);
    cm_read_color(priv, "background_color", cm_color_background,
                  priv->background_color);
    cm_read_color(priv, "text_color", cm_color_text, priv->text_color);
    cm_read_color(priv, "temp_color", cm_color_temp, priv->temp_color);
    cm_read_color(priv, "system_color", cm_color_system, &priv->load_colors[0][0]);
    cm_read_color(priv, "user_color", cm_color_user, &priv->load_colors[1][0]);
    cm_read_color(priv, "nice_color", cm_color_nice, &priv->load_colors[2][0]);
    cm_read_color(priv, "io_color", cm_color_io, &priv->load_colors[3][0]);

    g_key_file_set_integer(kf, p->name, "socket_id", priv->socket_id);
    g_key_file_set_integer(kf, p->name, "update_ms", priv->update_ms);
    g_key_file_set_integer(kf, p->name, "columns", priv->columns);
    g_key_file_set_integer(kf, p->name, "rows", priv->rows);
    g_key_file_set_integer(kf, p->name, "window_width", priv->window_width);
    g_key_file_set_integer(kf, p->name, "window_height", priv->window_height);
    xs_core_plugin_conf_flush(p->name);
    if (!cm_socket_claims)
        cm_socket_claims = g_hash_table_new(g_direct_hash, g_direct_equal);
    gpointer old_owner = g_hash_table_lookup(cm_socket_claims,
                                              GINT_TO_POINTER(priv->socket_id));
    if (old_owner && old_owner != p) {
        p->host->log("cpu_monitor: physical package %d is already assigned to %s",
                     priv->socket_id, (const char *)old_owner);
        g_ptr_array_unref(priv->cores);
        g_ptr_array_unref(priv->hwmon_temps);
        g_free(priv->font);
        g_free(priv);
        p->priv = NULL;
        return -1;
    }
    g_hash_table_insert(cm_socket_claims,
                        GINT_TO_POINTER(priv->socket_id), p);
    cm_discover_topology(priv);
    cm_discover_temperatures(priv);
    if (priv->cores->len == 0) {
        p->host->log("cpu_monitor: physical package %d has no discoverable cores",
                     priv->socket_id);
    }
    cm_read_proc_stat(priv->previous);
    for (width = 0; width < CM_MAX_CPUS; width++)
        priv->stat_valid[width] = priv->previous[width].valid;

    width = priv->window_width;
    height = priv->window_height;
    cm_ensure_history(priv, MAX(width / MAX(priv->columns, 1) - 2, 1));
    p->win = xs_host_api()->make_window(p, x, y, width, height);
    if (!p->win) {
        p->host->log("cpu_monitor: failed to create window");
        g_ptr_array_unref(priv->cores);
        g_ptr_array_unref(priv->hwmon_temps);
        g_free(priv->font);
        g_free(priv);
        p->priv = NULL;
        return -1;
    }
    priv->cache = cm_render(priv, width, height);
    priv->cache_width = width;
    priv->cache_height = height;
    xs_host_api()->set_opacity(p, CLAMP(opacity, 0.1, 1.0));
    xs_host_api()->set_tick(p, priv->update_ms);
    return 0;
}

static void cm_tick_samples(PrivData *priv)
{
    if (cm_sample_cpu(priv))
        cm_push_history(priv);
    if (priv->cache) {
        cairo_surface_destroy(priv->cache);
        priv->cache = cm_render(priv, priv->cache_width, priv->cache_height);
    }
    if (priv->plugin->win) {
        xs_host_api()->invalidate(priv->plugin);
        gtk_widget_queue_draw(priv->plugin->win);
    }
}

static guint cm_tick(XsPlugin *p)
{
    PrivData *priv = p ? p->priv : NULL;

    if (!priv || !p->win)
        return 0;
    cm_tick_samples(priv);
    return priv->update_ms;
}

static void cm_draw(XsPlugin *p, cairo_t *cr, int w, int h)
{
    PrivData *priv = p ? p->priv : NULL;

    if (!priv || !priv->cache)
        return;
    if (priv->cache_width != w || priv->cache_height != h) {
        cm_ensure_history(priv, MAX((int)(w / MAX(priv->columns, 1)) - 2, 1));
        cairo_surface_destroy(priv->cache);
        priv->cache = cm_render(priv, w, h);
        priv->cache_width = w;
        priv->cache_height = h;
    }
    cairo_set_source_surface(cr, priv->cache, 0, 0);
    cairo_paint(cr);
}

static void cm_shutdown(XsPlugin *p)
{
    PrivData *priv = p ? p->priv : NULL;

    if (!priv)
        return;
    if (cm_socket_claims &&
        g_hash_table_lookup(cm_socket_claims,
                            GINT_TO_POINTER(priv->socket_id)) == p)
        g_hash_table_remove(cm_socket_claims,
                            GINT_TO_POINTER(priv->socket_id));
    if (priv->cache)
        cairo_surface_destroy(priv->cache);
    g_ptr_array_unref(priv->cores);
    g_ptr_array_unref(priv->hwmon_temps);
    g_free(priv->font);
    g_free(priv);
    p->priv = NULL;
}

static void cm_flush(PrivData *priv)
{
    xs_core_plugin_conf_flush(priv->plugin->name);
    if (priv->plugin->win)
        gtk_widget_queue_draw(priv->plugin->win);
}

static void cm_int_changed(GtkSpinButton *spin, gpointer data)
{
    XsPlugin *p = data;
    PrivData *priv = p ? p->priv : NULL;
    const char *key;
    int value;

    if (!priv)
        return;
    key = g_object_get_data(G_OBJECT(spin), "xs-key");
    value = (int)gtk_spin_button_get_value(spin);
    if (strcmp(key, "socket_id") == 0) {
        gint new_socket = MAX(value, 0);
        gpointer target_owner = g_hash_table_lookup(cm_socket_claims,
                                                     GINT_TO_POINTER(new_socket));
        if (target_owner && target_owner != p) {
            g_signal_handlers_block_by_func(spin, cm_int_changed, p);
            gtk_spin_button_set_value(spin, priv->socket_id);
            g_signal_handlers_unblock_by_func(spin, cm_int_changed, p);
            return;
        }
        g_hash_table_remove(cm_socket_claims,
                            GINT_TO_POINTER(priv->socket_id));
        priv->socket_id = new_socket;
        g_hash_table_insert(cm_socket_claims,
                            GINT_TO_POINTER(priv->socket_id), p);
        value = priv->socket_id;
        cm_discover_topology(priv);
        cm_discover_temperatures(priv);
        cm_ensure_history(priv, MAX(priv->window_width / priv->columns - 2, 1));
        memset(priv->previous, 0, sizeof(priv->previous));
        memset(priv->stat_valid, 0, sizeof(priv->stat_valid));
    } else if (strcmp(key, "update_ms") == 0) {
        priv->update_ms = CLAMP(value, 100, 60000);
        value = priv->update_ms;
    } else if (strcmp(key, "columns") == 0) {
        priv->columns = CLAMP(value, 1, 16);
        value = priv->columns;
        xs_host_api()->resize(p, priv->columns * CM_CELL_WIDTH,
                              priv->rows * CM_CELL_HEIGHT);
    } else if (strcmp(key, "rows") == 0) {
        priv->rows = CLAMP(value, 1, 16);
        value = priv->rows;
    } else if (strcmp(key, "window_width") == 0) {
        priv->window_width = CLAMP(value, 100, 1600);
        value = priv->window_width;
    } else if (strcmp(key, "window_height") == 0) {
        priv->window_height = CLAMP(value, 80, 1200);
        value = priv->window_height;
    }
    g_key_file_set_integer(priv->kf, p->name, key, value);
    if (priv->plugin->win) {
        xs_host_api()->resize(p, priv->window_width, priv->window_height);
        cm_ensure_history(priv, MAX(priv->window_width / priv->columns - 2, 1));
    }
    cm_flush(priv);
    if (priv->cache) {
        cairo_surface_destroy(priv->cache);
        priv->cache = cm_render(priv, priv->cache_width, priv->cache_height);
    }
}

static void cm_font_set(GtkFontButton *button, gpointer data)
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
    priv->font = g_strdup(value ? value : CM_DEFAULT_FONT);
    g_key_file_set_string(priv->kf, p->name, "font", priv->font);
    cm_flush(priv);
    if (priv->cache) {
        cairo_surface_destroy(priv->cache);
        priv->cache = cm_render(priv, priv->cache_width, priv->cache_height);
    }
}

static void cm_color_set(GtkColorButton *button, gpointer data)
{
    XsPlugin *p = data;
    PrivData *priv = p ? p->priv : NULL;
    const char *key;
    gdouble *target = NULL;
    guint segment = 0;
    GdkRGBA color;
    char *value;

    if (!priv)
        return;
    key = g_object_get_data(G_OBJECT(button), "xs-key");
    if (strcmp(key, "background_color") == 0)
        target = priv->background_color;
    else if (strcmp(key, "text_color") == 0)
        target = priv->text_color;
    else if (strcmp(key, "temp_color") == 0)
        target = priv->temp_color;
    else {
        static const char *names[] = {"system_color", "user_color",
                                      "nice_color", "io_color"};
        guint i;
        for (i = 0; i < G_N_ELEMENTS(names); i++)
            if (strcmp(key, names[i]) == 0) {
                target = &priv->load_colors[i][0];
                segment = i;
                break;
            }
    }
    if (!target)
        return;
    gtk_color_chooser_get_rgba(GTK_COLOR_CHOOSER(button), &color);
    target[0] = color.red;
    target[1] = color.green;
    target[2] = color.blue;
    target[3] = color.alpha;
    value = cm_color_string(&color);
    g_key_file_set_string(priv->kf, p->name, key, value);
    g_free(value);
    (void)segment;
    cm_flush(priv);
    if (priv->cache) {
        cairo_surface_destroy(priv->cache);
        priv->cache = cm_render(priv, priv->cache_width, priv->cache_height);
    }
}

static void cm_add_color(XsPlugin *p, GtkWidget *page, const char *label,
                         const char *key, const gdouble color[4])
{
    GtkWidget *w = xs_prop_add_color(GTK_BOX(page), label,
                                      "RGBA color used by the monitor", color[0],
                                      color[1], color[2], color[3]);

    g_object_set_data_full(G_OBJECT(w), "xs-key", g_strdup(key), g_free);
    g_signal_connect(w, "color-set", G_CALLBACK(cm_color_set), p);
}

static void cm_properties(XsPlugin *p, GtkNotebook *nb)
{
    PrivData *priv = p ? p->priv : NULL;
    GtkWidget *page;
    GtkWidget *w;
    static const char *int_keys[] = {"socket_id", "update_ms", "columns", "rows"};
    static const int defaults[] = {0, 1000, CM_COLUMNS_DEFAULT, CM_ROWS_DEFAULT};
    guint i;

    if (!priv)
        return;
    page = gtk_box_new(GTK_ORIENTATION_VERTICAL, 4);
    gtk_container_set_border_width(GTK_CONTAINER(page), 10);
    xs_prop_add_group_header(GTK_BOX(page),
                             "One physical package per instance; topology is read from sysfs.");
    for (i = 0; i < G_N_ELEMENTS(int_keys); i++) {
        int value = xs_host_api()->conf_int(priv->kf, p->name, int_keys[i],
                                            defaults[i]);
        double min = i == 0 ? 0.0 : 1.0;
        double max = i == 0 ? 255.0 : (i == 1 ? 60000.0 : 16.0);
        w = xs_prop_add_int(GTK_BOX(page),
                            i == 0 ? "Socket ID" : i == 1 ? "Update (ms)" :
                            i == 2 ? "Columns" : "Rows",
                            "CPU monitor layout setting", value, min, max, 1);
        g_object_set_data_full(G_OBJECT(w), "xs-key", g_strdup(int_keys[i]), g_free);
        g_signal_connect(w, "value-changed", G_CALLBACK(cm_int_changed), p);
    }
    w = xs_prop_add_int(GTK_BOX(page), "Window width",
                        "Overall applet width in pixels", priv->window_width,
                        100, 1600, 1);
    g_object_set_data_full(G_OBJECT(w), "xs-key", g_strdup("window_width"), g_free);
    g_signal_connect(w, "value-changed", G_CALLBACK(cm_int_changed), p);
    w = xs_prop_add_int(GTK_BOX(page), "Window height",
                        "Overall applet height in pixels", priv->window_height,
                        80, 1200, 1);
    g_object_set_data_full(G_OBJECT(w), "xs-key", g_strdup("window_height"), g_free);
    g_signal_connect(w, "value-changed", G_CALLBACK(cm_int_changed), p);
    w = xs_prop_add_font(GTK_BOX(page), "Font", "Frequency and temperature text",
                         priv->font);
    g_object_set_data_full(G_OBJECT(w), "xs-key", g_strdup("font"), g_free);
    g_signal_connect(w, "font-set", G_CALLBACK(cm_font_set), p);
    cm_add_color(p, page, "Background", "background_color", priv->background_color);
    cm_add_color(p, page, "Text", "text_color", priv->text_color);
    cm_add_color(p, page, "Temperature", "temp_color", priv->temp_color);
    cm_add_color(p, page, "System load", "system_color", &priv->load_colors[0][0]);
    cm_add_color(p, page, "User load", "user_color", &priv->load_colors[1][0]);
    cm_add_color(p, page, "Nice load", "nice_color", &priv->load_colors[2][0]);
    cm_add_color(p, page, "I/O wait", "io_color", &priv->load_colors[3][0]);
    gtk_notebook_append_page(nb, page, gtk_label_new("CPU Monitor"));
    gtk_widget_show_all(page);
}

static const XsPluginOps cm_ops = {
    .init = cm_init,
    .draw = cm_draw,
    .tick = cm_tick,
    .button = NULL,
    .motion = NULL,
    .shutdown = cm_shutdown,
    .menu = NULL,
    .menu_cmd = NULL,
    .properties = cm_properties,
    .fill_themes = NULL,
    .scroll = NULL,
    .enter = NULL,
    .leave = NULL,
    .guest_list_changed = NULL,
};

static XsPluginDesc cm_desc = {
    "cpu_monitor",
    XS_API_VERSION,
    &cm_ops,
    "Topology-aware per-socket CPU load, frequency and temperature monitor",
    "xscreenlets",
    "1.0"
};

XS_PLUGIN_EXPORT(&cm_desc)
