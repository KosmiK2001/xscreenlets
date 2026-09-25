#ifndef MEMORY_MONITOR_CORE_H
#define MEMORY_MONITOR_CORE_H

#include <glib.h>

typedef struct {
    guint64 total_kib;
    guint64 free_kib;
    guint64 available_kib;
    guint64 buffers_kib;
    guint64 cached_kib;
    guint64 shared_kib;
    guint64 swap_total_kib;
    guint64 swap_free_kib;
    gboolean valid;
} MemorySample;

#define MM_RAM_COMPONENTS 4
enum {
    MM_RAM_APPS,
    MM_RAM_SHARED,
    MM_RAM_BUFFERS,
    MM_RAM_CACHE,
};

gboolean mm_parse_color(const char *text, gdouble out[4]);
char *mm_format_color(const gdouble color[4]);
gboolean mm_parse_meminfo(const char *text, MemorySample *sample);
gdouble mm_ram_fraction(const MemorySample *sample);
void mm_ram_components(const MemorySample *sample, gdouble out[MM_RAM_COMPONENTS]);
gdouble mm_swap_fraction(const MemorySample *sample);
int mm_graph_width(int window_width, int text_width, int margin, int gap);
int mm_graph_height(int window_height);
int mm_compact_graph_height(int window_height);
int mm_compact_hr_y(int window_height);
int mm_compact_content_width(int window_width);
int mm_compact_graph_width(int window_width, int text_width);
int mm_percent_anchor_x(int window_width);
char *mm_section_values_text(guint64 used_kib, guint64 total_kib);
int mm_stippled_width(int text_width, int available_width);
int mm_section_hr_width(int available_width);

#endif
