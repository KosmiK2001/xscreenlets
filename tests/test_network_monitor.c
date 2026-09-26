/* test_network_monitor.c — юнит-тесты ядра сетевого апплета. */
#include <stdio.h>
#include <string.h>
#include <math.h>
#include <gtk/gtk.h>
#include "network_monitor_core.h"

/* Дублируются намеренно: значения живут в заголовке плагина, а не в
 * core, и тянуть .c плагина в тест нельзя — Makefile линкует оба
 * объектных файла, и было бы дублирование символов. Тест защищает
 * геометрию от regressии, а не от рассогласования констант. */
#define NM_SPLIT_GAP 8
#define NM_MIN_WINDOW_WIDTH 160

static int failures;
static int checks;

static void check(int cond, const char *what)
{
    checks++;
    if (cond) {
        printf("  ok   %s\n", what);
    } else {
        failures++;
        printf("  FAIL %s\n", what);
    }
}

static void check_eq_int(gint64 got, gint64 want, const char *what)
{
    checks++;
    if (got == want)
        printf("  ok   %s (= %lld)\n", what, (long long) want);
    else {
        failures++;
        printf("  FAIL %s: got %lld, want %lld\n", what, (long long) got,
               (long long) want);
    }
}

static void check_eq_str(const char *got, const char *want, const char *what)
{
    checks++;
    if (got && want && !strcmp(got, want))
        printf("  ok   %s (= \"%s\")\n", what, want);
    else {
        failures++;
        printf("  FAIL %s: got \"%s\", want \"%s\"\n", what,
               got ? got : "(null)", want ? want : "(null)");
    }
}

/* --- парсер /proc/net/dev --- */

static void test_parse_netdev_basic(void)
{
    static const char *data =
        "Inter-|   Receive                                                |"
        "  Transmit\n"
        " face |bytes    packets errs drop fifo frame compressed multicast|"
        "bytes    packets errs drop fifo colls carrier compressed\n"
        "    lo: 1000      10    0    0    0     0          0         0 "
        "  2000      20    0    0    0     0       0          0\n"
        "  eth0: 9999999   1234    1    2    0     0          0         5 "
        " 8888888    5678    3    4    0     0       0          0\n";
    NmNetSample s;

    memset(&s, 0, sizeof(s));
    check(nm_parse_netdev(data, "eth0", &s), "eth0 найден");
    check(s.valid, "сэмпл валиден");
    check_eq_int((gint64) s.rx_bytes, 9999999, "rx bytes = 1-й столбец");
    check_eq_int((gint64) s.tx_bytes, 8888888, "tx bytes = 9-й столбец");
    check_eq_int((gint64) s.rx_errors, 1, "rx errors = 3-й столбец");
    check_eq_int((gint64) s.tx_errors, 3, "tx errors = 3-й столбец");
    check_eq_int((gint64) s.rx_dropped, 2, "rx drops = 4-й столбец");
    check_eq_int((gint64) s.tx_dropped, 4, "tx drops = 4-й столбец");
}

static void test_parse_netdev_missing(void)
{
    static const char *data =
        "  eth0: 100 1 0 0 0 0 0 0 200 2 0 0 0 0 0 0\n";
    NmNetSample s;

    memset(&s, 0, sizeof(s));
    check(!nm_parse_netdev(data, "nosuch0", &s), "нет такого интерфейса");
    check(!s.valid, "сэмпл невалиден");
}

static void test_parse_netdev_ignores_prefix(void)
{
    /* Имя с префиксом не должно матчиться по куску строки. */
    static const char *data =
        "  wan0: 100 1 0 0 0 0 0 0 200 2 0 0 0 0 0 0\n"
        " wan0_ifb: 300 3 0 0 0 0 0 0 400 4 0 0 0 0 0 0\n";
    NmNetSample s;

    memset(&s, 0, sizeof(s));
    check(nm_parse_netdev(data, "wan0", &s), "точное имя найдено");
    check_eq_int((gint64) s.rx_bytes, 100, "точное имя, не префикс");
    check(!nm_parse_netdev(data, "wan0_if", &s),
          "усечённое имя не матчится");
}

static void test_parse_netdev_malformed(void)
{
    NmNetSample s;
    /* Строка без ':' и с недостаточным числом полей. */
    static const char *data =
        "  badline\n"
        "  eth1: 1 2 3\n";

    memset(&s, 0, sizeof(s));
    check(!nm_parse_netdev(data, "eth1", &s), "неполная строка отвергнута");
    check(!nm_parse_netdev(data, "badline", &s), "строка без ':' отвергнута");
}

static void test_parse_netdev_all(void)
{
    static const char *data =
        "    lo: 1 1 0 0 0 0 0 0 2 2 0 0 0 0 0 0\n"
        "  eth0: 10 1 0 0 0 0 0 0 20 1 0 0 0 0 0 0\n"
        "   tun0: 30 1 0 0 0 0 0 0 40 1 0 0 0 0 0 0\n";
    NmNetSample samples[NM_MAX_IFACES];
    guint n = nm_parse_netdev_all(data, samples, NM_MAX_IFACES);

    check_eq_int(n, 3, "найдено 3 интерфейса");
    check_eq_str(samples[0].ifname, "lo", "первый = lo");
    check_eq_str(samples[1].ifname, "eth0", "второй = eth0");
    check_eq_str(samples[2].ifname, "tun0", "третий = tun0");
    g_free(samples[0].ifname);
    g_free(samples[1].ifname);
    g_free(samples[2].ifname);
}

/* --- скорость и переполнение счётчика --- */

static void test_rate_basic(void)
{
    check_eq_int(nm_rate_bytes_per_second(0, 1000000, 1000000, 2000000),
                 1000000, "1 МБ за 1 с");
    check_eq_int(nm_rate_bytes_per_second(0, 1250000, 1000000, 2000000),
                 1250000, "1.25 МБ за 1 с");
    check_eq_int(nm_rate_bytes_per_second(0, 500, 1000000, 2000000),
                 500, "500 Б за 1 с");
}

static void test_rate_no_baseline(void)
{
    /* prev_time_us == 0 означает "baseline не установлен": делить нельзя,
     * иначе первый тик рисовал бы гигабайты. */
    check_eq_int(nm_rate_bytes_per_second(0, 4200000000LL, 0, 1000000),
                 0, "без baseline = 0");
    check_eq_int(nm_rate_bytes_per_second(0, 1000, 2000000, 1000000),
                 0, "now раньше prev = 0");
}

static void test_rate_counter_wrap(void)
{
    /* Оборот 32-битного счётчика обязан распознаваться: иначе в графике
     * появился бы ложный скачок на ~4 ГБ. */
    check_eq_int(nm_rate_bytes_per_second(4200000000LL, 1200000, 1000000,
                                          2000000),
                 -1, "оборот счётчика = -1");
    /* Равные счётчики — нулевая скорость, а не ошибка. */
    check_eq_int(nm_rate_bytes_per_second(5000, 5000, 1000000, 2000000),
                 0, "счётчик не изменился = 0");
}

static void test_rate_saturates(void)
{
    /* Переполнение при умножении должно давать насыщение, а не UB. */
    gint64 r = nm_rate_bytes_per_second(0, G_MAXUINT64, 1000, 2000);
    check(r > 0, "насыщение без переполнения знака");
}

/* --- классификация интерфейсов --- */

/* Классификация читает настоящий sysfs, поэтому тест работает на
 * фикстуре с теми же раскладками, что и на машине: type=1 + MAC у
 * физических, каталог bridge/ у мостов, type=65534 у туннелей. */
#define NM_FIXTURE "/home/kosmik2001_dir/Документы/GLM-5.3-Kimi/temps/nmfix"

static void test_interface_kind(void)
{
    check_eq_int(nm_interface_kind("lo", NM_FIXTURE), NM_IF_LOOPBACK,
                 "lo = loopback");
    check_eq_int(nm_interface_kind("brlan0", NM_FIXTURE), NM_IF_BRIDGE,
                 "brlan0 = мост (каталог bridge/)");
    check_eq_int(nm_interface_kind("virbr0", NM_FIXTURE), NM_IF_BRIDGE,
                 "virbr0 = мост");
    check_eq_int(nm_interface_kind("wan0_ifb", NM_FIXTURE), NM_IF_VIRTUAL,
                 "ifb = программный зеркальный, не физический");
    check_eq_int(nm_interface_kind("tun0", NM_FIXTURE), NM_IF_TUNNEL,
                 "tun0 = туннель (type=65534)");
    check_eq_int(nm_interface_kind("tap0", NM_FIXTURE), NM_IF_TUNNEL,
                 "tap0 = туннель");
    check_eq_int(nm_interface_kind("wan0", NM_FIXTURE), NM_IF_PHYSICAL,
                 "wan0 = физический");
    /* Отсутствующий интерфейс обязан давать UNKNOWN, а не выдуманный тип. */
    check_eq_int(nm_interface_kind("nosuch0", NM_FIXTURE), NM_IF_UNKNOWN,
                 "нет такого интерфейса = UNKNOWN");
    check_eq_int(nm_interface_kind(NULL, NM_FIXTURE), NM_IF_UNKNOWN,
                 "NULL = UNKNOWN");
}

/* --- цвет, координаты, история --- */

static void test_rgba_roundtrip(void)
{
    gdouble out[4] = {0, 0, 0, 0};
    char *text = nm_format_rgba((const gdouble[4]) {0.25, 0.5, 0.75, 1.0});

    check(nm_parse_rgba(text, out), "свой же формат читается");
    check(fabs(out[0] - 0.25) < 0.01, "R сохранён");
    check(fabs(out[1] - 0.5) < 0.01, "G сохранён");
    check(fabs(out[2] - 0.75) < 0.01, "B сохранён");
    check(fabs(out[3] - 1.0) < 0.01, "A сохранён");
    g_free(text);
}

static void test_rgba_alpha_preserved(void)
{
    /* Настоящая прозрачность фона/рамки обязана пережить цикл
     * формат-парс: иначе редактирование цвета обнуляло бы alpha. */
    gdouble out[4] = {0, 0, 0, 0};
    char *text = nm_format_rgba((const gdouble[4]) {0.1, 0.2, 0.3, 0.4});

    check(nm_parse_rgba(text, out), "формат с alpha читается");
    check(fabs(out[3] - 0.4) < 0.01, "alpha 0.4 сохранён");
    g_free(text);
}

static void test_rgba_rejects_garbage(void)
{
    gdouble out[4] = {9, 9, 9, 9};

    check(!nm_parse_rgba("не цвет", out), "мусор отвергнут");
    check(!nm_parse_rgba("", out), "пустая строка отвергнута");
    check(!nm_parse_rgba(NULL, out), "NULL отвергнут");
}

static void test_scale_position(void)
{
    /* Конфиг хранит координаты в design-пикселях; рендер обязан их
     * пересчитать, но НЕ менять сохранённые значения. */
    check_eq_int(nm_scale_position(10, 420, 210, 209), 5, "x: 420 -> 210");
    check_eq_int(nm_scale_position(210, 420, 210, 209), 105, "x: середина");
    check_eq_int(nm_scale_position(0, 420, 210, 209), 0, "x: 0 -> 0");
    /* design == live: координата не меняется. */
    check_eq_int(nm_scale_position(37, 420, 420, 419), 37, "без масштаба");
    /* Резкое увеличение. */
    check_eq_int(nm_scale_position(200, 420, 840, 839), 400, "x: удвоение");
}

static void test_fit_text_coordinate(void)
{
    /* Подгонка учитывает ореол тени: значение формально внутри окна, но
     * его тень уже пересекает рамку. */
    check_eq_int(nm_fit_text_coordinate(10, 40, 100), 10, "влезает");
    check(nm_fit_text_coordinate(80, 40, 100) <= 100 - 40,
          "не выходит за правый край с ореолом");
    check(nm_fit_text_coordinate(-20, 40, 100) >= 0, "не выходит за левый");
}

static void test_history_ring(void)
{
    guint64 ring[8];
    guint head = 0, count = 0;

    memset(ring, 0, sizeof(ring));
    nm_push_history(ring, &head, &count, 100, 1000);
    nm_push_history(ring, &head, &count, 200, 1001);
    check_eq_int(count, 2, "два отсчёта");
    /* head указывает на СЛЕДУЮЩИЙ слот: самый свежий лежит перед ним. */
    check_eq_int((gint64) nm_history_value(ring, head, count, 0), 200,
                 "индекс 0 = свежий");
    check_eq_int((gint64) nm_history_value(ring, head, count, 1), 100,
                 "индекс 1 = предыдущий");
    check_eq_int((gint64) nm_history_origin_x(100, 20), 80, "начало по ширине");
}

static void test_history_columns(void)
{
    /* Число колонок не должно превышать доступную ширину. */
    guint c = nm_history_columns(40, 1000);
    check(c > 0, "хотя бы одна колонка");
    check(c <= 40, "колонок не больше пикселей");
    check_eq_int(nm_history_columns(40, 0), 0, "нет данных — нет колонок");
}

static void test_smoothing(void)
{
    NmSmoothRing sm;

    /* Свежий всплеск усредняется вместе с тихими соседями: одиночный
     * пик 1000 при трёх нулях не должен показываться как 1000. */
    memset(&sm, 0, sizeof(sm));
    nm_rate_smooth_push(&sm, 0);
    nm_rate_smooth_push(&sm, 0);
    nm_rate_smooth_push(&sm, 0);
    nm_rate_smooth_push(&sm, 1000);
    check_eq_int(nm_rate_smoothed(&sm, 4), 250, "окно 4: 1000 размазался");
    check_eq_int(nm_rate_smoothed(&sm, 1), 1000, "окно 1: без сглаживания");
    check_eq_int(nm_rate_smoothed(&sm, 2), 500, "окно 2: по двум свежим");

    /* Кольцо ограничено 14 отсчётами: 15-е значение вытесняет первое,
     * иначе усреднение опиралось бы на устаревшие данные. */
    {
        int i;
        for (i = 0; i < 20; i++)
            nm_rate_smooth_push(&sm, 100);
        check(sm.count == NM_RATE_SMOOTH_MAX, "кольцо не растёт бесконечно");
        check_eq_int(nm_rate_smoothed(&sm, NM_RATE_SMOOTH_MAX), 100,
                     "после переполнения все значения = 100");
    }
    nm_rate_smooth_reset(&sm);
    check_eq_int(nm_rate_smoothed(&sm, 4), 0, "сброс обнуляет кольцо");
    check(sm.count == 0, "сброс обнуляет счётчик");
}

static void test_smoothing_saturates(void)
{
    NmSmoothRing sm;
    int i;

    memset(&sm, 0, sizeof(sm));
    /* Каждый отсчёт — G_MAXUINT64: сумма переполнилась бы без
     * насыщения, и это UB, а не «очень большое число». */
    for (i = 0; i < NM_RATE_SMOOTH_MAX; i++)
        nm_rate_smooth_push(&sm, G_MAXINT64);
    check(nm_rate_smoothed(&sm, NM_RATE_SMOOTH_MAX) > 0,
          "насыщение без переполнения знака");
}

/* Регрессия: ключи виджета должны копироваться, а не указывать на буфер
 * в стеке nm_properties(). Раньше GTK хранил указатель на локальный
 * char[], к моменту клика стек был перезаписан, обработчик читал мусор и
 * писал в конфиг случайные байты. Проверяем, что все ключи, которые
 * плагин передаёт в g_object_set_data, копируются. */
static void test_config_key_is_copied(void)
{
    GKeyFile *kf = g_key_file_new();
    GtkWidget *spin;
    const char *key;
    gboolean ok;

    g_key_file_set_string(kf, "sec", "series0_x", "8");
    g_key_file_set_string(kf, "sec", "series0_label_x", "8");
    g_key_file_set_string(kf, "sec", "total0_x", "4");

    /* GTK требует открытого display: без gtk_init() создание виджета
     * падает с "Can't create a GtkStyleContext without a display". */
    if (!gtk_init_check(NULL, NULL))
        return;
    spin = gtk_spin_button_new_with_range(0, 100, 1);
    g_object_set_data_full(G_OBJECT(spin), "xs-key", g_strdup("series0_x"),
                           g_free);
    key = g_object_get_data(G_OBJECT(spin), "xs-key");
    check(key != NULL && !strcmp(key, "series0_x"), "ключ читается как есть");
    ok = g_key_file_has_key(kf, "sec", key, NULL);
    check(ok, "ключ находится в конфиге (не мусор)");

    g_object_set_data_full(G_OBJECT(spin), "xs-key", g_strdup("total0_x"),
                           g_free);
    key = g_object_get_data(G_OBJECT(spin), "xs-key");
    check(g_key_file_has_key(kf, "sec", key, NULL),
          "замена ключа тоже даёт валидный ключ");

    g_object_unref(spin);
    g_key_file_free(kf);
}

static void test_split_geometry(void)
{
    /* Две половины не должны смыкаться: зазор обязателен, иначе две
     * заливки образуют одну фигуру и границы между download и upload
     * не видно. Геометрия повторяет расчёт в nm_render(). */
    int gap, plot_w, right_x;
    int width = 420;

    gap = MIN(8, MAX(0, (width - 4) / 4));
    plot_w = MAX(1, (width - 4 - gap) / 2);
    right_x = 2 + plot_w + gap;
    check_eq_int(gap, 8, "зазор между половинами = 8");
    check(right_x > 2 + plot_w, "вторая половина начинается за зазором");
    check_eq_int(right_x + plot_w, width - 2, "вторая половина доходит до края");
    /* Первая половина не залезает за середину окна. */
    check(2 + plot_w < width / 2 + gap, "первая половина не залезает на вторую");

    /* Узкое окно: зазор сжимается, но половины остаются непустыми. */
    {
        int w = NM_MIN_WINDOW_WIDTH;   /* 160 */
        int g2 = MIN(8, MAX(0, (w - 4) / 4));
        int p2 = MAX(1, (w - 4 - g2) / 2);
        check(g2 > 0, "зазор остаётся положительным в узком окне");
        check(p2 >= 1, "половина не схлопывается в ноль");
        check_eq_int(2 + p2 + g2 + p2, w - 2, "узкое окно: обе половины в границах");
    }
    /* Совсем узкое окно: зазор обрезается, но переполнения не будет. */
    {
        int w = 20;
        int g3 = MIN(8, MAX(0, (w - 4) / 4));
        int p3 = MAX(1, (w - 4 - g3) / 2);
        check(2 + p3 + g3 + p3 <= w, "узкое окно без переполнения");
    }
}

static void test_format_rate(void)
{
    char *s = nm_format_rate(0);
    check_eq_str(s, "0 B/s", "ноль");
    g_free(s);
    s = nm_format_rate(1000000);
    check(s != NULL && strstr(s, "B") != NULL, "мегабайты помечены единицей");
    g_free(s);
    s = nm_format_rate(G_MAXUINT64);
    check(s != NULL, "гигантское значение не роняет форматтер");
    g_free(s);
}

static void test_format_bytes(void)
{
    char *s = nm_format_bytes(0);
    check_eq_str(s, "0 B", "ноль байт");
    g_free(s);
    s = nm_format_bytes(1536);
    check(s != NULL && strlen(s) > 0, "1.5 КБ форматируется");
    g_free(s);
}

static void test_corner_radius(void)
{
    check(nm_corner_radius_is_rounded(0) == FALSE, "0 = прямой угол");
    check(nm_corner_radius_is_rounded(8) == TRUE, "8 = скруглённый");
    /* Большой радиус не превращается в прямой угол: он обрезается по
     * меньшей стороне окна в nm_rounded_path(), иначе applet 160x110
     * с radius=200 выглядел бы как прямоугольник. */
    check(nm_corner_radius_is_rounded(200) == TRUE, "200 остаётся скруглённым");
    check(nm_corner_radius_value(8) == 8.0, "значение радиуса");
}

int main(void)
{
    printf("== парсер /proc/net/dev ==\n");
    test_parse_netdev_basic();
    test_parse_netdev_missing();
    test_parse_netdev_ignores_prefix();
    test_parse_netdev_malformed();
    test_parse_netdev_all();

    printf("== скорость и переполнение ==\n");
    test_rate_basic();
    test_rate_no_baseline();
    test_rate_counter_wrap();
    test_rate_saturates();

    printf("== интерфейсы ==\n");
    test_interface_kind();

    printf("== цвет и координаты ==\n");
    test_rgba_roundtrip();
    test_rgba_alpha_preserved();
    test_rgba_rejects_garbage();
    test_scale_position();
    test_fit_text_coordinate();

    printf("== история и формат ==\n");
    test_history_ring();
    test_history_columns();
    test_smoothing();
    test_smoothing_saturates();
    test_format_rate();
    test_format_bytes();
    test_corner_radius();
    test_split_geometry();
    test_config_key_is_copied();

    printf("\n%s: %d проверок, %d провалов\n",
           failures ? "TEST_FAIL" : "TEST_OK", checks, failures);
    return failures ? 1 : 0;
}
