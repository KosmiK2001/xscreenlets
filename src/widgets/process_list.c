/* process_list.c — C/GTK3 process list modelled after .conkyrc_proc.
 * Numeric /proc I/O happens only in tick(); draw() paints the cached surface. */
#include <gtk/gtk.h>
#include <glib.h>
#include <gmodule.h>
#include <pango/pangocairo.h>
#include <cairo.h>
#include <errno.h>
#include <fcntl.h>
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

/* Как часто перечитывать ВСЕ процессы. Таймер аплета продолжает
 * тикать каждый update_ms, но полный обход /proc идёт раз в 3 с. */
#define PL_FULL_SAMPLE_US     (3 * G_USEC_PER_SEC)
#define PL_ROWS_DEFAULT       8
#define PL_MAX_PROCESSES      16384
/* PL_NAME_MAX и PlProcess живут в process_list_core.h: разбор /proc и
 * структура процесса вынесены туда ради тестов без X. */
#include "process_list_core.h"
#include "../core/i18n.h"
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
#define PL_PROC_READ_SIZE     8192

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
    /* Рамка окна: цвет и толщина в пикселях. Толщина 0 = без рамки,
     * это поведение по умолчанию - до настройки applet вовсе не рисовал
     * рамки, и добавлять её всем подряд принудительно неправильно. */
    gdouble border[4];
    double border_width;
    char *title_font;
    char *row_font;
    GHashTable *previous;            /* pid -> PlProcess baseline */
    GPtrArray *snapshot;             /* all sampled processes, current sort */
    GPtrArray *rows;                 /* first row_count entries of snapshot */
    cairo_surface_t *cache;
    int cache_width;
    int cache_height;
    /* Радиус скругления углов окна, 0 = прямые углы. Форма окна задаётся
     * здесь, а не темой: тема у applet нет, фон рисуется кодом. */
    int corner_radius;
    /* Кэш применённой shape-маски: пересоздавать её на каждом draw
     * незачем, маска меняется только при смене радиуса или размера. */
    int shape_radius;
    int shape_w;
    int shape_h;
    guint process_count;
    guint running_count;
    gint64 last_sample_us;
    int proc_fd;
    /* Читать ли io/rss для ВСЕХ процессов или только для видимых.
     * Нужно всем, только когда по этой колонке идёт сортировка: иначе
     * порядок определялся бы по нулям. При сортировке по CPU/NAME/PID
     * эти файлы читаются вторым проходом только для top-N — на машине
     * с 1000+ процессов это снимает 2/3 всех openat. */
    gboolean want_io_everywhere;
    gboolean want_rss_everywhere;
    /* Режим schedstat: для всех pid читается дешёвый счётчик CPU (24
     * байта), а полный stat — только для видимых строк. Проверка
     * доступности файла делается один раз: если ядро собрано без
     * CONFIG_SCHEDSTATS, applet молча работает на полном пути. */
    gboolean use_schedstat;
    gboolean schedstat_usable;
    gboolean schedstat_checked;
    char *read_buffer;
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
/* Рамка окна по умолчанию - светло-серый, почти непрозрачная.
 *
 * Первая версия была 0.55/0.58/0.62 при alpha 0.7, и рамки не было видно
 * вообще: измерением на сервере получалось 56 против 45 внутри окна, то
 * есть разница в 11 единиц яркости на фоне, который сам по себе тёмный.
 * Причина в арифметике: 0.55 при alpha 0.7 даёт эффективные 0.385, а под
 * ним лежит фон обоев, и на тёмных обоях это просто тоньше тёмного.
 *
 * Вторая версия была 0.78 при alpha 0.92, и рамка вышла слишком белой -
 * на тёмном фоне это читалось уже не как граница окна, а как светлая
 * полоса поверх обоев.
 *
 * Итог: 0.62/0.66/0.72 при непрозрачности 1. Против почти чёрного фона
 * applet это около 158 в 8-битном счёте против 45 внутри - рамка читается
 * сразу, но остаётся границей, а не бликом.
 *
 * Alpha именно 1, а не меньше: у рамки нет подложки, кроме собственного
 * фона applet, и полупрозрачность здесь просто съедала контраст, не
 * давая ничего взамен (первая версия на этом и провалилась). За
 * полупрозрачностью окна целиком отвечает background_color. */
static const gdouble pl_border_default[4] = {0.62, 0.66, 0.72, 1.0};

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

static gboolean pl_read_proc(PrivData *priv, const char *relative,
                             char **data)
{
    int fd;
    gsize used = 0;

    if (priv->proc_fd < 0)
        return FALSE;
    fd = openat(priv->proc_fd, relative, O_RDONLY | O_CLOEXEC);
    if (fd < 0)
        return FALSE;
    for (;;) {
        gssize n;

        if (used + 1 >= PL_PROC_READ_SIZE) {
            close(fd);
            return FALSE;
        }
        n = read(fd, priv->read_buffer + used,
                 PL_PROC_READ_SIZE - used - 1);
        if (n < 0) {
            if (errno == EINTR)
                continue;
            close(fd);
            return FALSE;
        }
        if (n == 0)
            break;
        used += (gsize)n;
    }
    close(fd);
    priv->read_buffer[used] = '\0';
    *data = priv->read_buffer;
    return TRUE;
}

/* Дочитать «дорогие» поля для процесса, который попал в видимые строки.
 *
 * stat нужен ВСЕМ процессам: по нему сортировка по CPU и подсчёт. А вот
 * statm и io нужны только тем, кто реально рисуется — их row_count
 * (по умолчанию 8) из тысячи. Раньше оба открывались на каждый pid:
 * 2/3 всех openat демона, причём 10% запросов io — ENOENT на потоках
 * ядра, то есть работа впустую. */
static void pl_fill_extra(PrivData *priv, PlProcess *proc)
{
    char relative[64];
    char *data;
    gsize page_size;
    PlIo io;

    if (!priv || !proc)
        return;
    page_size = (gsize)sysconf(_SC_PAGESIZE);
    if (page_size > 0) {
        g_snprintf(relative, sizeof(relative), "%d/statm", proc->pid);
        if (pl_read_proc(priv, relative, &data)) {
            guint64 rss = 0;

            if (pl_core_parse_statm(data, (guint)page_size, &rss) &&
                rss <= (guint64)G_MAXINT64)
                proc->rss_bytes = (gint64)rss;
        }
    }
    if (!priv->want_io_everywhere) {
        g_snprintf(relative, sizeof(relative), "%d/io", proc->pid);
        if (pl_read_proc(priv, relative, &data) &&
            pl_core_parse_io(data, &io)) {
            proc->disk_read_bytes = io.read_bytes;
            proc->disk_write_bytes = io.write_bytes;
        }
    }
}

/* Дешёвый первый проход: только schedstat на каждый pid.
 *
 * Нужен, когда сортировка идёт по CPU или PID. schedstat — 24 байта и
 * три числа без форматирования, тогда как stat — 209 байт, которые ядро
 * печатает текстом. Имя, starttime и состояние отсюда не получить, их
 * добирает pl_fill_visible() уже для отсортированного top-N.
 *
 * Отсутствие файла — не ошибка: процесс мог завершиться между обходом
 * каталога и чтением (на 865 pid так исчезает 31, ошибок доступа нет).
 * Такой pid просто пропускается, как и раньше. */
static PlProcess *pl_read_schedstat(PrivData *priv, const char *pid_text)
{
    char relative[64];
    char *data;
    guint64 pid;
    guint64 cpu_ns = 0;
    PlProcess *proc;

    if (!g_ascii_isdigit(pid_text[0]) || !pl_number(pid_text, &pid) ||
        pid > G_MAXINT)
        return NULL;
    g_snprintf(relative, sizeof(relative), "%d/schedstat", (gint)pid);
    if (!pl_read_proc(priv, relative, &data))
        return NULL;
    if (!pl_core_parse_schedstat(data, &cpu_ns))
        return NULL;
    proc = g_new0(PlProcess, 1);
    proc->pid = (gint)pid;
    proc->cpu_ns = cpu_ns;
    /* cpu_ticks неизвестен: счётчики ядра даёт только stat. Плагин держит
     * оба счётчика, и вычисление скоростей выбирает путь по наличию
     * cpu_ns — см. pl_core_compute_rates(). */
    return proc;
}

/* Дочитать видимые строки: stat (имя, starttime, состояние) плюс
 * statm/io, если их видно. Только для top-N, поэтому дёшево.
 *
 * ВАЖНО: pl_core_parse_stat() обнуляет cpu_tenths и ставит cpu_ns в
 * G_MAXUINT64 — это нужно пути «только stat», где функция вообще не
 * вызывается, чтобы каждый сэмпл считался с нуля. Здесь же она
 * вызывается ПОСЛЕ pl_core_compute_rates(), и прямой вызов затирал бы
 * уже посчитанный процент: applet показывал 0.2% вместо 30% и пустые
 * колонки. Поэтому stat разбирается во временную структуру, а в
 * процесс переносятся только те поля, которых нет в schedstat. */
static void pl_fill_visible(PrivData *priv, PlProcess *proc,
                            gboolean *running)
{
    char relative[64];
    char *data;
    gsize page_size;
    PlProcess parsed;
    PlIo io;

    if (running)
        *running = FALSE;
    if (!priv || !proc)
        return;
    memset(&parsed, 0, sizeof parsed);
    g_snprintf(relative, sizeof(relative), "%d/stat", proc->pid);
    if (pl_read_proc(priv, relative, &data) &&
        pl_core_parse_stat(data, &parsed, running)) {
        /* name и start_time нужны для отрисовки и для защиты от
         * перезапуска процесса с тем же pid. Счётчики НЕ переносим:
         * скорости уже посчитаны по schedstat. */
        memcpy(proc->name, parsed.name, sizeof proc->name);
        proc->start_time = parsed.start_time;
        proc->running = parsed.running;
    }

    page_size = (gsize)sysconf(_SC_PAGESIZE);
    if (page_size > 0) {
        g_snprintf(relative, sizeof(relative), "%d/statm", proc->pid);
        if (pl_read_proc(priv, relative, &data)) {
            guint64 rss = 0;

            if (pl_core_parse_statm(data, (guint)page_size, &rss) &&
                rss <= (guint64)G_MAXINT64)
                proc->rss_bytes = (gint64)rss;
        }
    }
    if (priv->want_io_everywhere) {
        g_snprintf(relative, sizeof(relative), "%d/io", proc->pid);
        if (pl_read_proc(priv, relative, &data) &&
            pl_core_parse_io(data, &io)) {
            proc->disk_read_bytes = io.read_bytes;
            proc->disk_write_bytes = io.write_bytes;
        }
    }
}

static PlProcess *pl_read_pid(PrivData *priv, const char *pid_text,
                              gboolean *running)
{
    char relative[64];
    char *data;
    gboolean is_running = FALSE;
    guint64 pid;
    gsize page_size;
    PlProcess *proc;
    PlIo io;

    if (!g_ascii_isdigit(pid_text[0]) || !pl_number(pid_text, &pid) ||
        pid > G_MAXINT)
        return NULL;
    g_snprintf(relative, sizeof(relative), "%d/stat", (gint)pid);
    if (!pl_read_proc(priv, relative, &data))
        return NULL;
    proc = g_new0(PlProcess, 1);
    proc->pid = (gint)pid;
    if (!pl_core_parse_stat(data, proc, &is_running)) {
        g_free(proc);
        return NULL;
    }
    *running = is_running;

    /* statm и io читаются здесь только когда они нужны ВСЕМ процессам,
     * то есть когда по ним идёт сортировка: иначе порядок был бы по
     * нулям. Когда сортировка по CPU/NAME/PID, их дочитывает второй
     * проход pl_fill_extra() — уже для видимых строк. */
    if (!priv->want_rss_everywhere)
        return proc;
    page_size = (gsize)sysconf(_SC_PAGESIZE);
    if (page_size > 0) {
        g_snprintf(relative, sizeof(relative), "%d/statm", proc->pid);
        if (pl_read_proc(priv, relative, &data)) {
            guint64 rss = 0;

            if (pl_core_parse_statm(data, (guint)page_size, &rss) &&
                rss <= (guint64)G_MAXINT64)
                proc->rss_bytes = (gint64)rss;
        }
    }
    if (priv->want_io_everywhere) {
        g_snprintf(relative, sizeof(relative), "%d/io", proc->pid);
        if (pl_read_proc(priv, relative, &data) &&
            pl_core_parse_io(data, &io)) {
            proc->disk_read_bytes = io.read_bytes;
            proc->disk_write_bytes = io.write_bytes;
        }
    }
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
    gint64 cores_online;
    long ticks_per_second = sysconf(_SC_CLK_TCK);

    if (ticks_per_second <= 0)
        ticks_per_second = 100;
    if (elapsed_us <= 0)
        elapsed_us = (gint64)priv->update_ms * 1000;
    priv->last_sample_us = now;
    priv->process_count = 0;
    priv->running_count = 0;
    /* io читается только если влияет на порядок или на вывод. Раньше
     * файл открывался на каждый pid безусловно — 10% запросов к io были
     * ENOENT на потоках ядра, то есть работа впустую. */
    priv->want_io_everywhere =
        pl_core_needs_io(TRUE, priv->sorting.sort == PL_SORT_IO);
    priv->want_rss_everywhere = (priv->sorting.sort == PL_SORT_MEM);

    /* Режим schedstat включается, когда сортировке не нужны rss/io и её не
     * волнует имя: тогда единственное, что нужно от каждого процесса, —
     * счётчик CPU, а он есть в schedstat (24 байта) вместо stat (209
     * байт текста). При сортировке по NAME/MEM/IO полный stat обязателен:
     * первый не знает имён, второй — rss, третий — счётчиков io.
     * Если файла нет (ядро без CONFIG_SCHEDSTATS) — тихо откатываемся
     * на полный путь; проверка делается один раз за работу applet-а. */
    if (!priv->schedstat_checked) {
        char *probe = NULL;
        guint64 probe_ns = 0;

        priv->schedstat_checked = TRUE;
        priv->schedstat_usable =
            pl_read_proc(priv, "self/schedstat", &probe) &&
            pl_core_parse_schedstat(probe, &probe_ns);
    }
    priv->use_schedstat =
        !priv->want_rss_everywhere && priv->sorting.sort != PL_SORT_NAME &&
        priv->sorting.sort != PL_SORT_IO && priv->schedstat_usable;

    directory = g_dir_open("/proc", 0, NULL);
    if (directory) {
        while ((entry = g_dir_read_name(directory)) != NULL &&
               current->len < PL_MAX_PROCESSES) {
            gboolean running = FALSE;
            PlProcess *proc;

            if (!g_ascii_isdigit(entry[0]))
                continue;
            if (priv->use_schedstat) {
                proc = pl_read_schedstat(priv, entry);
                if (!proc)
                    continue;
            } else {
                proc = pl_read_pid(priv, entry, &running);
                if (!proc)
                    continue;
            }
            priv->process_count++;
            if (running)
                priv->running_count++;
            g_ptr_array_add(current, proc);
        }
        g_dir_close(directory);
    }

    /* Число ядер читается ОДИН раз на сэмпл, а не на каждый процесс:
     * раньше pl_online_cpu_count() стоял внутри этого цикла и перечитывал
     * sysfs 1021 раз за тик. */
    cores_online = priv->cpu_basis == 1 ? pl_core_online_cores() : 0;
    for (i = 0; i < current->len; i++) {
        PlProcess *proc = g_ptr_array_index(current, i);
        PlProcess *old = g_hash_table_lookup(priv->previous,
                                             GINT_TO_POINTER(proc->pid));

        pl_core_compute_rates(proc, old, (guint64)elapsed_us,
                              ticks_per_second, priv->cpu_basis,
                              cores_online);
    }

    g_hash_table_remove_all(priv->previous);
    for (i = 0; i < current->len; i++) {
        const PlProcess *proc = g_ptr_array_index(current, i);
        PlProcess *baseline = g_memdup2(proc, sizeof(*baseline));
        g_hash_table_insert(priv->previous, GINT_TO_POINTER(proc->pid),
                            baseline);
    }
    g_ptr_array_sort_with_data(current, pl_compare, &priv->sorting);
    /* Второй проход: дочитываем недостающее уже для отсортированного
     * top-N. Сортировать по CPU/MEM можно и по одному счётчику — нужно
     * только то, что реально попадёт в строки.
     *
     * В режиме schedstat здесь берётся stat: из него нужны имя,
     * start_time и состояние процесса, которых в schedstat нет. */
    if (priv->use_schedstat) {
        /* Счётчик Running в этом режиме осмыслен только по видимым
         * строкам: состояние всех процессов потребовало бы stat на
         * каждом pid, то есть ровно того, от чего уходим. */
        for (i = 0; i < MIN(current->len, priv->row_count); i++) {
            PlProcess *proc = g_ptr_array_index(current, i);
            gboolean is_running = FALSE;

            pl_fill_visible(priv, proc, &is_running);
            if (is_running)
                priv->running_count++;
        }
    } else {
        for (i = 0; i < MIN(current->len, priv->row_count); i++)
            pl_fill_extra(priv, g_ptr_array_index(current, i));
    }
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

static int pl_column_at(const PlColumns *columns, double x, double y,
                        const char *font_name)
{
    static const gdouble hit_padding = 3.0;
    PangoFontDescription *font = pango_font_description_from_string(font_name);
    cairo_surface_t *surface = cairo_image_surface_create(CAIRO_FORMAT_ARGB32, 1, 1);
    cairo_t *cr = cairo_create(surface);
    PangoLayout *layout = pango_cairo_create_layout(cr);
    int result = -1;

    if (!font || cairo_status(cr) != CAIRO_STATUS_SUCCESS) {
        if (layout)
            g_object_unref(layout);
        if (font)
            pango_font_description_free(font);
        cairo_destroy(cr);
        cairo_surface_destroy(surface);
        return -1;
    }
    pango_layout_set_font_description(layout, font);
    if (y < PL_HEADER_Y - 5.0 || y > PL_HEADER_Y + 16.0) {
        result = -1;
    } else {
        for (guint i = 0; i < G_N_ELEMENTS(pl_column_names); i++) {
            gboolean left = i == PL_SORT_NAME;
            double layout_width = MAX(columns->width[i] -
                                      2.0 * PL_TEXT_PADDING, 1.0);
            PangoRectangle ink;
            PangoRectangle logical;
            double text_x;

            pango_layout_set_text(layout, pl_column_names[i], -1);
            pango_layout_set_width(layout,
                                   (int)(layout_width * PANGO_SCALE));
            pango_layout_set_ellipsize(layout, PANGO_ELLIPSIZE_END);
            pango_layout_set_alignment(layout, left ? PANGO_ALIGN_LEFT :
                                                    PANGO_ALIGN_RIGHT);
            pango_layout_get_pixel_extents(layout, &ink, &logical);
            text_x = left ?
                columns->right[i] - columns->width[i] + PL_TEXT_PADDING + ink.x :
                columns->right[i] - PL_TEXT_PADDING - ink.width;
            if (x >= text_x - hit_padding &&
                x <= text_x + ink.width + hit_padding) {
                result = (int)i;
                break;
            }
        }
    }
    g_object_unref(layout);
    pango_font_description_free(font);
    cairo_destroy(cr);
    cairo_surface_destroy(surface);
    return result;
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

/* Определения ниже pl_render(), где они уже используются. */
static void pl_rounded_path(cairo_t *cr, int width, int height, int radius);
static void pl_border_region_path(cairo_t *cr, int width, int height,
                                  double radius, double thickness);
static double pl_corner_radius_value(int value);
static gboolean pl_corner_radius_is_rounded(double radius);

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

    /* Клип по округлому контуру ДО заливки фона.
     *
     * Фон заливается прямоугольником на весь размер окна, и маска X-сервера
     * срезает углы уже после того, как всё нарисовано. Без клипа в углах
     * остаются пиксели фона, маска их срежет, и получится ровно тот рваный
     * угол, который мы уже чинили в acpi_battery.
     *
     * dm_rounded_path() рисует контур ПРОВОДКОЙ и с отступом внутрь на
     * пиксель: маска режет по краю окна, поэтому контур, проведённый по
     * самому краю, терял бы половину обводки под срез. */
    cairo_save(cr);
    pl_rounded_path(cr, width, height, priv->corner_radius);
    cairo_clip(cr);

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
        /* В режиме schedstat состояние читается только для видимых строк,
         * поэтому счётчик Running показывает их, а не все процессы.
         * Врать было бы хуже: молчаливое «0» выглядит как зависшая
         * система. При полном пути счётчик, как и раньше, по всем. */
        char *running = priv->use_schedstat
            ? g_strdup_printf("     Running (top %u): %u", priv->row_count,
                              priv->running_count)
            : g_strdup_printf("     Running: %u", priv->running_count);
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
        /* Regex собирается ОДИН раз на процесс, а не на каждую строку
         * каждого кадра: раньше здесь было row_count компиляций в секунду.
         *
         * ВАЖНО: regex НЕЛЬЗЯ освобождать в конце итерации — ниже стоит
         * g_regex_unref(), оставшийся от прежней версии, где regex был
         * локальным. Со static он освобождался на первой же строке, и
         * вторая итерация работала с освобождённым GRegex: демон падал
         * при старте с SIGSEGV (pc == rax, вызов через освобождённый
         * указатель). Поэтому ниже unref убран. */
        static GRegex *regex;

        if (!regex)
            regex = g_regex_new("[[:cntrl:]]", 0, 0, NULL);
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
    }
    pl_dotted_line(cr, priv->title, PL_PADDING,
                   height - PL_PADDING - 1.0, right);

    pango_layout_set_font_description(layout, title_font);
    pango_layout_set_text(layout, " ", -1);
    (void)pl_text_height(layout);

    /* Закрываем clip, открытый в начале render. Без restore контекст
     * уедет с балансом save/restore, и следующий draw начнётся с лишним
     * уровнем - со временем cairo начнёт жаловаться на стек. */
    cairo_restore(cr);

    /* Рамка окна - ПОСЛЕ закрытия clip, и это не stylistic выбор.
     *
     * clip в render построен по контуру с отступом 1 px (pl_rounded_path
     * отступает внутрь на пиксель, чтобы дуга не уходила под срез маски).
     * Обводка же идёт по контуру с отступом border_width/2. При толщине 1
     * это 0.5, то есть обводка целиком ложилась в полосу, которую clip
     * отбрасывает, и рамка не рисовалась ВООБЩЕ. При толщине 4 отступ 2,
     * и в clip попадали только 3 пикселя из 4 - ровно столько я и
     * измерил на сервере (192 на y=1..3).
     *
     * Раньше это выглядело как "минимальная толщина 2", но на самом деле
     * рамка просто теряла часть ширины, причём чем толще, тем заметнее
     * недобор был в пикселях.
     *
     * После restore клипа нет, но маска X-сервера по-прежнему режет углы,
     * поэтому за скругление рамка вылезти не может. Собственный контур
     * с отступом border_width/2 остаётся - он нужен, чтобы дуга обводки
     * не оказалась снаружи дуги маски и не срезалась. */
    if (priv->border_width > 0.0) {
        cairo_set_fill_rule(cr, CAIRO_FILL_RULE_EVEN_ODD);
        pl_border_region_path(cr, width, height,
                              pl_corner_radius_value(priv->corner_radius),
                              priv->border_width);
        cairo_set_source_rgba(cr, priv->border[0], priv->border[1],
                              priv->border[2], priv->border[3]);
        cairo_fill(cr);
        cairo_set_fill_rule(cr, CAIRO_FILL_RULE_WINDING);
    }

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
    if (strcmp(key, "border_color") == 0)
        return priv->border;
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
    } else if (strcmp(key, "corner_radius") == 0) {
        /* Радиус действует немедленно: маска окна пересобирается по
         * shape_radius в pl_apply_shape, поэтому перерисовки и пересборки
         * кэша содержимого здесь не нужно - меняется только форма. */
        priv->corner_radius = CLAMP(value, 0, 200);
        value = priv->corner_radius;
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

/* Толщина рамки из Properties.
 *
 * Пишется в конфиг через conf_dbl, поэтому ключ и ползунок разведены
 * подсказкой, а не одной строкой. Содержимое перерисовывается целиком:
 * рамка рисуется в pl_render(), а не поверх кэша, поэтому нужен полный
 * re-render.
 *
 * Определение стоит здесь, до определения pl_add_color, и оба они ниже по
 * файлу, чем pl_properties, где подключаются. */
static void pl_border_width_changed(GtkWidget *widget, gpointer data)
{
    XsPlugin *plugin = data;
    PrivData *priv = plugin ? plugin->priv : NULL;
    const char *key;
    gdouble value;

    if (!priv || !priv->kf)
        return;
    key = g_object_get_data(G_OBJECT(widget), "xs-key");
    if (!key)
        return;
    value = gtk_spin_button_get_value(GTK_SPIN_BUTTON(widget));
    priv->border_width = CLAMP(value, 0.0, 4.0);
    g_key_file_set_double(priv->kf, plugin->name, key, priv->border_width);
    xs_core_plugin_conf_flush(plugin->name);
    pl_rerender(priv);
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
        gtk_combo_box_text_append_text(GTK_COMBO_BOX_TEXT(combo), _("NAME"));
        gtk_combo_box_text_append_text(GTK_COMBO_BOX_TEXT(combo), _("PID"));
        gtk_combo_box_text_append_text(GTK_COMBO_BOX_TEXT(combo), _("CPU"));
        gtk_combo_box_text_append_text(GTK_COMBO_BOX_TEXT(combo), _("MEM"));
        gtk_combo_box_text_append_text(GTK_COMBO_BOX_TEXT(combo), _("I/O"));
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
        gtk_combo_box_text_append_text(GTK_COMBO_BOX_TEXT(combo), _("Descending"));
        gtk_combo_box_text_append_text(GTK_COMBO_BOX_TEXT(combo), _("Ascending"));
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
                                      _("Per core (100% = one thread)"));
        gtk_combo_box_text_append_text(GTK_COMBO_BOX_TEXT(combo),
                                      _("Conky (${top cpu} — 100% = all online threads)"));
        gtk_combo_box_set_active(GTK_COMBO_BOX(combo),
                                 priv->cpu_basis ? 1 : 0);
        g_signal_connect(combo, "changed",
                         G_CALLBACK(pl_cpu_basis_changed), plugin);
    }
    pl_add_int(plugin, page, "Window width", "window_width",
               priv->window_width, 320, 1200);
    pl_add_int(plugin, page, "Window height", "window_height",
               priv->window_height, 100, 1000);
    /* Радиус углов окна в пикселях, 0 = прямые углы. Формула среза та же,
     * что у disk_monitor и acpi_battery, поэтому одинаковое значение даёт
     * одинаковый на вид угол у соседних апплетов. */
    pl_add_int(plugin, page, "Corner radius", "corner_radius",
               priv->corner_radius, 0, 200);
    /* Рамка окна: цвет кнопкой, толщина ползунком. Порядок неудобный по
     * смыслу (цвет перед толщиной), но добавляется после уже существующих
     * пунктов, и переставлять их ради cosmetics не хочется. */
    pl_add_color(plugin, page, "Border color", "border_color",
                 priv->border);
    {
        /* Ползунок с шагом 1 и нулём цифр: толщина рамки - целое число
         * пикселей, дробная часть тут только мешает и визуально, и при
         * записи в конфиг. */
        GtkWidget *bw = xs_prop_add_float(GTK_BOX(page), "Border width",
                                          "Window border thickness in pixels. "
                                          "0 = no border.",
                                          priv->border_width, 0.0, 4.0, 1.0, 0);

        g_object_set_data_full(G_OBJECT(bw), "xs-key", g_strdup("border_width"),
                               g_free);
        g_signal_connect(bw, "value-changed",
                         G_CALLBACK(pl_border_width_changed), plugin);
    }
    pl_add_font(plugin, page, "Title font", "title_font", priv->title_font);
    pl_add_font(plugin, page, "Row font", "row_font", priv->row_font);
    pl_add_color(plugin, page, "Background", "background_color",
                 priv->background);
    pl_add_color(plugin, page, "Accent", "accent_color", priv->accent);
    pl_add_color(plugin, page, "Title", "title_color", priv->title);
    pl_add_color(plugin, page, "Column headers", "header_color", priv->header);
    pl_add_color(plugin, page, "Text", "text_color", priv->text);
    gtk_notebook_append_page(notebook, page, gtk_label_new(_("Process List")));
    gtk_widget_show_all(page);
}

static guint pl_tick(XsPlugin *plugin)
{
    PrivData *priv = plugin ? plugin->priv : NULL;

    if (!priv || !plugin->win)
        return 0;

    /* Полный обход /proc не каждый тик.
     *
     * Замер strace: applet делал около 1020 openat в секунду - по одному
     * schedstat на каждый процесс машины, - и это был 89% всего файлового
     * ввода-вывода демона. При сортировке по CPU нужен счётчик CPU каждого
     * процесса, а он лежит только в schedstat (24 байта) или stat (209
     * байт); отказ от schedstat в пользу stat утяжелил бы чтение, а не
     * облегчил.
     *
     * Дешевле обходить реже: между полными обходами список и счётчики CPU
     * не меняются, поэтому перерисовывать есть что - ровно то же
     * изображение. Счётчик list идёт раз в платформенный update_ms, а
     * данные обновляются раз в PL_FULL_SAMPLE_US.
     *
     * Плата осознанная: процесс, который резко нагрузился, попадёт в
     * список с задержкой до PL_FULL_SAMPLE_US, а не немедленно. */
    {
        gint64 now = pl_now_us();

        if (priv->last_sample_us == 0 ||
            now - priv->last_sample_us >= PL_FULL_SAMPLE_US)
            pl_sample(priv);
    }
    pl_rerender(priv);
    return priv->update_ms;
}


/* Ширина среза у строки y для заданного радиуса.
 *
 * Та же формула, что и в pl_rounded_region(), вынесена отдельно, потому
 * что теперь по ней считается и маска окна, и рамка. Если они считаются
 * разными формулами, рамка не совпадёт с формой окна, и это видно глазом:
 * обводка по гладкой дуге уезжает на пол-пикселя от ступеней маски. */
static int pl_corner_cut(double scaled, int y, int height)
{
    double dy;

    if (y < (int)scaled)
        dy = scaled - y;
    else if (y >= height - (int)scaled)
        dy = (double)(y - (height - (int)scaled));
    else
        dy = 0.0;
    if (dy <= 0.0)
        return 0;

    /* floor(), а не округление: дуга маски никогда не должна выходить за
     * настоящую дугу, иначе в углу появится лишний пиксель фона. */
    {
        double t = scaled * scaled - dy * dy;

        if (t < 0.0)
            t = 0.0;
        return (int)floor(scaled - sqrt(t));
    }
}

/* Рамка окна как ЗАЛИВКА кольца по той же построчной сетке, что и маска.
 *
 * Раньше рамка рисулась обводкой по дуге, и на пологом участке обводка в
 * 1 px размазывалась сглаживанием между двумя пикселями: измерением на
 * сервере яркость рамки падала 160 -> 125 -> 87 -> 66 -> 52 при фоне 32,
 * то есть дуга буквально растворялась, и рамка выглядела рваной с
 * разрывами в 1-3 px. Увеличение толщины до 2 проблему снимало, но рамка
 * становилась заметно грубой.
 *
 * Заливка кольца построчной сеткой убирает разрывы без утолщения: каждый
 * пиксель заливается целиком, яркость не зависит от крутизны дуги, и
 * рамка по ширине совпадает с маской окна, то есть не выходит за срез.
 *
 * even-odd: внешний контур строится по радиусу r, внутренний по r - width.
 * Порядок контуров не важен - правило чётности смотрит на вложенность. */
static void pl_border_region_path(cairo_t *cr, int width, int height,
                                  double radius, double thickness)
{
    int t, y;
    double inner_scaled;

    if (width <= 0 || height <= 0 || thickness <= 0.0)
        return;
    t = (int)floor(thickness);
    if (t < 1 || t * 2 >= width || t * 2 >= height)
        return;

    if (pl_corner_radius_is_rounded(radius)) {
        double max_r = MIN(width, height) / 2.0;

        if (radius > max_r)
            radius = max_r;
        inner_scaled = radius - t;
        if (inner_scaled < 0.0)
            inner_scaled = 0.0;
    } else {
        radius = 0.0;
        inner_scaled = 0.0;
    }

    for (y = 0; y < height; y++) {
        int outer = pl_corner_cut(radius, y, height);
        int iy = y - t;
        int inner_h = height - 2 * t;

        cairo_rectangle(cr, outer, y, width - 2 * outer, 1);

        /* Внутренний контур рисуется ТОЛЬКО на строках уменьшенного окна.
         *
         * Это и есть главная тонкость. Если на строках вне уменьшенного
         * окна (верх и низ, где iy < 0 или iy >= height - 2*t) внутреннего
         * контура нет, то такая строка остаётся закрашенной целиком по
         * внешнему контуру - то есть становится рамкой толщиной в один
         * пиксель, и верхняя и нижняя стороны окна получают рамку.
         *
         * Если же там рисовать внутренний контур с нулевым срезом, он
         * покрывает всю строку целиком, правило чётности даёт ноль, и
         * сверху и снизу рамки не будет вовсе - остаются только бока.
         * Именно это и было: рамка шла по бокам, а сверху и снизу её не
         * было. Плюс по краям получалось вдвое толще, потому что там
         * внутренний контур всё-таки был. */
        if (iy >= 0 && iy < inner_h) {
            int inner = t + pl_corner_cut(inner_scaled, iy, inner_h);

            if (width - 2 * inner > 0)
                cairo_rectangle(cr, inner, y, width - 2 * inner, 1);
        }
    }
}

/* Радиус углов из конфига.
 *
 * Отрицательное значение в конфиге не должно превращаться в ошибку
 * shape-маски, поэтому всё, что не положительное, трактуется как «без
 * скругления». */
static double pl_corner_radius_value(int value)
{
    return value > 0 ? (double)value : 0.0;
}

static gboolean pl_corner_radius_is_rounded(double radius)
{
    return radius > 0.5;
}

/* Регион со скруглёнными углами для shape-маски X-окна.
 *
 * Формула скопирована из disk_monitor_core, а НЕ берётся оттуда
 * линковкой. Причина найдена на живом апплете: плагины собираются
 * изолированно, process_list.so линкуется только из process_list.o и
 * process_list_core.o, поэтому dm_rounded_region() в него просто не
 * попадает. При попытке позаимствовать вышло
 *
 *   process_list.so: undefined symbol: dm_corner_radius_value
 *
 * и applet переставал грузиться целиком - окно не появлялось вовсе,
 * без единой строчки в логе об ошибке, кроме undefined symbol в
 * сообщении демона.
 *
 * Так же поступает acpi_battery со своей ab_rounded_region(). Дубли
 * формулы - цена изоляции сборки плагинов, расплата за неё - обязанность
 * держать построения идентичными: тот же floor() (никогда не срезает
 * глубже настоящей дуги) и та же разбивка по строкам. */
static cairo_region_t *pl_rounded_region(int width, int height, int radius)
{
    const double r = pl_corner_radius_value(radius);
    cairo_region_t *region;
    cairo_rectangle_int_t box;
    int scaled;

    if (width <= 0 || height <= 0)
        return NULL;
    if (!pl_corner_radius_is_rounded(r))
        return NULL;

    scaled = (int)MIN(r, MIN(width, height) / 2.0);
    region = cairo_region_create();
    if (!region)
        return NULL;

    /* cairo_region хранит только целочисленные прямоугольники, поэтому
     * скруглённый контур приближается одним столбцом на строку: столбец
     * идёт от верхней дуги до нижней, что и нужно маске X-сервера. */
    for (int y = 0; y < height; y++) {
        int cut = 0;
        double dy;

        if (y < scaled)
            dy = scaled - y;
        else if (y >= height - scaled)
            dy = (double)(y - (height - scaled));
        else
            dy = 0.0;

        if (dy > 0.0) {
            double t = scaled * scaled - dy * dy;
            if (t < 0.0)
                t = 0.0;
            cut = (int)floor(scaled - sqrt(t));
        }
        box.x = cut;
        box.y = y;
        box.width = width - 2 * cut;
        box.height = 1;
        if (box.width > 0)
            cairo_region_union_rectangle(region, &box);
    }
    return region;
}

/* Контур скруглённого окна как путь, для clip в render.
 *
 * Отдельная функция, потому что dm_rounded_path() в disk_monitor.c
 * объявлена static и рисует рамку обводкой, а здесь нужен только путь:
 * рамку applet не рисует.
 *
 * Отступ внутрь на пиксель: маска X-сервера режет ровно по краю окна,
 * поэтому контур, проведённый ПО краю, терял бы половину обводки под
 * срез. */
static void pl_rounded_path(cairo_t *cr, int width, int height, int radius)
{
    const double inset = 1.0;
    double w = width - 2 * inset, h = height - 2 * inset;
    double r = pl_corner_radius_value(radius);

    if (w <= 0 || h <= 0) {
        cairo_rectangle(cr, 0, 0, width, height);
        return;
    }
    if (!pl_corner_radius_is_rounded(r)) {
        cairo_rectangle(cr, inset, inset, w, h);
        return;
    }
    r = MIN(r, MIN(w, h) / 2.0);
    cairo_new_sub_path(cr);
    cairo_arc(cr, inset + w - r, inset + r, r, -G_PI / 2.0, 0.0);
    cairo_arc(cr, inset + w - r, inset + h - r, r, 0.0, G_PI / 2.0);
    cairo_arc(cr, inset + r, inset + h - r, r, G_PI / 2.0, G_PI);
    cairo_arc(cr, inset + r, inset + r, r, G_PI, 1.5 * G_PI);
    cairo_close_path(cr);
}

/* Применить форму окна к X-серверу.
 *
 * dm_rounded_region() берётся из disk_monitor_core намеренно, а не
 * копируется: одинаковый corner_radius у соседних апплетов обязан давать
 * одинаковый на вид срез, иначе стоящие рядом окна выглядят по-разному.
 * Та же формула в acpi_battery и network_monitor считается независимо,
 * но идентично по построению.
 *
 * Именно gdk_window_shape_combine_region(), а не input_shape_:
 * input-форма влияет только на кликабельность пикселей, но они продолжают
 * рисоваться. Нужна shape, чтобы угловые пиксели перестали существовать.
 *
 * Кэш shape_radius/shape_w/shape_h: маска меняется только при смене
 * радиуса или размера окна, пересоздавать её на каждом draw (то есть
 * каждую секунду, update_ms по умолчанию 1000) незачем. */
static void pl_apply_shape(PrivData *priv, XsPlugin *p, int w, int h)
{
    GdkWindow *window;
    cairo_region_t *region;

    if (!priv || !p || !p->win || w <= 0 || h <= 0)
        return;
    if (priv->shape_radius == priv->corner_radius &&
        priv->shape_w == w && priv->shape_h == h)
        return;
    window = gtk_widget_get_window(p->win);
    if (!window)
        return;
    region = pl_rounded_region(w, h, priv->corner_radius);
    gdk_window_shape_combine_region(window, region, 0, 0);
    if (region)
        cairo_region_destroy(region);
    priv->shape_radius = priv->corner_radius;
    priv->shape_w = w;
    priv->shape_h = h;
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
    pl_apply_shape(priv, plugin, width, height);
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
    priv->proc_fd = open("/proc", O_RDONLY | O_DIRECTORY | O_CLOEXEC);
    if (priv->proc_fd < 0) {
        g_free(priv);
        return -1;
    }
    priv->read_buffer = g_malloc(PL_PROC_READ_SIZE);
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
    /* Радиус углов окна, 0 = прямые углы (поведение по умолчанию).
     * Верхняя граница 200 осмысленна при минимальной высоте окна 100:
     * радиус больше половины меньшей стороны всё равно срезается. */
    priv->corner_radius = CLAMP(xs_host_api()->conf_int(
        kf, plugin->name, "corner_radius", 0), 0, 200);
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
    pl_read_color(priv, "border_color", pl_border_default, priv->border);
    /* Толщина рамки в пикселях, 0 = без рамки. Верхняя граница 4:
     * толще рамка при окне 322x164 начинает съедать колонки, а не
     * обрамлять их. */
    priv->border_width = CLAMP(xs_host_api()->conf_dbl(
        kf, plugin->name, "border_width", 0.0), 0.0, 4.0);
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
    g_key_file_set_integer(kf, plugin->name, "corner_radius",
                           priv->corner_radius);
    g_key_file_set_double(kf, plugin->name, "border_width",
                          priv->border_width);
    pl_save_default_color(priv, "border_color", pl_border_default);
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
        g_free(priv->read_buffer);
        close(priv->proc_fd);
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
    g_free(priv->read_buffer);
    if (priv->proc_fd >= 0)
        close(priv->proc_fd);
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
    column = pl_column_at(&columns, event->x, event->y, priv->row_font);
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
    column = pl_column_at(&columns, event->x, event->y, priv->row_font);
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
        priv->sorting.last_click_column = -1;
        priv->sorting.repeat_clicks = 0;
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
    N_("Top processes sampled directly from /proc"), "kosmik2001 <kosmik2001@gmail.com>", "0.1"
};
XS_PLUGIN_EXPORT(&pl_desc)
