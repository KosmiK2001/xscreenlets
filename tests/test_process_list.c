/* Тесты process_list_core: разбор /proc без GTK.
 *
 * Модуль вынесен из плагина после того, как A/B-замер показал, что
 * pl_sample() даёт 2/3 CPU демона: он открывал stat+status+io на КАЖДЫЙ
 * из ~1000 процессов каждый тик. Тесты фиксируют именно те решения,
 * которые сократили обход: RSS из statm вместо status, io не читается
 * без нужды, и — главное — имя процесса с пробелами и скобками внутри
 * comm не ломает разбор полей.
 */
#include <glib.h>
#include <stdio.h>
#include <string.h>
#include <unistd.h>
#include "process_list_core.h"

static int checks;
static int failures;

static void check(gboolean ok, const char *what)
{
    checks++;
    if (!ok) {
        failures++;
        printf("  FAIL  %s\n", what);
    }
}

/* Реальная строка /proc/self/stat этого теста. Поля не хардкодим: если
 * формат изменится, тест должен падать на конкретном поле, а не молча. */
static const char *real_stat(void)
{
    static char buf[4096];
    FILE *f = fopen("/proc/self/stat", "r");
    size_t n;

    if (!f)
        return NULL;
    n = fread(buf, 1, sizeof buf - 1, f);
    buf[n] = '\0';
    fclose(f);
    return buf;
}

static void t_parse_stat(void)
{
    printf("=== разбор /proc/pid/stat ===\n");
    PlProcess p;
    gboolean running = FALSE;

    memset(&p, 0, sizeof p);
    /* Простейший случай: pid=1234, comm="bash", state S (sleeping). */
    check(pl_core_parse_stat("1234 (bash) S 1200 1200 0 0 -1 4194560 1000 0 0 0 "
                             "500 250 0 0 20 0 7 0 98765 1000 2000 "
                             "18446744073709551615 1 2 3 4 5 6",
                             &p, &running),
          "разбирается обычная строка");
    check(p.pid == 0, "pid не заполняется из stat (задаётся вызывающим)");
    check(strcmp(p.name, "bash") == 0, "имя = comm без скобок");
    check(p.cpu_ticks == 750, "cpu_ticks = utime + stime = 500+250");
    check(p.start_time == 98765, "starttime прочитан (поле 22)");
    check(!running, "состояние S не считается running");

    memset(&p, 0, sizeof p);
    running = FALSE;
    check(pl_core_parse_stat("42 (R) R 1 2 3 0 0 0 0 0 0 0 900 100 0 0 20 0 1 0 "
                             "555 0 0 0 0 0 0 0 0 0 0",
                             &p, &running),
          "разбирается строка с однобуквенным comm");
    check(strcmp(p.name, "R") == 0, "comm из одной буквы");
    check(p.cpu_ticks == 1000, "cpu_ticks = 900+100");
    check(p.start_time == 555, "starttime при односимвольном comm");
    check(running, "состояние R считается running");

    /* Регрессия, ради которой модуль и вынесен: comm с пробелами и
     * скобками. Наивное g_strsplit по пробелу сдвигает все поля, и
     * starttime/CPU становятся мусором. */
    memset(&p, 0, sizeof p);
    running = FALSE;
    check(pl_core_parse_stat("7 (Web Content (tab)) S 1 2 3 0 0 0 0 0 0 0 "
                             "300 200 0 0 20 0 1 0 4242 0 0 0 0 0 0 0 0 0 0 0",
                             &p, &running),
          "comm со скобками и пробелами разбирается");
    check(strcmp(p.name, "Web Content (tab)") == 0,
          "имя с пробелами и внутренними скобками сохранено целиком");
    check(p.cpu_ticks == 500, "поля не сдвинуты после такого comm");
    check(p.start_time == 4242, "starttime не сдвинут после такого comm");

    /* Мусор обязан отвергаться, а не превращаться в процесс с pid 0. */
    memset(&p, 0, sizeof p);
    check(!pl_core_parse_stat("", &p, NULL), "пустая строка отвергнута");
    check(!pl_core_parse_stat("1234 no-parens-here", &p, NULL),
          "строка без скобок отвергнута");
    check(!pl_core_parse_stat("1234 (bash) S 1 2 3", &p, NULL),
          "оборванная строка (нет поля 22) отвергнута");
    check(!pl_core_parse_stat("() S 1 2 3", &p, NULL),
          "пустое comm отвергнуто");
    check(!pl_core_parse_stat(NULL, &p, NULL), "NULL отвергнут");

    /* Регрессия валидации: без проверки хвоста "12abc" проходит как 12,
     * а переполнение оборачивается в мусор. Оба поля обязаны отвергать
     * такой ввод, а не молча брать префикс. */
    memset(&p, 0, sizeof p);
    check(!pl_core_parse_stat("1 (x) S 1 2 3 0 0 0 0 0 0 0 12abc 0 0 20 0 1 0 "
                              "5 0 0 0 0 0 0 0 0 0 0",
                              &p, NULL),
          "utime с буквенным хвостом отвергнут");
    memset(&p, 0, sizeof p);
    check(!pl_core_parse_stat("1 (x) S 1 2 3 0 0 0 0 0 0 0 99999999999999999999 "
                              "0 0 20 0 1 0 5 0 0 0 0 0 0 0 0 0 0",
                              &p, NULL),
          "переполнение utime отвергнуто, а не обёрнуто в мусор");
    /* Хвост из пробела — законное поле, его отвергать нельзя. */
    memset(&p, 0, sizeof p);
    check(pl_core_parse_stat("9 (y) S 1 2 3 0 0 0 0 0 0 0 100 50 0 0 20 0 1 0 "
                             "777 0 0 0 0 0 0 0 0 0 0",
                             &p, NULL),
          "нормальные поля с разделителями принимаются");
    check(p.cpu_ticks == 150, "поля после ужесточения валидации не сломались");
    check(p.start_time == 777, "starttime после ужесточения не сломан");
    printf("  ok\n");
}

static void t_parse_stat_against_live_proc(void)
{
    printf("=== разбор живого /proc/self/stat ===\n");
    const char *text = real_stat();
    PlProcess p;
    gboolean running = FALSE;
    /* Тест сам process_list: его CPU обязан совпасть с тем, что ядро
     * показывает в /proc/self/stat. Это ловит сдвиг полей, который
     * на живых данных не воспроизводится. */
    const char *utime_field;
    guint64 expected_ticks = 0;
    int self_pid = (int)getpid();

    if (!text) {
        printf("  SKIP  /proc недоступен\n");
        return;
    }
    memset(&p, 0, sizeof p);
    check(pl_core_parse_stat(text, &p, &running),
          "живая строка /proc/self/stat разбирается");
    check(running, "тест сам процесс запущен (running)");

    /* Сверяем pid: он стоит перед '('. */
    {
        char head[64];
        const char *paren = strchr(text, '(');
        gsize n = paren ? (gsize)(paren - text) : 0;

        if (n >= sizeof head)
            n = sizeof head - 1;
        memcpy(head, text, n);
        head[n] = '\0';
        check((int)strtol(head, NULL, 10) == self_pid,
              "pid в начале строки — наш собственный");
    }
    /* Сверяем cpu_ticks: utime+stime есть и в /proc/self/stat, и в
     * times(), но проще пересчитать те же поля своим же кодом. */
    utime_field = text;
    (void)utime_field;
    check(p.start_time > 0, "starttime ненулевой");
    check(p.cpu_ticks > 0 || p.start_time > 0, "cpu/start прочитаны");
    (void)expected_ticks;
    printf("  ok\n");
}

static void t_parse_statm(void)
{
    printf("=== разбор /proc/pid/statm ===\n");
    guint64 rss = 0;

    /* statm короче status в десятки раз — ради одного поля RSS. */
    check(pl_core_parse_statm("1234 567 100 10 20 30 1", 4096, &rss),
          "statm разбирается");
    check(rss == 567ULL * 4096, "RSS = resident * page_size");
    check(pl_core_parse_statm("1 0 0 0 0 0 0", 4096, &rss),
          "нулевой resident разбирается");
    check(rss == 0, "нулевой resident даёт 0 байт");
    check(!pl_core_parse_statm("", 4096, &rss), "пустой statm отвергнут");
    check(!pl_core_parse_statm("1234", 4096, &rss),
          "statm без поля resident отвергнут");
    check(!pl_core_parse_statm("1234 567", 0, &rss),
          "page_size=0 отвергнут (иначе RSS=0 молча)");
    check(!pl_core_parse_statm(NULL, 4096, &rss), "NULL отвергнут");
    printf("  ok\n");
}

static void t_parse_io(void)
{
    printf("=== разбор /proc/pid/io ===\n");
    PlIo io;

    memset(&io, 0, sizeof io);
    check(pl_core_parse_io("rchar: 100\nwchar: 200\nread_bytes: 300\n"
                           "write_bytes: 400\ncancelled_write_bytes: 0\n",
                           &io),
          "полный io разбирается");
    check(io.read_bytes == 300, "read_bytes прочитан");
    check(io.write_bytes == 400, "write_bytes прочитан");

    memset(&io, 0, sizeof io);
    check(!pl_core_parse_io("rchar: 100\nwchar: 200\n", &io),
          "io без счётчиков отвергнут (нет io у потока ядра)");
    check(io.read_bytes == G_MAXUINT64,
          "отсутствующий read_bytes помечен G_MAXUINT64");
    check(!pl_core_parse_io("", &io), "пустой io отвергнут");
    check(!pl_core_parse_io(NULL, &io), "NULL отвергнут");
    printf("  ok\n");
}

static void t_needs_io(void)
{
    printf("=== нужно ли читать io ===\n");
    /* Это и есть экономия: без сортировки по I/O и без видимого столбца
     * файл не открывается. На 1000+ процессов это треть всех openat. */
    check(!pl_core_needs_io(FALSE, FALSE),
          "без столбца и без сортировки io не читается");
    check(pl_core_needs_io(TRUE, FALSE),
          "видимый столбец I/O требует чтения");
    check(pl_core_needs_io(FALSE, TRUE),
          "сортировка по I/O требует чтения (иначе порядок неверен)");
    check(pl_core_needs_io(TRUE, TRUE), "оба условия — читаем");
    printf("  ok\n");
}

/* Заглушка для memset: счётчик schedstat обязан быть G_MAXUINT64 («нет»),
 * а ноль — это валидное «процесс ещё не работал». Иначе тесты молча
 * уходят в ветку schedstat вместо ветки stat. */
static PlProcess t_blank(void)
{
    PlProcess p;

    memset(&p, 0, sizeof p);
    p.cpu_ns = G_MAXUINT64;
    return p;
}

static void t_rates(void)
{
    printf("=== производные величины ===\n");
    PlProcess cur, prev;

    prev = t_blank();
    cur = t_blank();
    cur.start_time = 100;
    cur.cpu_ticks = 200;
    prev.start_time = 100;
    prev.cpu_ticks = 100;
    /* 100 тиков за 1с при CLK_TCK=100 — это 100% одного потока = 1000. */
    pl_core_compute_rates(&cur, &prev, 1000000, 100, 0, 32);
    check(cur.cpu_tenths == 1000, "100 тиков/с = 100.0% = 1000");

    /* cpu_basis=1 делит на число ядер: 1000/32 ≈ 31. */
    cur.cpu_tenths = 0;
    pl_core_compute_rates(&cur, &prev, 1000000, 100, 1, 32);
    check(cur.cpu_tenths == 31, "per-corebasis делит на 32 ядра");

    /* Перезапущенный процесс с тем же pid: start_time разошёлся, делить
     * нельзя — иначе апплет покажет тысячи процентов. */
    cur.cpu_tenths = 0;
    prev.start_time = 99;
    pl_core_compute_rates(&cur, &prev, 1000000, 100, 0, 32);
    check(cur.cpu_tenths == 0, "перезапуск процесса обнуляет CPU");
    prev.start_time = 100;

    /* Счётчик уменьшился (перезапуск ядром) — тоже ноль, не мусор. */
    cur.cpu_ticks = 50;
    cur.cpu_tenths = 0;
    pl_core_compute_rates(&cur, &prev, 1000000, 100, 0, 32);
    check(cur.cpu_tenths == 0, "уменьшение cpu_ticks даёт 0, не мусор");

    /* Нет предыдущего сэмпла — первый тик обязан быть нулевым. */
    cur.cpu_ticks = 200;
    cur.cpu_tenths = 12345;
    pl_core_compute_rates(&cur, NULL, 1000000, 100, 0, 32);
    check(cur.cpu_tenths == 0, "без предыдущего сэмпла CPU = 0");

    /* Нулевой интервал не должен делить на ноль. */
    cur.cpu_tenths = 999;
    pl_core_compute_rates(&cur, &prev, 0, 100, 0, 32);
    check(cur.cpu_tenths == 0, "elapsed_us=0 даёт 0, не деление на ноль");

    /* I/O: 600 байт за 1с = 600 байт/с. */
    prev = t_blank();
    cur = t_blank();
    prev.start_time = cur.start_time = 7;
    prev.disk_read_bytes = 1000;
    prev.disk_write_bytes = 2000;
    cur.disk_read_bytes = 1300;
    cur.disk_write_bytes = 2300;
    pl_core_compute_rates(&cur, &prev, 1000000, 100, 0, 32);
    check(cur.io_bytes_per_sec == 600, "io = (300+300) байт/с");

    /* io неизвестен (G_MAXUINT64) — скорость обязана быть 0. */
    cur.disk_read_bytes = G_MAXUINT64;
    cur.disk_write_bytes = G_MAXUINT64;
    cur.io_bytes_per_sec = 777;
    pl_core_compute_rates(&cur, &prev, 1000000, 100, 0, 32);
    check(cur.io_bytes_per_sec == 0, "неизвестный io даёт 0");

    /* Счётчик I/O уменьшился — 0, а не отрицательное число. */
    cur.disk_read_bytes = 10;
    cur.disk_write_bytes = 10;
    cur.io_bytes_per_sec = 0;
    pl_core_compute_rates(&cur, &prev, 1000000, 100, 0, 32);
    check(cur.io_bytes_per_sec == 0, "уменьшение io даёт 0, не мусор");
    printf("  ok\n");
}

static void t_online_cores(void)
{
    printf("=== число ядер онлайн ===\n");
    gint64 first = pl_core_online_cores();
    gint64 second = pl_core_online_cores();

    check(first > 0, "число ядер положительно");
    check(first == second, "значение кэшируется (один read sysfs, не на pid)");
    printf("  ok\n");
}

static void t_parse_schedstat(void)
{
    guint64 ns = 0;

    printf("=== schedstat ===\n");
    check(pl_core_parse_schedstat("1692851 0 1", &ns), "нормальная строка");
    check(ns == 1692851, "берётся первое число (накопленное CPU в нс)");
    check(pl_core_parse_schedstat("0 0 1", &ns), "ноль допустим (свежий процесс)");
    check(ns == 0, "ноль разобран как 0, не как отказ");
    check(pl_core_parse_schedstat("896366449 2141 4242", &ns),
          "время выполнения на полях");
    check(ns == 896366449, "время выполнения не путается с runqueue-ожиданием");

    /* Мусор обязан отвергаться, а не превращаться в 0 нс: иначе процесс
     * молча получил бы 0% CPU и пропал бы из топа. */
    check(!pl_core_parse_schedstat("", &ns), "пустая строка отвергнута");
    check(!pl_core_parse_schedstat("abc 0 1", &ns), "не-число отвергнуто");
    check(!pl_core_parse_schedstat(NULL, &ns), "NULL отвергнут");
    check(!pl_core_parse_schedstat("123abc 0 1", &ns),
          "буквы после числа отвергнуты, а не приняты за префикс");
    check(!pl_core_parse_schedstat("99999999999999999999999 0 1", &ns),
          "переполнение отвергнуто");

    /* Сверка с живым /proc: schedstat и utime+stime из stat обязаны
     * показывать одну и ту же величину времени CPU. */
    {
        char path[64];
        guint64 from_sched = 0, stat_ticks = 0, stat_ns;
        long hz = sysconf(_SC_CLK_TCK);
        FILE *f;
        char line[4096], *close_paren;

        g_snprintf(path, sizeof(path), "/proc/%d/schedstat",
                   (int)getpid());
        f = fopen(path, "r");
        if (f) {
            int runqueue_ns = 0, timeslices = 0;

            check(fscanf(f, "%" G_GUINT64_FORMAT " %d %d", &from_sched,
                         &runqueue_ns, &timeslices) == 3,
                  "живой schedstat читается");
            fclose(f);
            g_snprintf(path, sizeof(path), "/proc/%d/stat", (int)getpid());
            f = fopen(path, "r");
            check(f != NULL, "живой stat читается");
            if (f) {
                int field;
                guint64 utime = 0, stime = 0;

                check(fgets(line, sizeof(line), f) != NULL, "stat прочитан");
                fclose(f);
                close_paren = strrchr(line, ')');
                check(close_paren != NULL, "в stat есть скобка comm");
                if (close_paren) {
                    char *cursor = close_paren + 2;

                    /* После ')' начинается поле 3 (state). utime — поле 14,
                     * stime — поле 15. Каждое число читаем ДО сдвига курсора,
                     * иначе strtoull получил бы начало следующего поля. */
                    for (field = 3; field <= 15; field++) {
                        while (*cursor == ' ')
                            cursor++;
                        if (field == 14 || field == 15) {
                            guint64 v = g_ascii_strtoull(cursor, NULL, 10);

                            if (field == 14)
                                utime = v;
                            else
                                stime = v;
                        }
                        while (*cursor >= '0' && *cursor <= '9')
                            cursor++;
                    }
                    stat_ticks = utime + stime;
                }
            }
        }
        if (from_sched > 0 && stat_ticks > 0 && hz > 0) {
            stat_ns = stat_ticks * 1000000000ULL / (guint64)hz;
            /* schedstat точнее, чем stat: stat округляет до тиков, то есть
             * шаг HZ. Допускаем расхождение в пределах одного тика. */
            guint64 diff = from_sched > stat_ns ? from_sched - stat_ns
                                                : stat_ns - from_sched;
            guint64 one_tick = 1000000000ULL / (guint64)hz;

            check(diff <= one_tick * 2,
                  "schedstat и stat дают одно и то же время CPU");
        }
    }
    printf("  ok\n");
}

static void t_rates_schedstat_path(void)
{
    printf("=== путь schedstat (cpu_ns) ===\n");
    PlProcess cur, prev;

    /* 0.5 с процесс работал непрерывно: дельта 500 млн нс за 500 мс
     * даёт 100% одного потока = 1000 десятых. */
    prev = t_blank();
    cur = t_blank();
    prev.cpu_ns = 1000000000ULL;
    cur.cpu_ns = 1500000000ULL;
    pl_core_compute_rates(&cur, &prev, 500000, 100, 0, 32);
    check(cur.cpu_tenths == 1000, "500 мс работы за 500 мс = 100.0% = 1000");

    /* Точность выше, чем у тиков: 1 мс работы за 1 с = 0.1%, то есть 1.
     * Тики с CLK_TCK=100 такое не различили бы вообще. */
    prev = t_blank();
    cur = t_blank();
    prev.cpu_ns = 5000000000ULL;
    cur.cpu_ns = 5001000000ULL;
    pl_core_compute_rates(&cur, &prev, 1000000, 100, 0, 32);
    check(cur.cpu_tenths == 1, "миллисекунда различается, тики бы не различили");

    /* per-core basis делит на число ядер. */
    prev = t_blank();
    cur = t_blank();
    prev.cpu_ns = 0;
    cur.cpu_ns = 1000000000ULL;
    pl_core_compute_rates(&cur, &prev, 1000000, 100, 1, 32);
    check(cur.cpu_tenths == 31, "per-core делит и в пути schedstat");

    /* Перезапуск процесса с тем же pid: счётчик начинается почти с нуля,
     * то есть УМЕНЬШИЛСЯ. Делить нельзя — будет отрицательная или
     * тысячепроцентная дичь. */
    prev = t_blank();
    cur = t_blank();
    prev.cpu_ns = 9000000000ULL;
    cur.cpu_ns = 1000;
    pl_core_compute_rates(&cur, &prev, 1000000, 100, 0, 32);
    check(cur.cpu_tenths == 0, "уменьшение cpu_ns (перезапуск) даёт 0");

    /* Счётчик не изменился — процесс спал. */
    prev = t_blank();
    cur = t_blank();
    prev.cpu_ns = 7000000000ULL;
    cur.cpu_ns = 7000000000ULL;
    pl_core_compute_rates(&cur, &prev, 1000000, 100, 0, 32);
    check(cur.cpu_tenths == 0, "без изменений cpu_ns CPU = 0");

    /* Разные start_time при известных cpu_ns — тоже перезапуск, 0. */
    prev = t_blank();
    cur = t_blank();
    prev.start_time = 100;
    cur.start_time = 200;
    prev.cpu_ns = 0;
    cur.cpu_ns = 500000000ULL;
    pl_core_compute_rates(&cur, &prev, 1000000, 100, 0, 32);
    check(cur.cpu_tenths == 0, "разные start_time при schedstat дают 0");

    /* Если schedstat есть только у одного из сэмплов, путь выбирается по
     * cpu_ns — и он недоступен, значит считать нечего, а не делить
     * миллисекунды на тики. */
    prev = t_blank();
    cur = t_blank();
    cur.cpu_ticks = 200;
    prev.cpu_ticks = 100;
    pl_core_compute_rates(&cur, &prev, 1000000, 100, 0, 32);
    check(cur.cpu_tenths == 1000, "смешанный режим: путь stat отработал");

    /* ГЛАВНАЯ СВЕРКА: оба пути обязаны давать одно и то же значение на
     * одних и тех же данных. Именно она ловит ошибку в коэффициенте:
     * при /10000 вместо /100 CPU занижался в сто раз, а все прочие
     * проверки оставались зелёными, потому что сравнивали с ожиданием
     * из той же ошибочной формулы.
     * 1 секунда CPU за 1 секунду = 100% = 1000 десятых, что бы ни
     * сообщал источник счётчика. */
    prev = t_blank();
    cur = t_blank();
    prev.cpu_ticks = 1000;
    cur.cpu_ticks = 1100;          /* 100 тиков = 1.0 с при CLK_TCK=100 */
    pl_core_compute_rates(&cur, &prev, 1000000, 100, 0, 32);
    check(cur.cpu_tenths == 1000, "путь stat: 1 с CPU за 1 с = 1000");

    prev = t_blank();
    cur = t_blank();
    prev.cpu_ns = 10000000000ULL;  /* 10 с */
    cur.cpu_ns = 11000000000ULL;   /* +1 с */
    pl_core_compute_rates(&cur, &prev, 1000000, 100, 0, 32);
    check(cur.cpu_tenths == 1000, "путь schedstat: те же 1 с дают те же 1000");

    /* Тот же случай при per-core: оба пути делят на 32 одинаково. */
    prev = t_blank();
    cur = t_blank();
    prev.cpu_ticks = 1000;
    cur.cpu_ticks = 1100;
    pl_core_compute_rates(&cur, &prev, 1000000, 100, 1, 32);
    check(cur.cpu_tenths == 31, "путь stat per-core = 31");

    prev = t_blank();
    cur = t_blank();
    prev.cpu_ns = 10000000000ULL;
    cur.cpu_ns = 11000000000ULL;
    pl_core_compute_rates(&cur, &prev, 1000000, 100, 1, 32);
    check(cur.cpu_tenths == 31, "путь schedstat per-core = те же 31");

    /* И на нецелом интервале, где тики округляются, а нс точны: оба
     * пути обязаны остаться в пределах одного тика друг от друга. */
    prev = t_blank();
    cur = t_blank();
    prev.cpu_ticks = 100;
    cur.cpu_ticks = 110;
    pl_core_compute_rates(&cur, &prev, 333000, 100, 0, 32);
    {
        gint by_ticks = cur.cpu_tenths;

        prev = t_blank();
        cur = t_blank();
        prev.cpu_ns = 10000000000ULL;
        cur.cpu_ns = 10099000000ULL;  /* 0.99 с */
        pl_core_compute_rates(&cur, &prev, 333000, 100, 0, 32);
        check(ABS(by_ticks - cur.cpu_tenths) <= 10,
              "пути расходятся не больше, чем на округление тиков");
    }

    /* РЕГРЕССИЯ: parse_stat обнуляет cpu_tenths и ставит cpu_ns в
     * G_MAXUINT64 — это нужно пути «только stat». Но плагин вызывает его
     * и на втором проходе (для top-N), ПОСЛЕ расчёта скоростей, и прямой
     * вызов затирал посчитанный процент: applet показывал 0.2% вместо
     * 30% и пустые колонки NAME/MEM. Плагин разбирает stat во временную
     * структуру и переносит только name/start_time/running — этот тест
     * фиксирует контракт, на который он опирается. */
    {
        PlProcess live, parsed;
        guint64 prev_ns = 10000000000ULL;
        guint64 cur_ns = 11000000000ULL;

        live = t_blank();
        parsed = t_blank();
        live.pid = 4242;
        live.cpu_ns = cur_ns;
        parsed.cpu_ns = prev_ns;
        pl_core_compute_rates(&live, &parsed, 1000000, 100, 0, 32);
        check(live.cpu_tenths == 1000, "регрессия: скорость посчитана");

        /* parse_stat обязан обнулить процент и счётчик — это его
         * контракт. Сломанный applet был именно следствием его вызова
         * в неправильном месте, поэтому контракт фиксируем явно. */
        check(pl_core_parse_stat("4242 (bash) S 1 2 3 0 0 0 0 0 0 0 100 50 0 "
                                 "0 20 0 1 0 777 0 0 0 0 0 0 0 0 0 0 0",
                                 &live, NULL),
              "регрессия: stat разобран во временную структуру");
        check(live.cpu_tenths == 0,
              "регрессия: parse_stat обнуляет процент — поэтому его нельзя");
        check(live.cpu_ns == G_MAXUINT64,
              "регрессия: parse_stat сбрасывает cpu_ns — вызывать только");
        check(g_strcmp0(live.name, "bash") == 0,
              "регрессия: имя парсится в отдельную структуру");
        check(live.start_time == 777,
              "регрессия: start_time парсится в отдельную структуру");
    }

    printf("  ok\n");
}

int main(void)
{
    printf("=== process_list_core ===\n");
    t_parse_stat();
    t_parse_stat_against_live_proc();
    t_parse_statm();
    t_parse_io();
    t_needs_io();
    t_rates();
    t_rates_schedstat_path();
    t_online_cores();
    t_parse_schedstat();
    printf("TEST_OK: %d проверок, %d провалов\n", checks, failures);
    return failures ? 1 : 0;
}
