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

/* Скругление окна не меньше скругления графика: иначе рамка окна срежет
 * скруглённые углы графика по диагонали. */
static void test_window_radius_floor(void)
{
    int corner = 12, win;

    win = MAX(8, corner);
    check(win >= corner, "окно не меньше графика при window=8, graph=12");
    win = MAX(16, corner);
    check(win >= corner, "окно не меньше графика при window=16, graph=12");
    win = MAX(4, corner);
    check(win >= corner, "кламп поднимает окно до скругления графика");
    check(MAX(0, corner) == corner, "скругление графика не изменяется клампом");
}

/* Отступы между окном и графиком. Считаются по реальной высоте шрифта
 * того элемента, который выбран «снаружи» (если таких несколько — по
 * наибольшей), плюс поля сверху и снизу. Модель повторяет nm_render(). */
#define T_MARGIN_TOP    2
#define T_MARGIN_BOTTOM 2
#define T_ROW_H_MIN     8

/* Сколько внешних элементов выбрано снаружи: header, серии, сводки. */
typedef struct {
    int hdr, s0, s1, t0, t1;
} TOutside;

static int t_band(const TOutside *o, int font_px)
{
    int row_h = 0;
    int top = 0, bot = 0;
    int h, gy, gh;

    if (font_px <= 0)
        font_px = T_ROW_H_MIN;
    if (o->hdr) row_h = MAX(row_h, font_px);
    if (o->s0) row_h = MAX(row_h, font_px);
    if (o->s1) row_h = MAX(row_h, font_px);
    if (o->t0) row_h = MAX(row_h, font_px);
    if (o->t1) row_h = MAX(row_h, font_px);
    if (row_h > 0) {
        row_h = MAX(row_h, T_ROW_H_MIN);
        top = row_h + T_MARGIN_TOP + T_MARGIN_BOTTOM;
        bot = top;
    }
    /* Нижний отступ есть только если сводки снаружи: они внизу. */
    if (!o->t0 && !o->t1)
        bot = 0;

    h = 160;
    gy = top;
    gh = MAX(1, h - gy - bot);
    if (gh < h / 3 && h - top - bot > 0) {
        int room = MAX(1, h / 3);
        int overflow = top + bot - (h - room);

        top = MAX(0, top - overflow);
        bot = MAX(0, bot - overflow);
        gy = top;
        gh = MAX(1, h - gy - bot);
    }
    return top + bot;
}

static void test_outside_margins(void)
{

    check(t_band(&(TOutside){0,0,0,0,0}, 13) == 0,
          "без внешних элементов отступов нет");
    check(t_band(&(TOutside){1,0,0,0,0}, 13) == 13 + 4,
          "заголовок снаружи: отступ = высота шрифта + поля");
    check(t_band(&(TOutside){0,0,0,1,1}, 13) == 2 * (13 + 4),
          "обе сводки снаружи: отступ есть и сверху, и снизу");
    check(t_band(&(TOutside){1,1,1,1,1}, 13) == 2 * (13 + 4),
          "все снаружи: отступ не растёт от их количества");

    /* Отступ считается по НАИБОЛЬШЕМУ шрифту из выбранных наружу. */
    check(t_band(&(TOutside){0,1,0,0,0}, 8) == 8 + 4,
          "мелкий шрифт даёт маленький верхний отступ");
    check(t_band(&(TOutside){0,1,0,0,0}, 20) == 20 + 4,
          "крупный шрифт даёт большой верхний отступ");
    check(t_band(&(TOutside){0,1,0,0,0}, 0) == T_ROW_H_MIN + 4,
          "шрифт нулевой высоты поднимается до минимума");
    check(t_band(&(TOutside){0,0,0,0,0}, 40) == 0,
          "крупный ВНУТРЕННИЙ шрифт отступ не создаёт");
}

/* Кламп отсечения: y абсолютный, а предел считается как clip_y + clip_h.
 *
 * Раньше nm_show_text получал один height и клампил y по нему как если
 * бы тот был пределом окна. Для нижней полосы y = 105 при height = 17
 * превращался в 13 — сводка улетала наверх. Регрессия фиксирует
 * правило: y живёт в [clip_y + R, clip_y + clip_h - 1 - R]. */
#define T_SHADOW 3

static int t_clamp(int y, int clip_y, int clip_h)
{
    if (clip_h > 0 && y > clip_y + clip_h - 1 - T_SHADOW)
        y = clip_y + clip_h - 1 - T_SHADOW;
    if (y < clip_y + T_SHADOW)
        y = clip_y + T_SHADOW;
    return y;
}

static void test_text_clip_offsets(void)
{
    /* Верхняя полоса: отсчёт от нуля. */
    check(t_clamp(2, 0, 17) == 3, "верх: тень снизу не даёт выйти за край");
    check(t_clamp(5, 0, 17) == 5, "верх: координата внутри полосы не трогается");
    check(t_clamp(30, 0, 17) == 13,
          "верх: слишком низкое y прижимается к низу полосы, не к верху окна");

    /* Нижняя полоса: отсчёт от graph_y. y = 105 при clip_y = 103. */
    /* 105 попадает в [106, 116], но тень сверху требует 106 — и это
     * начало нижней полосы, а НЕ верх окна. Прежний код давал 13. */
    check(t_clamp(105, 103, 17) == 106,
          "низ: y прижимается к началу нижней полосы, а не к верху окна");
    check(t_clamp(105, 0, 17) == 13,
          "низ без clip_y — старый неверный результат");
    check(t_clamp(120, 103, 17) == 116, "низ: клампится по clip_y + clip_h");
    check(t_clamp(100, 103, 17) == 106, "низ: нижний предел не ниже clip_y");

    /* График: клип по области графика, а не по полосе. */
    check(t_clamp(50, 17, 106) == 50, "график: внутри не трогается");
    check(t_clamp(200, 17, 106) == 119,
          "график: выход за низ графика даёт последний ряд");
}

/* Полосы считаются по ФАЗАМ, а не по элементам.
 *
 * Строка — это фаза (заголовок / подписи серий / сводки), а не
 * элемент. Элементы одной фазы делят строку и раздвигаются по X.
 * Раньше высота считалась по числу элементов: две сводки давали две
 * строки, и вторая уезжала ниже первой — строки выглядели разной
 * высоты при одном шрифте. */
typedef struct {
    int hdr_top, hdr_bot, s0_top, s0_bot, t0_top, t0_bot;
} TSide;

static void t_bands(const TSide *s, int font_px, int *top, int *bot)
{
    int th = 0, bh = 0, tr = 0, br = 0;
    int totals_top = 0, totals_bottom = 0;

    if (font_px <= 0)
        font_px = T_ROW_H_MIN;
    if (s->hdr_top) { th = MAX(th, font_px); tr++; }
    if (s->hdr_bot) { bh = MAX(bh, font_px); br++; }
    /* Подписи серий — одна фаза: Down и Up делят строку и раздвигаются
     * по X. Раньше каждая серия увеличивала счётчик строк сама по себе,
     * и две подписи давали две строки — вторая пустовала, а полоса была
     * выше нужной. */
    if (s->s0_top)  { th = MAX(th, font_px); tr++; }
    if (s->s0_bot)  { bh = MAX(bh, font_px); br++; }
    /* Сводки — своя фаза: плюс ОДНА строка, но не по числу сводок.
     * Раньше стояло «занять строку, только если полоса пуста», из-за
     * чего подпись снизу + сводки снизу давали одну строку вместо двух и
     * нижний текст уезжал за границу окна. */
    if (s->t0_top)  { th = MAX(th, font_px); totals_top = 1; }
    if (s->t0_bot)  { bh = MAX(bh, font_px); totals_bottom = 1; }
    tr += totals_top;
    br += totals_bottom;
    th = MAX(th, T_ROW_H_MIN);
    bh = MAX(bh, T_ROW_H_MIN);
    *top = tr ? th * tr + T_MARGIN_TOP + T_MARGIN_BOTTOM : 0;
    *bot = br ? bh * br + T_MARGIN_TOP + T_MARGIN_BOTTOM : 0;
}

/* Номер строки элемента по фазе. */
static int t_slot(int phase, int hdr_in_band, int lbl_in_band)
{
    /* Заголовок (фаза 0) сдвигает всё, что ниже. Подписи серий (фаза 1)
     * сдвигают только сводки (фаза 2). Своя фаза не сдвигает себя. */
    if (phase > 0 && hdr_in_band)
        return 1;
    if (phase > 1 && lbl_in_band)
        return 1;
    return 0;
}

static void test_independent_bands(void)
{
    int top = -1, bot = -1;

    t_bands(&(TSide){0,0,0,0,0,0}, 13, &top, &bot);
    check(top == 0 && bot == 0, "все внутри: полос нет");

    t_bands(&(TSide){1,0,0,0,0,0}, 13, &top, &bot);
    check(top == 13 + 4 && bot == 0, "заголовок сверху: полоса только сверху");

    t_bands(&(TSide){0,0,0,0,0,1}, 13, &top, &bot);
    check(top == 0 && bot == 13 + 4, "сводка снизу: полоса только снизу");

    /* Регрессия: подпись серии снизу + сводки снизу = ДВЕ строки. */
    t_bands(&(TSide){0,0,0,1,0,1}, 13, &top, &bot);
    check(bot == 13 * 2 + 4, "подпись и сводки снизу занимают две строки");
    check(bot == 30, "две строки снизу дают полосу 30");

    t_bands(&(TSide){1,0,1,0,0,1}, 13, &top, &bot);
    check(top == 13 * 2 + 4 && bot == 13 + 4,
          "элементы сверху и снизу: обе полосы по своим элементам");
    check(top == 30, "заголовок и подписи сверху = две строки");

    t_bands(&(TSide){1,0,0,0,0,0}, 30, &top, &bot);
    check(top == 30 + 4, "крупный шрифт наверху увеличивает верхнюю полосу");
    check(bot == 0, "крупный шрифт наверху не трогает нижнюю");

    /* Две сводки снизу — одна строка, а не две. Иначе Total up уезжает
     * на строку ниже Total down при одинаковом шрифте. */
    t_bands(&(TSide){0,0,0,0,1,1}, 13, &top, &bot);
    check(bot == 13 + 4, "две сводки снизу делят одну строку");
    t_bands(&(TSide){1,1,0,0,1,1}, 13, &top, &bot);
    check(top == 13 * 2 + 4 && bot == 13 * 2 + 4,
          "заголовок плюс сводки в одной полосе = две строки");

    /* Фазы: заголовок / подписи / сводки. Каждая — одна строка, сколько
     * бы элементов в ней ни стояло. */
    t_bands(&(TSide){1,0,0,0,0,0}, 13, &top, &bot);
    t_bands(&(TSide){1,0,1,1,0,0}, 13, &top, &bot);
    check(top == 13 * 2 + 4,
          "заголовок и подписи сверху = две строки, не три");
    t_bands(&(TSide){1,0,0,0,1,1}, 13, &top, &bot);
    check(top == 13 * 2 + 4,
          "заголовок и сводки сверху = две строки, не три");
    t_bands(&(TSide){1,1,0,1,0,0}, 13, &top, &bot);
    check(bot == 13 * 2 + 4,
          "заголовок и подписи снизу = две строки");
    t_bands(&(TSide){0,0,1,1,0,0}, 13, &top, &bot);
    check(top == 13 + 4 && bot == 13 + 4,
          "только подписи: по одной строке в каждой полосе");
    check(t_slot(2, 0, 0) == 0, "фаза 2 без соседей — строка 0");
    check(t_slot(1, 0, 0) == 0, "фаза 1 без соседей — строка 0");
    check(t_slot(2, 0, 1) == 1, "подписи серий сдвигают сводки на строку вниз");
    check(t_slot(2, 1, 1) == 1,
          "и заголовок, и подписи всё равно дают одну строку сдвига");
    check(t_slot(0, 1, 1) == 0, "заголовок всегда первая строка");
    check(t_slot(1, 0, 1) == 0,
          "подписи серий не сдвигают сами себя — это их фаза");
    check(t_slot(2, 0, 1) == 1, "подписи серий сдвигают сводки на строку");
    check(t_slot(0, 0, 1) == 0,
          "заголовок не сдвигается от подписей, стоящих ниже");
}

typedef enum {
    T_INSIDE = 0,
    T_OUT_TOP,
    T_OUT_BOTTOM,
} TPlacement;

static TPlacement t_from_combo(int active)
{
    if (active == 1)
        return T_OUT_TOP;
    if (active == 2)
        return T_OUT_BOTTOM;
    return T_INSIDE;
}

static void test_placement_combo_mapping(void)
{
    check(t_from_combo(0) == T_INSIDE, "комбо 0 — внутри");
    check(t_from_combo(1) == T_OUT_TOP, "комбо 1 — снаружи сверху");
    check(t_from_combo(2) == T_OUT_BOTTOM, "комбо 2 — снаружи снизу");
    check(t_from_combo(-1) == T_INSIDE, "комбо без выбора — внутри");

    /* Все три ключа используют один маппинг, значит «снизу» доезжает
     * и до сводок, и до подписей, и до заголовка. */
    check(t_from_combo(2) != t_from_combo(1),
          "снизу и сверху — разные значения, не схлопываются в одно");
    check(t_from_combo(2) != t_from_combo(0),
          "снизу не превращается во внутри");
}

/* Подпись и значение — разные элементы с разными координатами.
 *
 * Раньше число рисовалось на «конец подписи + 6», то есть ездило
 * вместе с ней. Отсюда две жалобы: Label pos. двигал всю строку, а
 * Position не двигал ничего. Правило: число стоит там, где сказано
 * series<N>_x/y, но не левее конца подписи плюс зазор. */
#define T_GAP 6

static int t_value_x(int label_x, int label_w, int value_x)
{
    int min_x = label_x + label_w + T_GAP;

    return value_x > min_x ? value_x : min_x;
}

static void test_label_and_value_positions(void)
{
    /* Координаты разведены: каждая на своей. */
    check(t_value_x(6, 30, 120) == 120, "число правее подписи: своя координата");
    check(t_value_x(6, 30, 60) == 60, "число между подписью и краем: своя");
    /* 156 + 22 + 6 = 184 — значение упирается в зазор, как и у первой. */
    check(t_value_x(156, 22, 180) == 184, "вторая серия: то же правило");
    check(t_value_x(156, 22, 190) == 190, "вторая серия: своя координата");

    /* Координаты сведены: число не наезжает на подпись. */
    check(t_value_x(30, 30, 7) == 66,
          "число левее подписи сдвигается за её конец, не наезжает");
    check(t_value_x(6, 30, 40) == 42, "зазор ровно в NM_VALUE_GAP");
    check(t_value_x(6, 30, 6) == 42, "число на координате подписи — тоже зазор");
    check(t_value_x(6, 30, 36) == 42, "меньше зазора — ровно зазор");
    check(t_value_x(6, 30, 39) == 42, "на пиксель меньше зазора — тот же");
    check(t_value_x(6, 30, 43) == 43, "больше зазора — не трогаем");

    /* Подпись нулевой ширины: число ровно на своей координате. */
    check(t_value_x(10, 0, 20) == 20, "пустая подпись не сдвигает число");
    check(t_value_x(10, 0, 12) == 16, "даже пустая даёт зазор");
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

static void test_format_label(void)
{
    char *r;

    /* Обычный случай: единственный %s подставляется. */
    r = nm_format_label("Total: %s", "1.5 MiB");
    check(r && !strcmp(r, "Total: 1.5 MiB"), "тот же %s");
    g_free(r);

    /* Нет спецификатора: значение дописывается, а не теряется. */
    r = nm_format_label("Total", "1.5 MiB");
    check(r && !strcmp(r, "Total: 1.5 MiB"), "формата без %s");
    g_free(r);

    /* Ревью нашло: «Total %d: %s» печатал мусор из стека. */
    r = nm_format_label("Total %d: %s", "1.5 MiB");
    check(r && strstr(r, "1.5 MiB") != NULL, "%%d не роняет значение");
    check(r && strstr(r, "Total %d: %s") != NULL, "%%d остаётся в тексте");
    g_free(r);

    r = nm_format_label("100% done: %s", "1.5 MiB");
    check(r && strstr(r, "1.5 MiB") != NULL, "процент перед %s");
    g_free(r);

    /* Хвостовой процент без спецификатора. */
    r = nm_format_label("итого %", "1.5 MiB");
    check(r && strstr(r, "1.5 MiB") != NULL, "хвостовой процент");
    g_free(r);

    /* Пустой формат. */
    r = nm_format_label("", "1.5 MiB");
    check(r && !strcmp(r, "1.5 MiB"), "пустой формат");
    g_free(r);
    r = nm_format_label(NULL, "1.5 MiB");
    check(r && !strcmp(r, "1.5 MiB"), "NULL-формат");
    g_free(r);
}

static void test_parse_rgba_formats(void)
{
    gdouble c[4];

    /* Формат из живого конфига и из примера. Ревью нашло, что он не
     * разбирался вообще: nm_read_color молча брал дефолт, и все
     * настройки цвета были мёртвыми. */
    check(nm_parse_rgba("rgba(51,191,255,255)", c), "rgba() разбирается");
    check(fabs(c[0] - 51.0 / 255.0) < 1e-6, "rgba() R = 51/255");
    check(fabs(c[1] - 191.0 / 255.0) < 1e-6, "rgba() G = 191/255");
    check(fabs(c[2] - 255.0 / 255.0) < 1e-6, "rgba() B = 255/255");
    check(fabs(c[3] - 1.0) < 1e-6, "rgba() A = 1.0");

    /* Старый формат, который пишет nm_format_rgba: 0..1 без скобок. */
    check(nm_parse_rgba("0.2,0.75,1.0,1.0", c), "старый формат жив");
    check(fabs(c[0] - 0.2) < 1e-6, "старый R = 0.2");
    check(fabs(c[3] - 1.0) < 1e-6, "старый A = 1.0");

    /* Пробелы и регистр. */
    check(nm_parse_rgba("  rgba( 0 , 128 , 0 , 255 ) ", c), "пробелы в rgba()");
    check(fabs(c[1] - 128.0 / 255.0) < 1e-6, "пробелы: G = 128/255");
    check(nm_parse_rgba("RGBA(0,0,0,255)", c), "регистр не важен");
    check(nm_parse_rgba("rgba (0,0,0,255)", c), "пробел перед скобкой");

    /* rgb() без альфы. */
    check(nm_parse_rgba("rgb(255,0,0)", c), "rgb() без альфы");
    check(fabs(c[0] - 1.0) < 1e-6, "rgb() R = 1.0");
    check(fabs(c[3] - 1.0) < 1e-6, "rgb() A = 1.0 по умолчанию");

    /* Мусор и выход за диапазон по-прежнему отвергаются. */
    check(!nm_parse_rgba("rgba(300,0,0,255)", c), "канал > 255");
    check(nm_parse_rgba("rgba(0,0,0)", c), "rgba() без альфы");
    check(fabs(c[3] - 1.0) < 1e-6, "rgba() без альфы даёт A = 1.0");
    check(!nm_parse_rgba("rgba(0,0,0,255", c), "незакрытая скобка");
    check(!nm_parse_rgba("rgba 0,0,0,255", c), "нет скобки");
    check(!nm_parse_rgba("hello", c), "мусор");
    check(!nm_parse_rgba("", c), "пустая строка");
    check(!nm_parse_rgba(NULL, c), "NULL");
    check(!nm_parse_rgba("2.0,0,0,1.0", c), "старое значение > 1");
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
    test_outside_margins();
    test_text_clip_offsets();
    test_independent_bands();
    test_placement_combo_mapping();
    test_label_and_value_positions();
    test_format_label();
    test_parse_rgba_formats();
    test_split_geometry();
    test_window_radius_floor();
    test_config_key_is_copied();

    printf("\n%s: %d проверок, %d провалов\n",
           failures ? "TEST_FAIL" : "TEST_OK", checks, failures);
    return failures ? 1 : 0;
}
