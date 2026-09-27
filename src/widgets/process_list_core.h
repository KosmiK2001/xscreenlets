/* process_list_core.h — логика сэмплирования /proc, вынесенная из GTK-слоя
 * ради тестируемости. Плагин process_list.c по-прежнему владеет окном и
 * отрисовкой; всё, что можно проверить без X, живёт здесь.
 *
 * Зачем вынесено: pl_sample() раз в секунду обходил весь /proc и на каждый
 * pid открывал stat + status + io. На 1021 процессе это 3063 openat в
 * секунду — 96% всех системных вызовов демона и 2/3 его CPU. Модуль
 * добавляет два решения, каждое проверяется тестом:
 *
 *   1. RSS берётся из /proc/<pid>/statm (одно целое поле), а не из
 *      /proc/<pid>/status (60+ строк, нужен ради двух значений). statm
 *      не содержит State, поэтому running определяется по одному символу
 *      в /proc/<pid>/stat — файл и так уже читается ради utime/stime.
 *   2. Файл io открывается не для каждого pid, а только когда столбец I/O
 *      реально нужен (сортировка по I/O или колонка видима). На этой
 *      машине 10% запросов io возвращали ENOENT — это потоки ядра.
 */
#ifndef PROCESS_LIST_CORE_H
#define PROCESS_LIST_CORE_H

#include <glib.h>

/* Длина имени процесса (comm). 96 с запасом на длинные comm вроде
 * "Web Content"; обрезается, а не отвергается. */
#define PL_NAME_MAX 96

/* Размер одного процесса в байтах, как его отдаёт /proc. */
typedef struct {
    gint     pid;
    guint64  cpu_ticks;      /* utime + stime */
    guint64  start_time;     /* поле 22 /proc/pid/stat — идентификатор процесса */
    guint64  disk_read_bytes;   /* G_MAXUINT64 = неизвестно */
    guint64  disk_write_bytes;  /* G_MAXUINT64 = неизвестно */
    gint64   rss_bytes;
    gint     cpu_tenths;     /* 1000 = 100.0% одного логического потока */
    gint64   io_bytes_per_sec;
    gboolean running;
    char     name[PL_NAME_MAX];
} PlProcess;

/* Что именно нужно от этого сэмпла. Всё лишнее не читается. */
typedef struct {
    gboolean want_io;      /* нужен столбец I/O или сортировка по нему */
    gboolean want_rss;     /* нужен столбец MEM */
    gboolean want_state;   /* нужен счётчик running */
} PlNeeds;

/* Одно поле io. G_MAXUINT64 — файла нет (поток ядра, ENOENT). */
typedef struct {
    guint64 read_bytes;
    guint64 write_bytes;
} PlIo;

/* Разбор одной строки /proc/<pid>/stat.
 *
 * comm может содержать пробелы и скобки, поэтому поле ищется по паре
 * скобок, а не по g_strsplit: имя процесса вроде "(Web Content)" ломало
 * наивное разбиение. utime=14, stime=15, starttime=22 (1-based, как в
 * документации ядра; после ')' идёт space, поэтому индексы -2).
 *
 * Возвращает FALSE на мусоре — тогда pid пропускается целиком. */
gboolean pl_core_parse_stat(const char *text,
                            PlProcess *out,
                            gboolean *out_running);

/* Разбор /proc/<pid>/statm: "size resident shared text lib data dt".
 * Нужно только поле resident (второе), в страницах. */
gboolean pl_core_parse_statm(const char *text, guint page_size,
                             guint64 *out_rss_bytes);

/* Разбор /proc/<pid>/io: пары "key: value", нужны read_bytes/write_bytes. */
gboolean pl_core_parse_io(const char *text, PlIo *out);

/* Нужно ли читать io для этого сэмпла.
 *
 * Логика: сортировка по I/O требует значений для ВСЕХ процессов (иначе
 * порядок неверен), а колонка — только для видимых. Если ни то, ни другое
 * не нужно, файл не открывается вовсе: это 1/3 всех openat на машине с
 * 1000+ процессов, и на этой конкретно 10% из них — ENOENT на потоках
 * ядра, то есть работа впустую. */
gboolean pl_core_needs_io(gboolean io_column_visible,
                          gboolean io_sort_active);

/* Пересчёт производных величин между двумя сэмплами.
 *
 * cpu_tenths: 1000 = 100.0% одного логического потока, как в conky.
 * io_bytes_per_sec: сумма дельт чтения и записи, в байтах/с.
 * Оба требуют предыдущего сэмпла того же процесса (совпадение по
 * start_time — иначе это перезапущенный процесс с тем же pid).
 *
 * cpu_basis: 0 = на один логический поток, 1 = делить на число онлайн-CPU.
 * cores_online передаётся снаружи, чтобы не читать sysfs на каждый процесс. */
void pl_core_compute_rates(PlProcess *cur,
                           const PlProcess *prev,
                           guint64 elapsed_us,
                           long ticks_per_second,
                           gint cpu_basis,
                           gint64 cores_online);

/* Сколько ядер онлайн. Кэшируется вызывающим: это чтение sysfs, а не
 * результат работы на один процесс — читать его на каждый pid в цикле
 * значит удваивать и так доминирующий обход /proc. */
gint64 pl_core_online_cores(void);

#endif
