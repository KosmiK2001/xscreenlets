/* process_list.c — C/GTK3 process list modelled after .conkyrc_proc.
 * Numeric /proc I/O happens only in tick(); draw() paints the cached surface. */
#include <gtk/gtk.h>
#include <glib.h>
#include <gmodule.h>
#include <pango/pangocairo.h>
#include <cairo.h>
#include <errno.h>
#include <inttypes.h>
#include <math.h>
#include <string.h>
#include <sys/types.h>
#include <time.h>
#include <unistd.h>
#include "xs_api.h"
#include "common.h"

#define PL_TITLE_FONT_DEFAULT "Ubuntu 8"
#define PL_ROW_FONT_DEFAULT   "Verdana 7"
#define PL_WIDTH_DEFAULT      320
#define PL_HEIGHT_DEFAULT     164
#define PL_UPDATE_DEFAULT     1000
#define PL_ROWS_DEFAULT       8
#define PL_MAX_PROCESSES      16384
#define PL_NAME_MAX           96
#define PL_PADDING             4.0
#define PL_TEXT_PADDING        2.0
#define PL_COL_GAP             4.0
#define PL_PID_WIDTH           58.0
#define PL_CPU_WIDTH           48.0  /* fits 3200.0% on this 32-thread host */
#define PL_MEM_WIDTH           48.0
#define PL_IO_WIDTH            42.0
#define PL_TITLE_Y             3.0
#define PL_HEADER_Y           21.0
#define PL_ROWS_Y             36.0

typedef struct {
    gint pid;
    guint64 cpu_ticks;
    guint64 start_time;
    guint64 disk_read_bytes;
    guint64 disk_write_bytes;
    gint64 rss_bytes;
    gint cpu_tenths;                 /* 1000 = 100.0 percent */
    gint64 io_bytes_per_sec;
    gboolean running;
    char name[PL_NAME_MAX];
} PlProcess;

typedef enum {
    PL_SORT_NAME,
    PL_SORT_PID,
    PL_SORT_CPU,
    PL_SORT_MEM,
    PL_SORT_IO
} PlSort;

typedef struct {
    guint default_sort;
    gboolean default_descending;
    guint sort;
    gboolean descending;
    int pressed_column;
    int last_click_column;
    guint repeat_clicks;
} PlSortState;

typedef struct {
    XsPlugin *plugin;
    PlSortState sorting;
    GKeyFile *kf;
    guint update_ms;
    guint row_count;
    guint cpu_basis;                  /* 0 = per core, 1 = Conky total */
    int window_width;
    int window_height;
    gdouble background[4];
    gdouble accent[4];
    gdouble title[4];
    gdouble header[4];
    gdouble text[4];
    char *title_font;
    char *row_font;
    GHashTable *previous;            /* pid -> PlProcess baseline */
    GPtrArray *snapshot;             /* all sampled processes, current sort */
    GPtrArray *rows;                 /* first row_count entries of snapshot */
    cairo_surface_t *cache;
    int cache_width;
    int cache_height;
    guint process_count;
    guint running_count;
    gint64 last_sample_us;
} PrivData;

static const gdouble pl_background_default[4] = {
    25.0 / 255.0, 25.0 / 255.0, 25.0 / 255.0, 0.75
};
static const gdouble pl_accent_default[4] = {
    83.0 / 255.0, 130.0 / 255.0, 186.0 / 255.0, 1.0
};
static const gdouble pl_title_default[4] = {0.75, 0.75, 0.75, 1.0};
static const gdouble pl_header_default[4] = {1.0, 0.0, 0.0, 1.0};
static const gdouble pl_text_default[4] = {1.0, 1.0, 1.0, 1.0};

static gint64 pl_now_us(void)
{
    return g_get_monotonic_time();
}

static void pl_process_free(gpointer data)
{
    g_free(data);
}

static gboolean pl_number(const char *text, guint64 *value)
{
    gchar *end = NULL;
    guint64 result;

    if (!text || !*text)
        return FALSE;
    errno = 0;
    result = g_ascii_strtoull(text, &end, 10);
    if (errno == ERANGE || end == text || *end != '\0')
        return FALSE;
    *value = result;
    return TRUE;
}

static gboolean pl_number_prefix(const char *text, guint64 *value)
{
    gchar *end = NULL;
    guint64 result;

    if (!text || !*text)
        return FALSE;
    errno = 0;
    result = g_ascii_strtoull(text, &end, 10);
    if (errno == ERANGE || end == text ||
        (*end != '\0' && !g_ascii_isspace(*end) && *end != 'k' && *end != 'K'))
        return FALSE;
    *value = result;
    return TRUE;
}

static gboolean pl_parse_color(const char *text, gdouble out[4])
{
    const char *p = text;
    guint i;

    if (!text)
        return FALSE;
    for (i = 0; i < 4; i++) {
        gchar *end = NULL;
        gdouble value;

        errno = 0;
        value = g_ascii_strtod(p, &end);
        if (end == p || errno == ERANGE || !isfinite(value) ||
            value < 0.0 || value > 1.0)
            return FALSE;
        out[i] = value;
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

static char *pl_color_string(const gdouble color[4])
{
    gchar values[4][32];
    static const char *format[] = {"%.9g", "%.9g", "%.9g", "%.9g"};

    for (guint i = 0; i < 4; i++)
        g_ascii_formatd(values[i], sizeof(values[i]), format[i], color[i]);
    return g_strdup_printf("%s,%s,%s,%s", values[0], values[1],
                           values[2], values[3]);
}

static void pl_read_color(PrivData *priv, const char *key,
                          const gdouble fallback[4], gdouble out[4])
{
    char *value = xs_host_api()->conf_str(priv->kf, priv->plugin->name,
                                          key, NULL);

    memcpy(out, fallback, 4 * sizeof(*out));
    if (value) {
        if (!pl_parse_color(value, out))
            priv->plugin->host->log("process_list: invalid %s; using default",
                                    key);
        g_free(value);
    }
}

static gboolean pl_parse_status(const char *text, gint64 *rss_kb,
                                gboolean *running)
{
    gchar **lines = g_strsplit(text, "\n", -1);
    gboolean found_rss = FALSE;
    guint i;

    if (running)
        *running = FALSE;
    for (i = 0; lines[i]; i++) {
        if (g_str_has_prefix(lines[i], "State:") &&
            (strchr(lines[i] + 6, 'R') || strchr(lines[i] + 6, 'r'))) {
            if (running)
                *running = TRUE;
        } else if (g_str_has_prefix(lines[i], "VmRSS:")) {
            guint64 kb;
            if (pl_number_prefix(g_strstrip(lines[i] + 6), &kb) &&
                kb <= G_MAXINT64) {
                *rss_kb = (gint64)kb;
                found_rss = TRUE;
            }
        }
    }
    g_strfreev(lines);
    return found_rss;
}

static gboolean pl_read_io(const gint pid, guint64 *read_bytes,
                           guint64 *write_bytes)
{
    g_autofree char *path = g_strdup_printf("/proc/%d/io", pid);
    g_autofree char *text = NULL;
    g_auto(GStrv) lines = NULL;

    if (!g_file_get_contents(path, &text, NULL, NULL))
        return FALSE;
    lines = g_strsplit(text, "\n", -1);
    for (guint i = 0; lines[i]; i++) {
        gchar *end = NULL;
        guint64 value;

        if (g_str_has_prefix(lines[i], "read_bytes:")) {
            errno = 0;
            value = g_ascii_strtoull(lines[i] + 11, &end, 10);
            if (end != lines[i] + 11 && errno != ERANGE)
                *read_bytes = value;
        } else if (g_str_has_prefix(lines[i], "write_bytes:")) {
            errno = 0;
            value = g_ascii_strtoull(lines[i] + 12, &end, 10);
            if (end != lines[i] + 12 && errno != ERANGE)
                *write_bytes = value;
        }
    }
    return *read_bytes != G_MAXUINT64 && *write_bytes != G_MAXUINT64;
}

static PlProcess *pl_read_pid(const char *pid_text, gboolean *running)
{
    g_autofree char *stat_path = NULL;
    g_autofree char *status_path = NULL;
    g_autofree char *stat_data = NULL;
    g_autofree char *status_data = NULL;
    g_autofree char *name = NULL;
    const char *open_paren;
    const char *close_paren;
    gchar **fields = NULL;
    guint64 pid;
    guint64 utime;
    guint64 stime;
    guint64 start_time;
    gsize name_length;
    PlProcess *proc;
    gint64 rss_kb = 0;
    gboolean is_running = FALSE;

    if (!g_ascii_isdigit(pid_text[0]) || !pl_number(pid_text, &pid) ||
        pid > G_MAXINT)
        return NULL;
    stat_path = g_strdup_printf("/proc/%d/stat", (gint)pid);
    status_path = g_strdup_printf("/proc/%d/status", (gint)pid);
    if (!g_file_get_contents(stat_path, &stat_data, NULL, NULL) ||
        !g_file_get_contents(status_path, &status_data, NULL, NULL))
        return NULL;

    open_paren = strchr(stat_data, '(');
    close_paren = open_paren ? strrchr(open_paren, ')') : NULL;
    if (!open_paren || !close_paren)
        return NULL;
    name_length = (gsize)(close_paren - open_paren - 1);
    name = g_strndup(open_paren + 1, name_length);
    fields = g_strsplit_set(close_paren + 1, " \t", -1);
    if (!fields[0] || !fields[1] || !fields[9] || !fields[10] ||
        !fields[11] || !fields[12] || !fields[18] || !fields[19] ||
        !pl_number(fields[11], &utime) || !pl_number(fields[12], &stime) ||
        !pl_number(fields[19], &start_time)) {
        g_strfreev(fields);
        return NULL;
    }
    g_strfreev(fields);
    pl_parse_status(status_data, &rss_kb, &is_running);

    proc = g_new0(PlProcess, 1);
    proc->pid = (gint)pid;
    proc->cpu_ticks = utime + stime;
    proc->start_time = start_time;
    proc->disk_read_bytes = G_MAXUINT64;
    proc->disk_write_bytes = G_MAXUINT64;
    (void)pl_read_io(proc->pid, &proc->disk_read_bytes,
                     &proc->disk_write_bytes);
    proc->rss_bytes = rss_kb > 0 &&
                      rss_kb <= G_MAXINT64 / 1024 ?
                      rss_kb * 1024 : 0;
    proc->running = is_running;
    g_strlcpy(proc->name, name, sizeof(proc->name));
    *running = is_running;
    return proc;
}

static const char *pl_sort_name(guint sort)
{
    static const char *const names[] = { "name", "pid", "cpu", "mem", "io" };

    return sort < G_N_ELEMENTS(names) ? names[sort] : "cpu";
}

static guint pl_sort_value(const char *name)
{
    if (g_strcmp0(name, "name") == 0)
        return PL_SORT_NAME;
    if (g_strcmp0(name, "pid") == 0)
        return PL_SORT_PID;
    if (g_strcmp0(name, "mem") == 0)
        return PL_SORT_MEM;
    if (g_strcmp0(name, "io") == 0)
        return PL_SORT_IO;
    return PL_SORT_CPU;
}

static gint pl_compare(gconstpointer a, gconstpointer b, gpointer data)
{
    const PlProcess *pa = *(PlProcess * const *)a;
    const PlProcess *pb = *(PlProcess * const *)b;
    const PlSortState *sorting = data;
    gint result = 0;

    switch ((PlSort)sorting->sort) {
    case PL_SORT_NAME:
        result = g_strcmp0(pa->name, pb->name);
        break;
    case PL_SORT_PID:
        result = pa->pid < pb->pid ? -1 : pa->pid > pb->pid ? 1 : 0;
        break;
    case PL_SORT_MEM:
        result = pa->rss_bytes < pb->rss_bytes ? -1 :
                 pa->rss_bytes > pb->rss_bytes ? 1 : 0;
        break;
    case PL_SORT_IO:
        result = pa->io_bytes_per_sec < pb->io_bytes_per_sec ? -1 :
                 pa->io_bytes_per_sec > pb->io_bytes_per_sec ? 1 : 0;
        break;
    case PL_SORT_CPU:
    default:
        result = pa->cpu_tenths < pb->cpu_tenths ? -1 :
                 pa->cpu_tenths > pb->cpu_tenths ? 1 : 0;
        break;
    }
    if (result)
        return sorting->descending ? -result : result;
    return pa->pid < pb->pid ? -1 : pa->pid > pb->pid ? 1 : 0;
}

static gint64 pl_online_cpu_count(void)
{
    g_autofree char *online = NULL;

    if (g_file_get_contents("/sys/devices/system/cpu/online", &online,
                            NULL, NULL)) {
        g_auto(GStrv) parts = g_strsplit(online, ",", -1);
        guint64 count = 0;

        for (guint i = 0; parts[i]; i++) {
            guint64 first;
            guint64 last;

            if (sscanf(parts[i], "%" G_GUINT64_FORMAT "-%" G_GUINT64_FORMAT,
                       &first, &last) == 2) {
                if (last >= first && last < 100000)
                    count += last - first + 1;
            } else if (pl_number(g_strstrip(parts[i]), &first) &&
                       first < 100000) {
                count++;
            }
        }
        if (count)
            return (gint64)count;
    }
    return (gint64)MAX((long)sysconf(_SC_NPROCESSORS_ONLN), 1L);
}

static void pl_rebuild_rows(PrivData *priv)
{
    GPtrArray *rows = g_ptr_array_new_with_free_func(pl_process_free);
    guint i;

    if (!priv->snapshot) {
        priv->rows = rows;
        return;
    }
    g_ptr_array_sort_with_data(priv->snapshot, pl_compare, &priv->sorting);
    for (i = 0; i < MIN(priv->snapshot->len, priv->row_count); i++) {
        const PlProcess *proc = g_ptr_array_index(priv->snapshot, i);
        g_ptr_array_add(rows, g_memdup2(proc, sizeof(*proc)));
    }
    g_ptr_array_unref(priv->rows);
    priv->rows = rows;
}

static void pl_sample(PrivData *priv)
{
    GDir *directory;
    const gchar *entry;
    GPtrArray *current = g_ptr_array_new_with_free_func(pl_process_free);
    guint i;
    gint64 now = pl_now_us();
    gint64 elapsed_us = now - priv->last_sample_us;
    long ticks_per_second = sysconf(_SC_CLK_TCK);

    if (ticks_per_second <= 0)
        ticks_per_second = 100;
    if (elapsed_us <= 0)
        elapsed_us = (gint64)priv->update_ms * 1000;
    priv->last_sample_us = now;
    priv->process_count = 0;
    priv->running_count = 0;

    directory = g_dir_open("/proc", 0, NULL);
    if (directory) {
        while ((entry = g_dir_read_name(directory)) != NULL &&
               current->len < PL_MAX_PROCESSES) {
            gboolean running = FALSE;
            PlProcess *proc;

            if (!g_ascii_isdigit(entry[0]))
                continue;
            proc = pl_read_pid(entry, &running);
            if (!proc)
                continue;
            priv->process_count++;
            if (running)
                priv->running_count++;
            g_ptr_array_add(current, proc);
        }
        g_dir_close(directory);
    }

    for (i = 0; i < current->len; i++) {
        PlProcess *proc = g_ptr_array_index(current, i);
        PlProcess *old = g_hash_table_lookup(priv->previous,
                                             GINT_TO_POINTER(proc->pid));

        if (old && old->start_time == proc->start_time) {
            if (proc->cpu_ticks >= old->cpu_ticks) {
                guint64 delta = proc->cpu_ticks - old->cpu_ticks;
                /* cpu_tenths is 1000 = 100.0% of one logical thread.
                 * CPU ticks are CLK_TCK units; elapsed_us is microseconds. */
                gdouble tenths = 100.0 * (gdouble)delta * 10000000.0 /
                                ((gdouble)ticks_per_second *
                                 (gdouble)elapsed_us);
                if (priv->cpu_basis == 1)
                    tenths /= (gdouble)pl_online_cpu_count();
                proc->cpu_tenths = (gint)CLAMP((gint64)(tenths + 0.5),
                                               (gint64)0, (gint64)G_MAXINT);
            }
            if (proc->disk_read_bytes != G_MAXUINT64 &&
                proc->disk_write_bytes != G_MAXUINT64 &&
                old->disk_read_bytes != G_MAXUINT64 &&
                old->disk_write_bytes != G_MAXUINT64 &&
                proc->disk_read_bytes >= old->disk_read_bytes &&
                proc->disk_write_bytes >= old->disk_write_bytes) {
                guint64 delta = (proc->disk_read_bytes -
                                 old->disk_read_bytes) +
                                (proc->disk_write_bytes -
                                 old->disk_write_bytes);
                guint64 scaled = delta * G_GUINT64_CONSTANT(1000000);
                proc->io_bytes_per_sec = (gint64)MIN(
                    scaled / (guint64)elapsed_us, (guint64)G_MAXINT64);
            }
        }
    }

    g_hash_table_remove_all(priv->previous);
    for (i = 0; i < current->len; i++) {
        const PlProcess *proc = g_ptr_array_index(current, i);
        PlProcess *baseline = g_memdup2(proc, sizeof(*baseline));
        g_hash_table_insert(priv->previous, GINT_TO_POINTER(proc->pid),
                            baseline);
    }
    g_ptr_array_sort_with_data(current, pl_compare, &priv->sorting);
    g_ptr_array_unref(priv->snapshot);
    priv->snapshot = g_ptr_array_ref(current);
    pl_rebuild_rows(priv);
    g_ptr_array_unref(current);
}

typedef struct {
    double right[5];
    double width[5];
} PlColumns;

static PlColumns pl_columns(int width)
{
    PlColumns columns = { { 0 }, { 0 } };
    double right = width - PL_PADDING;

    columns.right[4] = right;
    columns.width[4] = PL_IO_WIDTH;
    columns.right[3] = columns.right[4] - (PL_IO_WIDTH + PL_COL_GAP);
    columns.width[3] = PL_MEM_WIDTH;
    columns.right[2] = columns.right[3] - (PL_MEM_WIDTH + PL_COL_GAP);
    columns.width[2] = PL_CPU_WIDTH;
    columns.right[1] = columns.right[2] - (PL_CPU_WIDTH + PL_COL_GAP);
    columns.width[1] = PL_PID_WIDTH;
    columns.right[0] = columns.right[1] - (PL_PID_WIDTH + PL_COL_GAP);
    columns.width[0] = MAX(columns.right[0] - PL_PADDING, 1.0);
    return columns;
}

static const char *const pl_column_names[] = { "NAME", "PID", "CPU", "MEM", "I/O" };
static const PlSort pl_column_sorts[] = {
    PL_SORT_NAME, PL_SORT_PID, PL_SORT_CPU, PL_SORT_MEM, PL_SORT_IO
};

static int pl_column_at(const PlColumns *columns, double x, double y)
{
    guint i;

    if (y < PL_HEADER_Y - 5.0 || y > PL_HEADER_Y + 16.0)
        return -1;
    for (i = 0; i < G_N_ELEMENTS(pl_column_names); i++) {
        if (x >= columns->right[i] - columns->width[i] &&
            x <= columns->right[i])
            return (int)i;
    }
    return -1;
}

static PangoFontDescription *pl_font(const char *name)
{
    return pango_font_description_from_string(name);
}

static double pl_text_width(PangoLayout *layout)
{
    PangoRectangle logical;
    PangoRectangle ink;

    pango_layout_get_pixel_extents(layout, &ink, &logical);
    return ink.width;
}

static double pl_text_height(PangoLayout *layout)
{
    gint width;
    gint height;

    pango_layout_get_pixel_size(layout, &width, &height);
    return height;
}

static void pl_show_column(PangoLayout *layout, cairo_t *cr,
                           const char *text, double right, double y,
                           double column_width, gboolean align_left)
{
    double text_width = MAX(column_width - 2.0 * PL_TEXT_PADDING, 1.0);
    double cairo_x;

    pango_layout_set_text(layout, text, -1);
    pango_layout_set_width(layout, (int)(text_width * PANGO_SCALE));
    pango_layout_set_ellipsize(layout, PANGO_ELLIPSIZE_END);
    pango_layout_set_alignment(layout, align_left ? PANGO_ALIGN_LEFT :
                                            PANGO_ALIGN_RIGHT);
    if (align_left) {
        cairo_x = right - column_width + PL_TEXT_PADDING;
    } else {
        PangoRectangle ink;
        PangoRectangle logical;
        pango_layout_get_pixel_extents(layout, &ink, &logical);
        cairo_x = right - PL_TEXT_PADDING - ink.x - ink.width;
    }
    cairo_move_to(cr, cairo_x, y);
    pango_cairo_show_layout(cr, layout);
}

static void pl_dotted_line(cairo_t *cr, const gdouble color[4],
                           double x1, double y, double x2)
{
    cairo_save(cr);
    cairo_set_source_rgba(cr, color[0], color[1], color[2],
                          color[3] * 0.65);
    cairo_set_line_width(cr, 1.0);
    for (double x = x1; x <= x2; x += 3.0) {
        cairo_move_to(cr, x, y + 0.5);
        cairo_line_to(cr, MIN(x + 1.0, x2), y + 0.5);
    }
    cairo_stroke(cr);
    cairo_restore(cr);
}

static void pl_format_bytes(gchar *buffer, gsize size, gint64 bytes)
{
    static const char *const units[] = { "B", "K", "M", "G", "T", "P", "E" };
    guint64 value = bytes > 0 ? (guint64)bytes : 0;
    gdouble scaled = (gdouble)value;
    guint unit = 0;

    while (scaled >= 1024.0 && unit + 1 < G_N_ELEMENTS(units)) {
        scaled /= 1024.0;
        unit++;
    }
    if (unit == 0)
        g_snprintf(buffer, size, "%" G_GUINT64_FORMAT "%s",
                   value, units[unit]);
    else
        g_snprintf(buffer, size, "%.1f%s", scaled, units[unit]);
}

static void pl_format_io(gchar *buffer, gsize size, gint64 bytes_per_sec)
{
    guint64 value = bytes_per_sec > 0 ? (guint64)bytes_per_sec : 0;

    if (value < 10ULL * 1024 * 1024)
        g_snprintf(buffer, size, "%" G_GUINT64_FORMAT "K", value / 1024);
    else
        g_snprintf(buffer, size, "%.1fM", value / (1024.0 * 1024.0));
}

static cairo_surface_t *pl_render(PrivData *priv, int width, int height)
{
    cairo_surface_t *surface = cairo_image_surface_create(CAIRO_FORMAT_ARGB32,
                                                          MAX(width, 1),
                                                          MAX(height, 1));
    cairo_t *cr = cairo_create(surface);
    PangoLayout *layout = pango_cairo_create_layout(cr);
    PangoFontDescription *title_font = pl_font(priv->title_font);
    PangoFontDescription *row_font = pl_font(priv->row_font);
    PlColumns columns = pl_columns(width);
    PlSortState *sorting = &priv->sorting;
    double right = columns.right[4];
    double name_right = columns.right[0];
    double name_width = columns.width[0];
    double available_rows_height = MAX(height - PL_ROWS_Y - PL_PADDING, 1.0);
    double row_height = MIN(16.0, available_rows_height /
                            MAX(priv->row_count, 1U));
    guint i;

    cairo_set_operator(cr, CAIRO_OPERATOR_SOURCE);
    cairo_set_source_rgba(cr, priv->background[0], priv->background[1],
                          priv->background[2], priv->background[3]);
    cairo_rectangle(cr, 0, 0, width, height);
    cairo_fill(cr);
    cairo_set_operator(cr, CAIRO_OPERATOR_OVER);

    pango_layout_set_font_description(layout, title_font);
    pango_layout_set_text(layout, "Processes: ", -1);
    cairo_set_source_rgba(cr, priv->title[0], priv->title[1],
                          priv->title[2], priv->title[3]);
    cairo_move_to(cr, PL_PADDING, PL_TITLE_Y);
    pango_cairo_show_layout(cr, layout);
    {
        char *count = g_strdup_printf("%u", priv->process_count);
        double x = PL_PADDING + pl_text_width(layout);
        pango_layout_set_text(layout, count, -1);
        cairo_set_source_rgba(cr, priv->accent[0], priv->accent[1],
                              priv->accent[2], priv->accent[3]);
        cairo_move_to(cr, x, PL_TITLE_Y);
        pango_cairo_show_layout(cr, layout);
        g_free(count);
    }
    {
        char *running = g_strdup_printf("     Running: %u",
                                        priv->running_count);
        pango_layout_set_text(layout, running, -1);
        cairo_set_source_rgba(cr, priv->accent[0], priv->accent[1],
                              priv->accent[2], priv->accent[3]);
        cairo_move_to(cr, right - pl_text_width(layout), PL_TITLE_Y);
        pango_cairo_show_layout(cr, layout);
        g_free(running);
    }

    pl_dotted_line(cr, priv->title, PL_PADDING, PL_HEADER_Y - 4.0, right);
    pango_layout_set_font_description(layout, row_font);
    cairo_set_source_rgba(cr, priv->header[0], priv->header[1],
                          priv->header[2], priv->header[3]);
    for (i = 0; i < G_N_ELEMENTS(pl_column_names); i++) {
        gboolean active = sorting->sort == pl_column_sorts[i];
        gboolean left = i == PL_SORT_NAME;

        pl_show_column(layout, cr, pl_column_names[i], columns.right[i],
                       PL_HEADER_Y, columns.width[i], left);
        if (active) {
            PangoRectangle logical;
            PangoRectangle ink;
            double text_x;
            double text_width;
            double text_h;

            pango_layout_get_pixel_extents(layout, &ink, &logical);
            text_x = left ?
                columns.right[i] - columns.width[i] + PL_TEXT_PADDING + ink.x :
                columns.right[i] - PL_TEXT_PADDING - ink.width;
            text_width = ink.width;
            text_h = pl_text_height(layout);
            cairo_save(cr);
            cairo_set_source_rgba(cr, 0.20, 0.85, 0.35, 1.0);
            cairo_set_line_width(cr, 1.0);
            cairo_rectangle(cr, text_x - 2.0 + 0.5,
                            floor(PL_HEADER_Y) - 2.0 + 0.5,
                            text_width + 4.0, text_h + 4.0);
            cairo_stroke(cr);
            cairo_restore(cr);
        }
    }

    cairo_set_source_rgba(cr, priv->text[0], priv->text[1],
                          priv->text[2], priv->text[3]);
    for (i = 0; i < priv->rows->len; i++) {
        const PlProcess *proc = g_ptr_array_index(priv->rows, i);
        char pid_text[24];
        char cpu_text[24];
        char mem_text[24];
        char io_text[24];
        double y = PL_ROWS_Y + i * row_height;
        GRegex *regex = g_regex_new("[[:cntrl:]]", 0, 0, NULL);
        gchar *clean_name = regex ?
            g_regex_replace(regex, proc->name, -1, 0, "_", 0, NULL) :
            g_strdup(proc->name);

        pl_show_column(layout, cr, clean_name, name_right, y, name_width, TRUE);
        g_snprintf(pid_text, sizeof(pid_text), "%d", proc->pid);
        pl_show_column(layout, cr, pid_text, columns.right[1], y,
                       PL_PID_WIDTH, FALSE);
        g_snprintf(cpu_text, sizeof(cpu_text), "%.1f%%",
                   proc->cpu_tenths / 10.0);
        pl_show_column(layout, cr, cpu_text, columns.right[2], y,
                       PL_CPU_WIDTH, FALSE);
        pl_format_bytes(mem_text, sizeof(mem_text), proc->rss_bytes);
        pl_show_column(layout, cr, mem_text, columns.right[3], y,
                       PL_MEM_WIDTH, FALSE);
        pl_format_io(io_text, sizeof(io_text), proc->io_bytes_per_sec);
        pl_show_column(layout, cr, io_text, columns.right[4], y,
                       PL_IO_WIDTH, FALSE);
        g_free(clean_name);
        if (regex)
            g_regex_unref(regex);
    }
    pl_dotted_line(cr, priv->title, PL_PADDING,
                   height - PL_PADDING - 1.0, right);

    pango_layout_set_font_description(layout, title_font);
    pango_layout_set_text(layout, " ", -1);
    (void)pl_text_height(layout);
    g_object_unref(layout);
    g_free(title_font);
    g_free(row_font);
    cairo_destroy(cr);
    cairo_surface_mark_dirty(surface);
    return surface;
}

static void pl_rerender(PrivData *priv)
{
    if (priv->cache) {
        cairo_surface_destroy(priv->cache);
        priv->cache = pl_render(priv, priv->cache_width,
                                priv->cache_height);
    }
    if (priv->plugin->win) {
        xs_host_api()->invalidate(priv->plugin);
        gtk_widget_queue_draw(priv->plugin->win);
    }
}

static void pl_flush(PrivData *priv)
{
    xs_core_plugin_conf_flush(priv->plugin->name);
    pl_rerender(priv);
}

static gdouble *pl_color_target(PrivData *priv, const char *key)
{
    if (strcmp(key, "background_color") == 0)
        return priv->background;
    if (strcmp(key, "accent_color") == 0)
        return priv->accent;
    if (strcmp(key, "title_color") == 0)
        return priv->title;
    if (strcmp(key, "header_color") == 0)
        return priv->header;
    return priv->text;
}

static void pl_color_set(GtkColorButton *button, gpointer data)
{
    XsPlugin *plugin = data;
    PrivData *priv = plugin ? plugin->priv : NULL;
    const char *key;
    GdkRGBA color;
    gdouble *target;
    g_autofree char *value = NULL;

    if (!priv)
        return;
    key = g_object_get_data(G_OBJECT(button), "xs-key");
    target = pl_color_target(priv, key);
    gtk_color_chooser_get_rgba(GTK_COLOR_CHOOSER(button), &color);
    target[0] = color.red;
    target[1] = color.green;
    target[2] = color.blue;
    target[3] = color.alpha;
    value = pl_color_string(target);
    g_key_file_set_string(priv->kf, plugin->name, key, value);
    pl_flush(priv);
}

static void pl_int_changed(GtkSpinButton *spin, gpointer data)
{
    XsPlugin *plugin = data;
    PrivData *priv = plugin ? plugin->priv : NULL;
    const char *key;
    gint value;

    if (!priv)
        return;
    key = g_object_get_data(G_OBJECT(spin), "xs-key");
    value = gtk_spin_button_get_value_as_int(spin);
    if (strcmp(key, "update_ms") == 0) {
        priv->update_ms = CLAMP(value, 100, 60000);
        value = priv->update_ms;
        xs_host_api()->set_tick(plugin, priv->update_ms);
    } else if (strcmp(key, "row_count") == 0) {
        priv->row_count = CLAMP(value, 1, 32);
        value = priv->row_count;
        pl_rebuild_rows(priv);
    } else if (strcmp(key, "window_width") == 0) {
        priv->window_width = CLAMP(value, 320, 1200);
        value = priv->window_width;
    } else {
        priv->window_height = CLAMP(value, 100, 1000);
        value = priv->window_height;
    }
    g_key_file_set_integer(priv->kf, plugin->name, key, value);
    if (plugin->win)
        xs_host_api()->resize(plugin, priv->window_width,
                             priv->window_height);
    pl_flush(priv);
}

static void pl_font_set(GtkFontButton *button, gpointer data)
{
    XsPlugin *plugin = data;
    PrivData *priv = plugin ? plugin->priv : NULL;
    const char *key;
    const char *font_name;
    g_autofree char *value = NULL;

    if (!priv)
        return;
    key = g_object_get_data(G_OBJECT(button), "xs-key");
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wdeprecated-declarations"
    font_name = gtk_font_button_get_font_name(button);
#pragma GCC diagnostic pop
    if (!font_name || !*font_name)
        return;
    value = g_strdup(font_name);
    if (strcmp(key, "title_font") == 0)
        g_free(priv->title_font);
    else
        g_free(priv->row_font);
    if (strcmp(key, "title_font") == 0)
        priv->title_font = g_steal_pointer(&value);
    else
        priv->row_font = g_steal_pointer(&value);
    g_key_file_set_string(priv->kf, plugin->name, key,
                          strcmp(key, "title_font") == 0 ?
                          priv->title_font : priv->row_font);
    pl_flush(priv);
}

static void pl_add_color(XsPlugin *plugin, GtkWidget *page, const char *label,
                         const char *key, const gdouble color[4])
{
    GtkWidget *widget = xs_prop_add_color(GTK_BOX(page), label,
                                          "RGBA color; alpha is applied while drawing",
                                          color[0], color[1], color[2], color[3]);

    g_object_set_data_full(G_OBJECT(widget), "xs-key", g_strdup(key), g_free);
    g_signal_connect(widget, "color-set", G_CALLBACK(pl_color_set), plugin);
}

static void pl_add_font(XsPlugin *plugin, GtkWidget *page, const char *label,
                        const char *key, const char *value)
{
    GtkWidget *widget = xs_prop_add_font(GTK_BOX(page), label,
                                         "Font used by the process list", value);

    g_object_set_data_full(G_OBJECT(widget), "xs-key", g_strdup(key), g_free);
    g_signal_connect(widget, "font-set", G_CALLBACK(pl_font_set), plugin);
}

static void pl_default_sort_changed(GtkComboBox *combo, gpointer data)
{
    XsPlugin *plugin = data;
    PrivData *priv = plugin ? plugin->priv : NULL;
    int active = gtk_combo_box_get_active(combo);

    if (!priv || active < 0)
        return;
    priv->sorting.default_sort = (guint)active;
    priv->sorting.sort = active;
    priv->sorting.last_click_column = -1;
    priv->sorting.repeat_clicks = 0;
    priv->sorting.descending = priv->sorting.default_descending;
    g_key_file_set_string(priv->kf, plugin->name, "default_sort",
                          pl_sort_name(active));
    pl_rebuild_rows(priv);
    pl_flush(priv);
}

static void pl_default_direction_changed(GtkComboBox *combo, gpointer data)
{
    XsPlugin *plugin = data;
    PrivData *priv = plugin ? plugin->priv : NULL;
    gboolean descending = gtk_combo_box_get_active(combo) == 0;

    if (!priv)
        return;
    priv->sorting.default_descending = descending;
    priv->sorting.descending = descending;
    g_key_file_set_string(priv->kf, plugin->name, "default_direction",
                          descending ? "descending" : "ascending");
    pl_rebuild_rows(priv);
    pl_flush(priv);
}

static void pl_cpu_basis_changed(GtkComboBox *combo, gpointer data)
{
    XsPlugin *plugin = data;
    PrivData *priv = plugin ? plugin->priv : NULL;
    const char *basis = "per-core";

    if (!priv)
        return;
    priv->cpu_basis = gtk_combo_box_get_active(combo) == 1;
    if (priv->cpu_basis)
        basis = "all-cores";
    g_key_file_set_string(priv->kf, plugin->name, "cpu_basis", basis);
    pl_flush(priv);
}

static void pl_add_int(XsPlugin *plugin, GtkWidget *page, const char *label,
                       const char *key, gint value, gint min, gint max)
{
    GtkWidget *widget = xs_prop_add_int(GTK_BOX(page), label,
                                        "Process list layout setting",
                                        value, min, max, 1);

    g_object_set_data_full(G_OBJECT(widget), "xs-key", g_strdup(key), g_free);
    g_signal_connect(widget, "value-changed",
                     G_CALLBACK(pl_int_changed), plugin);
}

static void pl_properties(XsPlugin *plugin, GtkNotebook *notebook)
{
    PrivData *priv = plugin ? plugin->priv : NULL;
    GtkWidget *page;

    if (!priv)
        return;
    page = gtk_box_new(GTK_ORIENTATION_VERTICAL, 6);
    gtk_container_set_border_width(GTK_CONTAINER(page), 10);
    xs_prop_add_group_header(GTK_BOX(page),
                             "Process list sampled directly from /proc; default layout follows .conkyrc_proc.");
    pl_add_int(plugin, page, "Update (ms)", "update_ms", priv->update_ms,
               100, 60000);
    pl_add_int(plugin, page, "Rows", "row_count", priv->row_count, 1, 32);
    {
        GtkWidget *combo = gtk_combo_box_text_new();
        GtkWidget *row = xs_prop_add_row(
            GTK_BOX(page), "Default sort",
            "Column used when the applet starts; header clicks can override it for this session",
            combo);

        (void)row;
        gtk_combo_box_text_append_text(GTK_COMBO_BOX_TEXT(combo), "NAME");
        gtk_combo_box_text_append_text(GTK_COMBO_BOX_TEXT(combo), "PID");
        gtk_combo_box_text_append_text(GTK_COMBO_BOX_TEXT(combo), "CPU");
        gtk_combo_box_text_append_text(GTK_COMBO_BOX_TEXT(combo), "MEM");
        gtk_combo_box_text_append_text(GTK_COMBO_BOX_TEXT(combo), "I/O");
        gtk_combo_box_set_active(GTK_COMBO_BOX(combo),
                                 priv->sorting.default_sort);
        g_signal_connect(combo, "changed",
                         G_CALLBACK(pl_default_sort_changed), plugin);
    }
    {
        GtkWidget *combo = gtk_combo_box_text_new();
        GtkWidget *row = xs_prop_add_row(
            GTK_BOX(page), "Default direction",
            "Initial direction for the selected default sort",
            combo);

        (void)row;
        gtk_combo_box_text_append_text(GTK_COMBO_BOX_TEXT(combo), "Descending");
        gtk_combo_box_text_append_text(GTK_COMBO_BOX_TEXT(combo), "Ascending");
        gtk_combo_box_set_active(GTK_COMBO_BOX(combo),
                                 priv->sorting.default_descending ? 0 : 1);
        g_signal_connect(combo, "changed",
                         G_CALLBACK(pl_default_direction_changed), plugin);
    }
    {
        GtkWidget *combo = gtk_combo_box_text_new();
        GtkWidget *row = xs_prop_add_row(
            GTK_BOX(page), "CPU mode",
            "Per core: 100% is one thread; Conky: 100% is all online threads",
            combo);

        (void)row;
        gtk_combo_box_text_append_text(GTK_COMBO_BOX_TEXT(combo),
                                      "Per core (100% = one thread)");
        gtk_combo_box_text_append_text(GTK_COMBO_BOX_TEXT(combo),
                                      "Conky (${top cpu} — 100% = all online threads)");
        gtk_combo_box_set_active(GTK_COMBO_BOX(combo),
                                 priv->cpu_basis ? 1 : 0);
        g_signal_connect(combo, "changed",
                         G_CALLBACK(pl_cpu_basis_changed), plugin);
    }
    pl_add_int(plugin, page, "Window width", "window_width",
               priv->window_width, 320, 1200);
    pl_add_int(plugin, page, "Window height", "window_height",
               priv->window_height, 100, 1000);
    pl_add_font(plugin, page, "Title font", "title_font", priv->title_font);
    pl_add_font(plugin, page, "Row font", "row_font", priv->row_font);
    pl_add_color(plugin, page, "Background", "background_color",
                 priv->background);
    pl_add_color(plugin, page, "Accent", "accent_color", priv->accent);
    pl_add_color(plugin, page, "Title", "title_color", priv->title);
    pl_add_color(plugin, page, "Column headers", "header_color", priv->header);
    pl_add_color(plugin, page, "Text", "text_color", priv->text);
    gtk_notebook_append_page(notebook, page, gtk_label_new("Process List"));
    gtk_widget_show_all(page);
}

static guint pl_tick(XsPlugin *plugin)
{
    PrivData *priv = plugin ? plugin->priv : NULL;

    if (!priv || !plugin->win)
        return 0;
    pl_sample(priv);
    pl_rerender(priv);
    return priv->update_ms;
}

static void pl_draw(XsPlugin *plugin, cairo_t *cr, int width, int height)
{
    PrivData *priv = plugin ? plugin->priv : NULL;

    if (!priv)
        return;
    if (!priv->cache || priv->cache_width != width ||
        priv->cache_height != height) {
        if (priv->cache)
            cairo_surface_destroy(priv->cache);
        priv->cache = pl_render(priv, width, height);
        priv->cache_width = width;
        priv->cache_height = height;
    }
    cairo_set_source_surface(cr, priv->cache, 0, 0);
    cairo_paint(cr);
}

static void pl_save_default_string(PrivData *priv, const char *key,
                                   const char *value)
{
    if (!g_key_file_has_key(priv->kf, priv->plugin->name, key, NULL))
        g_key_file_set_string(priv->kf, priv->plugin->name, key, value);
}

static void pl_save_default_color(PrivData *priv, const char *key,
                                  const gdouble color[4])
{
    g_autofree char *value = NULL;

    value = pl_color_string(color);
    g_key_file_set_string(priv->kf, priv->plugin->name, key, value);
}

static int pl_init(XsPlugin *plugin, GKeyFile *kf)
{
    PrivData *priv = g_new0(PrivData, 1);
    g_autofree char *title_font = NULL;
    g_autofree char *row_font = NULL;
    g_autofree char *cpu_basis = NULL;
    g_autofree char *default_sort = NULL;
    g_autofree char *default_direction = NULL;
    gint x;
    gint y;

    priv->plugin = plugin;
    priv->kf = kf;
    priv->update_ms = CLAMP(xs_host_api()->conf_int(
        kf, plugin->name, "update_ms", PL_UPDATE_DEFAULT), 100, 60000);
    priv->row_count = CLAMP(xs_host_api()->conf_int(
        kf, plugin->name, "row_count", PL_ROWS_DEFAULT), 1, 32);
    cpu_basis = xs_host_api()->conf_str(kf, plugin->name, "cpu_basis",
                                        "per-core");
    priv->cpu_basis = g_strcmp0(cpu_basis, "all-cores") == 0;
    default_sort = xs_host_api()->conf_str(kf, plugin->name,
                                            "default_sort", "cpu");
    default_direction = xs_host_api()->conf_str(kf, plugin->name,
                                                 "default_direction",
                                                 "descending");
    priv->sorting.default_sort = pl_sort_value(default_sort);
    priv->sorting.default_descending =
        g_strcmp0(default_direction, "ascending") != 0;
    priv->sorting.sort = priv->sorting.default_sort;
    priv->sorting.descending = priv->sorting.default_descending;
    priv->window_width = CLAMP(xs_host_api()->conf_int(
        kf, plugin->name, "window_width", PL_WIDTH_DEFAULT), 320, 1200);
    priv->window_height = CLAMP(xs_host_api()->conf_int(
        kf, plugin->name, "window_height", PL_HEIGHT_DEFAULT), 100, 1000);
    title_font = xs_host_api()->conf_str(kf, plugin->name, "title_font",
                                         PL_TITLE_FONT_DEFAULT);
    row_font = xs_host_api()->conf_str(kf, plugin->name, "row_font",
                                       PL_ROW_FONT_DEFAULT);
    priv->title_font = g_steal_pointer(&title_font);
    priv->row_font = g_steal_pointer(&row_font);
    pl_read_color(priv, "background_color", pl_background_default,
                  priv->background);
    pl_read_color(priv, "accent_color", pl_accent_default, priv->accent);
    pl_read_color(priv, "title_color", pl_title_default, priv->title);
    pl_read_color(priv, "header_color", pl_header_default, priv->header);
    pl_read_color(priv, "text_color", pl_text_default, priv->text);
    priv->previous = g_hash_table_new_full(g_direct_hash, g_direct_equal,
                                            NULL, pl_process_free);
    priv->snapshot = g_ptr_array_new_with_free_func(pl_process_free);
    priv->rows = g_ptr_array_new_with_free_func(pl_process_free);
    x = xs_host_api()->conf_int(kf, plugin->name, "x", 300);
    y = xs_host_api()->conf_int(kf, plugin->name, "y", 80);
    plugin->priv = priv;

    g_key_file_set_integer(kf, plugin->name, "update_ms", priv->update_ms);
    g_key_file_set_integer(kf, plugin->name, "row_count", priv->row_count);
    g_key_file_set_string(kf, plugin->name, "cpu_basis",
                          priv->cpu_basis ? "all-cores" : "per-core");
    g_key_file_set_string(kf, plugin->name, "default_sort",
                          pl_sort_name(priv->sorting.default_sort));
    g_key_file_set_string(kf, plugin->name, "default_direction",
                          priv->sorting.default_descending ? "descending" :
                          "ascending");
    g_key_file_remove_key(kf, plugin->name, "cpu_coefficient", NULL);
    g_key_file_set_integer(kf, plugin->name, "window_width",
                           priv->window_width);
    g_key_file_set_integer(kf, plugin->name, "window_height",
                           priv->window_height);
    pl_save_default_string(priv, "title_font", priv->title_font);
    pl_save_default_string(priv, "row_font", priv->row_font);
    pl_save_default_color(priv, "background_color", priv->background);
    pl_save_default_color(priv, "accent_color", priv->accent);
    pl_save_default_color(priv, "title_color", priv->title);
    pl_save_default_color(priv, "header_color", priv->header);
    pl_save_default_color(priv, "text_color", priv->text);
    xs_core_plugin_conf_flush(plugin->name);

    pl_sample(priv);
    plugin->win = xs_host_api()->make_window(plugin, x, y,
                                             priv->window_width,
                                             priv->window_height);
    if (!plugin->win) {
        g_hash_table_destroy(priv->previous);
        g_ptr_array_unref(priv->snapshot);
        g_ptr_array_unref(priv->rows);
        g_free(priv->title_font);
        g_free(priv->row_font);
        g_free(priv);
        plugin->priv = NULL;
        return -1;
    }
    priv->cache = pl_render(priv, priv->window_width, priv->window_height);
    priv->cache_width = priv->window_width;
    priv->cache_height = priv->window_height;
    xs_host_api()->set_tick(plugin, priv->update_ms);
    return 0;
}

static void pl_shutdown(XsPlugin *plugin)
{
    PrivData *priv = plugin ? plugin->priv : NULL;

    if (!priv)
        return;
    if (priv->cache)
        cairo_surface_destroy(priv->cache);
    g_hash_table_destroy(priv->previous);
    g_ptr_array_unref(priv->snapshot);
    g_ptr_array_unref(priv->rows);
    g_free(priv->title_font);
    g_free(priv->row_font);
    g_free(priv);
    plugin->priv = NULL;
}

static gboolean pl_button_press(XsPlugin *plugin, GdkEventButton *event)
{
    PrivData *priv = plugin ? plugin->priv : NULL;
    PlColumns columns;
    int column;

    if (!priv || event->type != GDK_BUTTON_PRESS || event->button != 1)
        return FALSE;
    columns = pl_columns(priv->cache_width > 0 ? priv->cache_width :
                         priv->window_width);
    column = pl_column_at(&columns, event->x, event->y);
    if (column < 0)
        return FALSE;
    priv->sorting.pressed_column = column;
    pl_rerender(priv);
    return TRUE;
}

static gboolean pl_button_release(XsPlugin *plugin, GdkEventButton *event)
{
    PrivData *priv = plugin ? plugin->priv : NULL;
    PlColumns columns;
    int column;

    if (!priv || event->type != GDK_BUTTON_RELEASE || event->button != 1)
        return FALSE;
    columns = pl_columns(priv->cache_width > 0 ? priv->cache_width :
                         priv->window_width);
    column = pl_column_at(&columns, event->x, event->y);
    if (column < 0 || column != priv->sorting.pressed_column) {
        priv->sorting.pressed_column = -1;
        return FALSE;
    }
    if (priv->sorting.last_click_column != column) {
        priv->sorting.last_click_column = column;
        priv->sorting.repeat_clicks = 1;
    } else if (priv->sorting.repeat_clicks < 3) {
        priv->sorting.repeat_clicks++;
    }
    priv->sorting.sort = pl_column_sorts[column];
    if (priv->sorting.repeat_clicks == 1)
        priv->sorting.descending = column == PL_SORT_NAME ||
                                   column == PL_SORT_PID;
    else if (priv->sorting.repeat_clicks == 2)
        priv->sorting.descending = !(column == PL_SORT_NAME ||
                                     column == PL_SORT_PID);
    else {
        priv->sorting.sort = priv->sorting.default_sort;
        priv->sorting.descending = priv->sorting.default_descending;
    }
    priv->sorting.pressed_column = -1;
    pl_rebuild_rows(priv);
    pl_rerender(priv);
    return TRUE;
}

static gboolean pl_button(XsPlugin *plugin, GdkEventButton *event)
{
    return event->type == GDK_BUTTON_PRESS ? pl_button_press(plugin, event) :
                                             pl_button_release(plugin, event);
}

static const XsPluginOps pl_ops = {
    .init = pl_init,
    .draw = pl_draw,
    .tick = pl_tick,
    .shutdown = pl_shutdown,
    .button = pl_button,
    .motion = NULL,
    .menu = NULL,
    .menu_cmd = NULL,
    .properties = pl_properties,
    .fill_themes = NULL,
    .scroll = NULL,
    .enter = NULL,
    .leave = NULL,
    .guest_list_changed = NULL,
};

static XsPluginDesc pl_desc = {
    "process_list", XS_API_VERSION, &pl_ops,
    "Top processes sampled directly from /proc", "xscreenlets", "0.1"
};
XS_PLUGIN_EXPORT(&pl_desc)
