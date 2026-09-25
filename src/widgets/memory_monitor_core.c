/* memory_monitor_core.c — deterministic /proc/meminfo value calculations. */
#include "memory_monitor_core.h"
#include <ctype.h>
#include <string.h>

static gboolean mm_parse_line(const char *line, const char *key, guint64 *value)
{
    size_t key_len = strlen(key);
    char *end = NULL;
    guint64 parsed;

    if (strncmp(line, key, key_len) != 0)
        return FALSE;
    parsed = g_ascii_strtoull(line + key_len, &end, 10);
    if (end == line + key_len || (*end != '\0' && !g_ascii_isspace(*end)))
        return FALSE;
    *value = parsed;
    return TRUE;
}

gboolean mm_parse_meminfo(const char *text, MemorySample *sample)
{
    gchar **lines;
    gboolean have_total = FALSE;
    gboolean have_available = FALSE;
    gboolean have_swap_total = FALSE;
    gboolean have_swap_free = FALSE;
    guint i;

    g_return_val_if_fail(text != NULL, FALSE);
    g_return_val_if_fail(sample != NULL, FALSE);
    memset(sample, 0, sizeof(*sample));
    lines = g_strsplit(text, "\n", -1);
    for (i = 0; lines[i]; i++) {
        guint64 value;
        if (mm_parse_line(lines[i], "MemTotal:", &value)) {
            sample->total_kib = value;
            have_total = TRUE;
        } else if (mm_parse_line(lines[i], "MemAvailable:", &value)) {
            sample->available_kib = value;
            have_available = TRUE;
        } else if (mm_parse_line(lines[i], "SwapTotal:", &value)) {
            sample->swap_total_kib = value;
            have_swap_total = TRUE;
        } else if (mm_parse_line(lines[i], "SwapFree:", &value)) {
            sample->swap_free_kib = value;
            have_swap_free = TRUE;
        }
    }
    g_strfreev(lines);
    sample->valid = have_total && sample->total_kib > 0 &&
                    have_available && have_swap_total && have_swap_free;
    return sample->valid;
}

gdouble mm_ram_fraction(const MemorySample *sample)
{
    guint64 available;

    if (!sample || !sample->valid || sample->total_kib == 0)
        return 0.0;
    available = MIN(sample->available_kib, sample->total_kib);
    return CLAMP((sample->total_kib - available) / (gdouble)sample->total_kib,
                 0.0, 1.0);
}

gdouble mm_swap_fraction(const MemorySample *sample)
{
    guint64 free_swap;

    if (!sample || sample->swap_total_kib == 0)
        return 0.0;
    free_swap = MIN(sample->swap_free_kib, sample->swap_total_kib);
    return CLAMP((sample->swap_total_kib - free_swap) /
                 (gdouble)sample->swap_total_kib, 0.0, 1.0);
}

static char *mm_format_kib_full(guint64 kib)
{
    static const char *units[] = {"KiB", "MiB", "GiB", "TiB", "PiB", "EiB"};
    gdouble value = kib;
    guint i = 0;

    while (value >= 1024.0 && i + 1 < G_N_ELEMENTS(units)) {
        value /= 1024.0;
        i++;
    }
    if (i == 0)
        return g_strdup_printf("%.0f %s", value, units[i]);
    return g_strdup_printf(value < 10.0 ? "%.2f %s" : "%.1f %s",
                           value, units[i]);
}

char *mm_section_values_text(guint64 used_kib, guint64 total_kib)
{
    char *used_text = mm_format_kib_full(used_kib);
    char *total_text = mm_format_kib_full(total_kib);
    char *result = g_strdup_printf("%s\n%s", used_text, total_text);

    g_free(total_text);
    g_free(used_text);
    return result;
}

int mm_stippled_width(int text_width, int available_width)
{
    return CLAMP(text_width, 0, MAX(available_width, 0));
}

int mm_graph_width(int window_width, int text_width, int margin, int gap)
{
    int available = window_width - MAX(text_width, 0) - 2 * margin - gap;

    return MAX(20, available);
}

int mm_graph_height(int window_height)
{
    if (window_height < 200)
        return MAX(12, (window_height - 76) / 2);
    return MAX(40, (window_height - 64) / 2);
}

int mm_compact_graph_height(int window_height)
{
    return MAX(12, (window_height - 45) / 2);
}

int mm_compact_hr_y(int window_height)
{
    return 18 + mm_compact_graph_height(window_height) + 3;
}

int mm_compact_content_width(int window_width)
{
    return MAX(1, window_width - 8);
}
