#include "memory_monitor_core.h"
#include <assert.h>
#include <math.h>
#include <stdio.h>
#include <string.h>

static int close_enough(double a, double b)
{
    return fabs(a - b) < 0.000001;
}

static void test_colors(void)
{
    gdouble color[4] = {0.0, 0.0, 0.0, 0.0};

    assert(mm_parse_color("0,0,0,0.1", color));
    assert(close_enough(color[0], 0.0));
    assert(close_enough(color[1], 0.0));
    assert(close_enough(color[2], 0.0));
    assert(close_enough(color[3], 0.1));
    assert(mm_parse_color("0.1,0.2,0.3,1", color));
    {
        char *value = mm_format_color(color);
        const char *p;
        int separators = 0;

        assert(value != NULL);
        for (p = value; *p; p++)
            if (*p == ',')
                separators++;
        assert(separators == 3);
        assert(mm_parse_color(value, color));
        g_free(value);
    }
    assert(!mm_parse_color("0,0,0,0,2", color));
    assert(!mm_parse_color("0,0,0", color));
    assert(!mm_parse_color("garbage", color));
}

static void test_section_values_text(void)
{
    char *text = mm_section_values_text(1536, 4096);

    assert(strchr(text, '\n') != NULL);
    assert(strchr(text, '/') == NULL);
    g_free(text);
}

static void test_responsive_graph_width(void)
{
    assert(mm_graph_width(320, 112, 4, 6) == 194);
    assert(mm_graph_width(200, 112, 4, 6) == 74);
    assert(mm_graph_width(80, 200, 4, 6) == 20);
    assert(mm_graph_height(100) == 12);
    assert(mm_graph_height(120) == 22);
    assert(mm_graph_height(240) == 88);
    assert(mm_graph_height(320) == 128);
    assert(mm_graph_height(344) == 140);
    assert(mm_graph_height(600) == 268);
    assert(mm_compact_graph_height(100) == 27);
    assert(mm_compact_graph_height(120) == 37);
    assert(mm_compact_graph_height(180) == 67);
    assert(mm_compact_graph_height(199) == 77);
    assert(mm_compact_hr_y(100) == 48);
    assert(mm_compact_hr_y(180) == 88);
    assert(mm_compact_content_width(100) == 92);
    assert(mm_compact_content_width(320) == 312);
    assert(mm_percent_anchor_x(1) == 4);
    assert(mm_percent_anchor_x(0) == 0);
    assert(mm_stippled_width(80, 200) == 80);
    assert(mm_stippled_width(240, 200) == 200);
    assert(mm_stippled_width(0, 200) == 0);
    assert(mm_compact_graph_width(259, 80) == 165);
    assert(mm_compact_graph_width(100, 80) == 20);
    assert(mm_section_hr_width(251) == 251);
    assert(mm_section_hr_width(92) == 92);
    assert(mm_section_hr_width(0) == 0);
}

int main(void)
{
    static const char text[] =
        "MemTotal:       16000 kB\n"
        "MemFree:         1000 kB\n"
        "MemAvailable:    6000 kB\n"
        "Buffers:          500 kB\n"
        "Cached:           2000 kB\n"
        "SwapTotal:        8000 kB\n"
        "SwapFree:         3000 kB\n";
    MemorySample sample;

    assert(mm_parse_meminfo(text, &sample));
    assert(sample.total_kib == 16000);
    assert(sample.available_kib == 6000);
    assert(close_enough(mm_ram_fraction(&sample), 0.625));
    assert(sample.swap_total_kib == 8000);
    assert(sample.swap_free_kib == 3000);
    assert(close_enough(mm_swap_fraction(&sample), 0.625));

    memset(&sample, 0, sizeof(sample));
    assert(mm_parse_meminfo("MemTotal: 8192 kB\nMemAvailable: 2048 kB\n"
                             "SwapTotal: 0 kB\nSwapFree: 0 kB\n", &sample));
    assert(close_enough(mm_ram_fraction(&sample), 0.75));
    assert(close_enough(mm_swap_fraction(&sample), 0.0));

    memset(&sample, 0, sizeof(sample));
    assert(!mm_parse_meminfo("MemTotal: 8192 kB\n", &sample));
    assert(!sample.valid);

    memset(&sample, 0, sizeof(sample));
    assert(mm_parse_meminfo("MemTotal: 4096 kB\nMemAvailable: 4096 kB\n"
                             "SwapTotal: 0 kB\nSwapFree: 0 kB\n", &sample));
    assert(close_enough(mm_ram_fraction(&sample), 0.0));
    assert(close_enough(mm_swap_fraction(&sample), 0.0));

    test_colors();
    test_section_values_text();
    test_responsive_graph_width();
    puts("memory_monitor parser tests: OK");
    return 0;
}
