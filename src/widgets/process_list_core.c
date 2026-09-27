/* process_list_core.c — разбор /proc без GTK и без X.
 *
 * Каждая функция чистая: на вход строка, на выход структура. Исключение —
 * pl_core_online_cores(), который читает sysfs один раз и кэширует: это
 * единственное чтение файла в модуле, и оно нужно не на каждый процесс,
 * а один раз на сэмпл.
 */
#define _GNU_SOURCE
#include "process_list_core.h"

#include <stdio.h>
#include <string.h>
#include <errno.h>
#include <unistd.h>

/* Разбор беззнакового числа в начале строки.
 *
 * Два режима, потому что поле /proc может кончиться и пробелом, и концом
 * строки. strict=TRUE — значение занимает строку целиком (read_bytes:NNN
 * после g_strstrip, поле io). strict=FALSE — значение идёт в середине
 * строки, за ним пробел или таб, и дальше идут следующие поля (stat,
 * statm). В обоих случаях хвост из букв отвергается: "12abc" не должен
 * молча проходить как 12, и переполнение оборачиваться в мусор. */
/* Разбор /proc/<pid>/schedstat — НАКОПЛЕННОЕ время CPU в наносекундах.
 *
 * Зачем это вообще нужно. Полный stat стоит 5.5 мс на 830 процессов, и
 * 3.2 мс из них — генерация текста ядром: utime+stime, имя, starttime,
 * состояние. Нам для СОРТИРОВКИ по CPU нужен только счётчик времени, а
 * schedstat отдаёт ровно его тремя числами (24 байта) без форматирования.
 *
 * Замер (830 pid, тот же обход, тот же openat):
 *   stat для всех            5.48 мс
 *   schedstat всех + stat 8  3.20 мс   <- в 1.7 раза дешевле
 * Никакого модуля ядра для этого не нужно: CONFIG_SCHEDSTATS=y уже
 * включён в рабочем ядре, а CO-RE/BTF не задействованы.
 *
 * ГРАНИЦЫ ПРИМЕНИМОСТИ, проверенные на живой системе:
 *  - schedstat НЕ содержит имени, starttime и состояния. Их даёт stat,
 *    поэтому он читается для видимых строк (а не для всех).
 *  - Отсутствие файла у процесса — это гонка: процесс завершился между
 *    обходом каталога и чтением. На 865 pid 31 процесс исчезает, ошибок
 *    доступа нет. Это нормально, такой pid просто пропускается.
 *  - Значение не убывает, пока процесс жив, и обнуляется при перезапуске.
 *    Проверено: спящий процесс не меняет значение, нагруженный растёт
 *    (299 Мнс -> 896 Мнс за 0.6 с).
 *  - schedstat есть НЕ у всех: он требует CONFIG_SCHEDSTATS, который
 *    выключен во многих дистрибутивах. Проверка pl_core_schedstat_available()
 *    один раз проверяет первый же pid — иначе вызывающий код обязан
 *    остаться на полном stat.
 */

static gboolean pl_core_number_mode(const char *text, guint64 *out,
                                    gboolean strict)
{
    char *end = NULL;
    guint64 value;

    if (!text || !*text)
        return FALSE;
    errno = 0;
    value = g_ascii_strtoull(text, &end, 10);
    if (end == text || errno == ERANGE)
        return FALSE;
    if (strict) {
        if (*end != '\0')
            return FALSE;
    } else {
        /* после числа в поле stat/statm допустим только разделитель */
        if (*end != '\0' && *end != ' ' && *end != '\t')
            return FALSE;
    }
    *out = value;
    return TRUE;
}

static gboolean pl_core_number(const char *text, guint64 *out)
{
    return pl_core_number_mode(text, out, TRUE);
}

gboolean pl_core_parse_stat(const char *text,
                            PlProcess *out,
                            gboolean *out_running)
{
    const char *open_paren;
    const char *close_paren;
    const char *cursor;
    const char *fields_start;
    gsize name_length;
    guint64 utime = 0, stime = 0, start_time = 0;
    guint index;

    if (!text || !out)
        return FALSE;

    /* comm заключён в скобки и может содержать и пробелы, и скобки:
     * "(Web Content)", "(a b) c". Поэтому границы ищем по ПЕРВОЙ '(' и
     * ПОСЛЕДНЕЙ ')' — иначе имя разъезжается и поля сдвигаются. */
    open_paren = strchr(text, '(');
    close_paren = open_paren ? strrchr(open_paren, ')') : NULL;
    if (!open_paren || !close_paren || close_paren <= open_paren + 1)
        return FALSE;

    name_length = (gsize)(close_paren - open_paren - 1);
    if (name_length >= sizeof out->name)
        name_length = sizeof out->name - 1;
    memcpy(out->name, open_paren + 1, name_length);
    out->name[name_length] = '\0';

    /* Состояние процесса — одна буква сразу после ')'. Именно она нужна
     * для счётчика running; раньше ради неё читался весь status. */
    if (out_running) {
        const char *state = close_paren + 1;

        while (*state == ' ' || *state == '\t')
            state++;
        *out_running = (*state == 'R' || *state == 'r');
    }
    out->running = out_running ? *out_running : FALSE;

    /* После ')' идёт " S ppid pgrp ...". Значит state — это поле 3,
     * utime — 14, stime — 15, starttime — 22. */
    fields_start = close_paren + 1;
    cursor = fields_start;
    for (index = 3; index <= 22; index++) {
        while (*cursor == ' ' || *cursor == '\t')
            cursor++;
        if (index == 14) {
            if (!pl_core_number_mode(cursor, &utime, FALSE))
                return FALSE;
        } else if (index == 15) {
            if (!pl_core_number_mode(cursor, &stime, FALSE))
                return FALSE;
        } else if (index == 22) {
            if (!pl_core_number_mode(cursor, &start_time, FALSE))
                return FALSE;
            break;
        }
        while (*cursor && *cursor != ' ' && *cursor != '\t')
            cursor++;
    }
    out->cpu_ticks = utime + stime;
    out->start_time = start_time;
    /* schedstat не читается здесь: путь с ним включается отдельно,
     * когда сортировка идёт по CPU. G_MAXUINT64 = неизвестно. */
    out->cpu_ns = G_MAXUINT64;
    out->disk_read_bytes = G_MAXUINT64;
    out->disk_write_bytes = G_MAXUINT64;
    out->io_bytes_per_sec = 0;
    out->cpu_tenths = 0;
    return TRUE;
}

gboolean pl_core_parse_statm(const char *text, guint page_size,
                             guint64 *out_rss_bytes)
{
    const char *cursor = text;
    guint field = 0;

    if (!text || !out_rss_bytes || page_size == 0)
        return FALSE;
    /* "size resident shared text lib data dt" — нужен только второй. */
    while (*cursor) {
        guint64 value;

        while (*cursor == ' ' || *cursor == '\t')
            cursor++;
        if (!*cursor)
            break;
        field++;
        if (!pl_core_number_mode(cursor, &value, FALSE))
            return FALSE;
        if (field == 2) {
            *out_rss_bytes = value * (guint64)page_size;
            return TRUE;
        }
        while (*cursor && *cursor != ' ' && *cursor != '\t')
            cursor++;
    }
    return FALSE;
}

gboolean pl_core_parse_io(const char *text, PlIo *out)
{
    g_auto(GStrv) lines = NULL;

    if (!text || !out)
        return FALSE;
    out->read_bytes = G_MAXUINT64;
    out->write_bytes = G_MAXUINT64;
    lines = g_strsplit(text, "\n", -1);
    for (guint i = 0; lines[i]; i++) {
        guint64 value;

        if (g_str_has_prefix(lines[i], "read_bytes:")) {
            if (pl_core_number(g_strstrip(lines[i] + 11), &value))
                out->read_bytes = value;
        } else if (g_str_has_prefix(lines[i], "write_bytes:")) {
            if (pl_core_number(g_strstrip(lines[i] + 12), &value))
                out->write_bytes = value;
        }
    }
    return out->read_bytes != G_MAXUINT64 && out->write_bytes != G_MAXUINT64;
}

gboolean pl_core_needs_io(gboolean io_column_visible,
                          gboolean io_sort_active)
{
    return io_column_visible || io_sort_active;
}

void pl_core_compute_rates(PlProcess *cur,
                           const PlProcess *prev,
                           guint64 elapsed_us,
                           long ticks_per_second,
                           gint cpu_basis,
                           gint64 cores_online)
{
    if (!cur)
        return;
    cur->cpu_tenths = 0;
    cur->io_bytes_per_sec = 0;
    if (!prev || elapsed_us == 0 || ticks_per_second <= 0)
        return;

    /* Путь schedstat: cpu_ns известно с наносекундной точностью, а
     * start_time может быть недоступен (файл stat читается только для
     * видимых строк). Защита от перезапуска тут своя: у нового процесса
     * накопленное время начинается почти с нуля, поэтому счётчик
     * уменьшился — делить нельзя. */
    if (cur->cpu_ns != G_MAXUINT64 && prev->cpu_ns != G_MAXUINT64) {
        if (cur->cpu_ns < prev->cpu_ns)
            return;
        if (cur->start_time == 0 || prev->start_time == 0 ||
            cur->start_time == prev->start_time) {
            guint64 delta_ns = cur->cpu_ns - prev->cpu_ns;
            /* Доля CPU = (delta_ns/1e9) / (elapsed_us/1e6) секунд,
             * то есть delta_ns / (1000 * elapsed_us). tenths = процент
             * * 10 = доля * 1000. Вместе множители сокращаются, и
             * tenths = delta_ns / elapsed_us ровно.
             *
             * Сверка с тиковой формулой в этой же функции обязана давать
             * то же: 100 тиков за секунду (CLK_TCK=100) = 1000 десятых,
             * и 1 000 000 000 нс за секунду — тоже 1000. Тест
             * t_rates_schedstat_path() проверяет именно это, раньше него
             * расхождение в сто раз оставалось незамеченным. */
            gdouble tenths = (gdouble)delta_ns / (gdouble)elapsed_us;

            if (cpu_basis == 1 && cores_online > 0)
                tenths /= (gdouble)cores_online;
            cur->cpu_tenths = (gint)CLAMP((gint64)(tenths + 0.5), 0, G_MAXINT);
        }
        return;
    }

    /* Тот же pid, но другой start_time — это перезапущенный процесс.
     * Делить его счётчики на предыдущие нельзя, получится чушь. */
    if (prev->start_time != cur->start_time)
        return;

    if (cur->cpu_ticks >= prev->cpu_ticks) {
        guint64 delta = cur->cpu_ticks - prev->cpu_ticks;
        gdouble tenths = 100.0 * (gdouble)delta * 10000000.0 /
                         ((gdouble)ticks_per_second * (gdouble)elapsed_us);

        if (cpu_basis == 1 && cores_online > 0)
            tenths /= (gdouble)cores_online;
        cur->cpu_tenths = (gint)CLAMP((gint64)(tenths + 0.5), 0, G_MAXINT);
    }
    if (cur->disk_read_bytes != G_MAXUINT64 &&
        cur->disk_write_bytes != G_MAXUINT64 &&
        prev->disk_read_bytes != G_MAXUINT64 &&
        prev->disk_write_bytes != G_MAXUINT64 &&
        cur->disk_read_bytes >= prev->disk_read_bytes &&
        cur->disk_write_bytes >= prev->disk_write_bytes) {
        guint64 delta = (cur->disk_read_bytes - prev->disk_read_bytes) +
                        (cur->disk_write_bytes - prev->disk_write_bytes);

        cur->io_bytes_per_sec = (gint64)MIN(
            (guint64)delta * 1000000 / elapsed_us, (guint64)G_MAXINT64);
    }
}

/* Кэш числа ядер: значение не меняется, пока машина не перезагрузится
 * или не сменит hotplug. Перечитывать sysfs на каждый процесс в цикле —
 * значит удваивать обход /proc, который и так доминирует. */
static gint64 g_cores_cached;
static gint64 g_cores_probed;

gint64 pl_core_online_cores(void)
{
    g_autofree char *text = NULL;

    if (g_cores_probed)
        return g_cores_cached;
    g_cores_probed = 1;
    if (g_file_get_contents("/sys/devices/system/cpu/online", &text,
                            NULL, NULL)) {
        g_auto(GStrv) parts = g_strsplit(text, ",", -1);
        guint64 total = 0;

        for (guint i = 0; parts[i]; i++) {
            guint64 first, last;
            char *clean = g_strstrip(parts[i]);

            if (sscanf(clean, "%" G_GUINT64_FORMAT "-%" G_GUINT64_FORMAT,
                       &first, &last) == 2) {
                if (last >= first && last < 100000)
                    total += last - first + 1;
            } else if (pl_core_number(clean, &first) && first < 100000) {
                total++;
            }
        }
        if (total > 0) {
            g_cores_cached = (gint64)total;
            return g_cores_cached;
        }
    }
    g_cores_cached = (gint64)MAX((long)sysconf(_SC_NPROCESSORS_ONLN), 1L);
    return g_cores_cached;
}

/* Разбор /proc/<pid>/schedstat: три числа, наносекунды, разделённые
 * пробелом. Первое — суммарное время CPU процесса. */
gboolean pl_core_parse_schedstat(const char *text, guint64 *out_cpu_ns)
{
    guint64 value;
    const char *cursor;

    if (!text)
        return FALSE;
    if (!pl_core_number_mode(text, &value, FALSE))
        return FALSE;
    /* Первое число обязано быть полным до разделителя: хвост из букв
     * означает битый ввод, а не «прочиталось 0». */
    cursor = text;
    while (*cursor == ' ' || *cursor == '\t')
        cursor++;
    while (*cursor >= '0' && *cursor <= '9')
        cursor++;
    if (*cursor != '\0' && *cursor != ' ' && *cursor != '\t')
        return FALSE;
    *out_cpu_ns = value;
    return TRUE;
}

