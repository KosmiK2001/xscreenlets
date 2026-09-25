#include "memory_monitor_core.h"
#include <assert.h>
#include <math.h>
#include <stdio.h>
#include <string.h>

static int close_enough(double a, double b)
{
    return fabs(a - b) < 0.000001;
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

    test_responsive_graph_width();
    puts("memory_monitor parser tests: OK");
    return 0;
}
