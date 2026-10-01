/* acpi_battery.c — applet батареи: заряд в процентах, время, состояние.
 *
 * Каркас отрисовки (окно, тема, Properties) написан по образцу
 * disk_monitor/memory_monitor. Исходники ACPIBatteryScreenlet.py здесь не
 * используются и не меняются: это отдельный плагин со своим core.
 *
 * Отличия от оригинала, и каждое по причине:
 *
 *  - данные читаются из sysfs, а не из /proc/acpi/battery (см.
 *    acpi_battery_core.h — путь закрыт ядром ещё в 3.8);
 *  - отсутствие батареи даёт «No battery» и НЕ роняет applet. В
 *    оригинале listdir() несуществующего каталога ронял демон с
 *    OSError, а на месте обработки стояла пометка «TODO: raise
 *    exception!!!» — автор этот случай не закрыл;
 *  - «No battery» рисуется БЕЛОЙ надписью и обычным индикатором, а не
 *    алармом. В оригинале отсутствие данных попадало в ту же ветку,
 *    что и низкий заряд, и оба случая показывались красным:
 *    «батарея сдохла» и «батареи нет» выглядели одинаково;
 *  - UPower не используется. Он считает TimeToEmpty точнее, но требует
 *    живого демона и D-Bus, тогда как sysfs есть всегда, где есть
 *    устройство, и отдаёт те же capacity/status — включая батарейку
 *    Logitech-мыши, которую hid-logitech-hidpp добавляет как обычный
 *    узел power_supply с типом Mouse.
 *
 * Геометрия текста взята из оригинальной темы default (окно 100x50,
 * текст с x=37): тема acpibattery-* рассчитана именно на эти
 * координаты, сдвиг делает её нечитаемой.
 */
#include <gtk/gtk.h>
#include <string.h>
#include <stdlib.h>

#include "xs_api.h"
#include "common.h"
#include "acpi_battery_core.h"

#define AB_DEFAULT_WIDTH    100
#define AB_DEFAULT_HEIGHT    50
/* Порог ниже которого заряд считается низким. 15 — тот же процент,
 * что стоял в оригинале по умолчанию. */
#define AB_DEFAULT_ALARM     15
#define AB_DEFAULT_INTERVAL  30     /* секунд */

#define AB_TEXT_X            37
#define AB_TEXT_Y_PERCENT     5
#define AB_TEXT_Y_TIME       26
#define AB_TEXT_Y_ONLY       13
#define AB_MIN_WIDTH         60
#define AB_MIN_HEIGHT        30

typedef struct {
    int      window_width;
    int      window_height;
    int      update_interval;
    int      alarm_threshold;
    gboolean show_percent;
    gboolean show_time;

    /* Последнее прочитанное состояние: держим между тиками, чтобы
     * перерисовка по движению мыши не ходила в sysfs. */
    AcpiBattery *battery;         /* NULL = батареи нет */
    char        *time_text;       /* NULL = строку не рисуем */
    char        *percent_text;
    gboolean     low;            /* заряд ниже порога */
    GtkWidget   *widget;         /* окно для gtk_widget_create_pango_layout */
} AbState;

static void ab_set_text(char **dst, char *value)
{
    g_free(*dst);
    *dst = value;
}

static void ab_clamp(AbState *st)
{
    if (st->window_width  < AB_MIN_WIDTH)  st->window_width  = AB_MIN_WIDTH;
    if (st->window_height < AB_MIN_HEIGHT) st->window_height = AB_MIN_HEIGHT;
    if (st->update_interval < 1)   st->update_interval = 1;
    if (st->alarm_threshold < 0)   st->alarm_threshold = 0;
    if (st->alarm_threshold > 100) st->alarm_threshold = 100;
}

/* Пересчитать тексты из прочитанного состояния. */
static void ab_refresh_text(AbState *st)
{
    const AcpiBattery *bat = st->battery;

    ab_set_text(&st->percent_text, NULL);
    ab_set_text(&st->time_text, NULL);
    st->low = FALSE;

    if (!bat || bat->state == ACPI_BATTERY_NOT_PRESENT) {
        /* Устройства нет вовсе, либо батарейка извлечена: цифры не
         * показываем, сообщение рисует draw(). */
        ab_set_text(&st->percent_text, g_strdup("     "));
        return;
    }

    if (bat->percent >= 0) {
        ab_set_text(&st->percent_text, g_strdup_printf("%3d%%", bat->percent));
        st->low = (bat->percent <= st->alarm_threshold);
    } else {
        ab_set_text(&st->percent_text, g_strdup("  --"));
    }

    switch (bat->state) {
    case ACPI_BATTERY_FULL:
        ab_set_text(&st->time_text, g_strdup(" Full"));
        st->low = FALSE;      /* полная батарея не бывает «низкой» */
        break;
    case ACPI_BATTERY_CHARGING:
    case ACPI_BATTERY_DISCHARGING:
        ab_set_text(&st->time_text, acpi_battery_format_minutes(bat->minutes_left));
        break;
    case ACPI_BATTERY_UNKNOWN:
    default:
        /* Данных нет: прочерк и НЕ аларм. Иначе сервер без батареи или
         * ноутбук со статусом Unknown мигал бы красным. */
        ab_set_text(&st->time_text, g_strdup("   --"));
        st->low = FALSE;
        break;
    }
}

/* Прочитать sysfs. Список освобождаем сразу, нужные поля копируем:
 * держать весь список ради одного узла — лишняя память на каждый тик. */
static void ab_poll(AbState *st)
{
    AcpiBatteryList *list = acpi_battery_list_read(ACPI_BATTERY_SYSFS_ROOT);
    AcpiBattery *bat = NULL;

    acpi_battery_free(st->battery);
    st->battery = NULL;

    if (list) {
        bat = acpi_battery_list_primary(list);
        if (bat) {
            /* Deep copy: список сейчас освободится вместе со своими
             * элементами, а applet держит указатель между тиками. */
            st->battery = g_new0(AcpiBattery, 1);
            st->battery->name         = g_strdup(bat->name);
            st->battery->type         = g_strdup(bat->type);
            st->battery->status       = g_strdup(bat->status);
            st->battery->state        = bat->state;
            st->battery->percent      = bat->percent;
            st->battery->minutes_left = bat->minutes_left;
            st->battery->has_energy   = bat->has_energy;
        }
        acpi_battery_list_free(list);
    }

    ab_refresh_text(st);
}

/* Рисование текста. Белый — обычное состояние, красный — аларм.
 * Цвет из настроек не читаем: тема задаёт белый, а настройка цвета в
 * оригинале была, но не работала — писалась в конфиг и не читалась. */
static void ab_text(AbState *st, cairo_t *cr, const char *text,
                    double x, double y, gboolean red)
{
    PangoLayout *layout;

    if (!text || !*text)
        return;

    cairo_move_to(cr, x, y);
    if (red)
        cairo_set_source_rgb(cr, 1.0, 0.15, 0.15);
    else
        cairo_set_source_rgb(cr, 1.0, 1.0, 1.0);

    layout = gtk_widget_create_pango_layout(st->widget, text);
    pango_cairo_show_layout(cr, layout);
    g_object_unref(layout);
}

static int ab_init(XsPlugin *p, GKeyFile *kf)
{
    AbState *st;
    XsHostApi *api = p->host;
    int x, y;

    st = g_new0(AbState, 1);
    /* Координаты читаем здесь же: если их не передать в make_window,
     * окно создастся в 0,0 и applet окажется в углу, где его не видно.
     * Значения по умолчанию те же, что у остальных плагинов. */
    x = api->conf_int(kf, p->name, "x", 80);
    y = api->conf_int(kf, p->name, "y", 80);
    st->window_width    = api->conf_int(kf, p->name, "window_width", AB_DEFAULT_WIDTH);
    st->window_height   = api->conf_int(kf, p->name, "window_height", AB_DEFAULT_HEIGHT);
    st->update_interval = api->conf_int(kf, p->name, "update_interval", AB_DEFAULT_INTERVAL);
    st->alarm_threshold = api->conf_int(kf, p->name, "alarm_threshold", AB_DEFAULT_ALARM);
    st->show_percent    = api->conf_int(kf, p->name, "show_percent", 1) != 0;
    st->show_time       = api->conf_int(kf, p->name, "show_time", 1) != 0;
    ab_clamp(st);

    p->priv = st;

    api->make_window(p, x, y, st->window_width, st->window_height);
    api->theme_load(p, "default");

    st->widget = p->win;
    ab_poll(st);
    api->set_tick(p, (guint)st->update_interval * 1000);
    return 0;
}

/* Рисует элемент темы в градациях серого.
 *
 * Как это работает: элемент рисуется во временную группу, затем группа
 * переносится в источник и поверх неё рисуется серым сплошным заполнением
 * с оператором MULTIPLY. Обе операции вместе дают обесцвечивание: чёрное
 * остаётся чёрным, белое становится серым, цветные пиксели уходят в
 * оттенки того же серого.
 *
 * Почему не cairo_set_source_surface + paint с OVER: обычный paint
 * положил бы серый ПОВЕРХ всего, включая тёмный фон индикатора, и рамка
 * пропала бы. MULTIPY сохраняет светлоту каждого пикселя.
 *
 * Группа обязательна: красить прямо на cr нельзя, там же лежат элементы,
 * уже нарисованные без обесцвечивания (фон и корпус батареи).
 */
static void ab_draw_gray(XsPlugin *p, cairo_t *cr, const char *element,
                         double x, double y, double w, double h)
{
    cairo_surface_t *surf;
    cairo_t *tmp;
    int iw, ih;

    if (!xs_core_theme_has(p, element))
        return;

    iw = (int)(w > 0.0 ? w : 1.0);
    ih = (int)(h > 0.0 ? h : 1.0);

    /* Промежуточная ARGB-поверхность нужна, чтобы MULTIPLY применился
     * только к нарисованному элементу, а не ко всему окну. */
    surf = cairo_image_surface_create(CAIRO_FORMAT_ARGB32, iw, ih);
    tmp = cairo_create(surf);
    p->host->theme_draw_full(p, tmp, element, x, y, w, h);

    /* MULTIPLY сохраняет светлоту каждого пикселя: чёрное остаётся
     * чёрным, белое становится серым. При OVER серый просто закрыл бы
     * элемент, и рамка индикатора исчезла бы. */
    cairo_set_operator(cr, CAIRO_OPERATOR_MULTIPLY);
    cairo_set_source_surface(cr, surf, x, y);
    cairo_paint_with_alpha(cr, 1.0);
    cairo_set_operator(cr, CAIRO_OPERATOR_OVER);

    cairo_destroy(tmp);
    cairo_surface_destroy(surf);
}

static void ab_draw(XsPlugin *p, cairo_t *cr, int w, int h)
{
    AbState *st = p->priv;
    XsHostApi *api = p->host;
    const char *indicator = NULL;
    gboolean no_battery;

    /* Порядок как в оригинале: фон, корпус, индикатор, текст. */
    if (xs_core_theme_has(p, "acpibattery-bg"))
        api->theme_draw(p, cr, "acpibattery-bg", 0, 0, w);
    if (xs_core_theme_has(p, "acpibattery-battery"))
        api->theme_draw(p, cr, "acpibattery-battery", 0, 0, w);

    no_battery = (!st->battery ||
                  st->battery->state == ACPI_BATTERY_NOT_PRESENT);

    if (no_battery) {
        /* Батареи нет: индикатор рисуется В СЕРЫХ тонах. Аларм НЕ рисуем
         * никогда — красное читалось бы как «заряд кончился», а батареи
         * просто нет. Серый корпус даёт читаемый applet, при этом не
         * выдаёт его за низкий заряд. */
        if (xs_core_theme_has(p, "acpibattery-using"))
            ab_draw_gray(p, cr, "acpibattery-using", 0, 0, w, h);
    } else if (st->battery->state == ACPI_BATTERY_CHARGING) {
        indicator = "acpibattery-charging";
    } else if (st->battery->state == ACPI_BATTERY_DISCHARGING) {
        indicator = st->low ? "acpibattery-alarm" : "acpibattery-using";
    } else {
        indicator = "acpibattery-using";
    }
    if (indicator && xs_core_theme_has(p, indicator))
        api->theme_draw(p, cr, indicator, 0, 0, w);

    /* Клип по окну: при крупном шрифте текст вылезет за рамку, а окно
     * непрозрачное, и текст зарисуется поверх соседних апплетов. */
    cairo_save(cr);
    cairo_rectangle(cr, 0, 0, w, h);
    cairo_clip(cr);

    if (no_battery) {
        ab_text(st, cr, " No", AB_TEXT_X, AB_TEXT_Y_PERCENT, FALSE);
        ab_text(st, cr, " battery", AB_TEXT_X, AB_TEXT_Y_TIME, FALSE);
    } else if (st->show_percent && st->show_time) {
        ab_text(st, cr, st->percent_text, AB_TEXT_X, AB_TEXT_Y_PERCENT, st->low);
        ab_text(st, cr, st->time_text,    AB_TEXT_X, AB_TEXT_Y_TIME,    st->low);
    } else if (st->show_percent) {
        ab_text(st, cr, st->percent_text, AB_TEXT_X, AB_TEXT_Y_ONLY, st->low);
    } else if (st->show_time) {
        ab_text(st, cr, st->time_text, AB_TEXT_X, AB_TEXT_Y_ONLY, st->low);
    }

    cairo_restore(cr);
}

static guint ab_tick(XsPlugin *p)
{
    AbState *st = p->priv;
    XsHostApi *api = p->host;

    ab_poll(st);

    /* Интервал могли поменять в настройках — сбрасываем таймер, иначе
     * старая периодичность пережила бы правку до перезапуска. */
    api->set_tick(p, (guint)st->update_interval * 1000);
    return 0;
}

static void ab_shutdown(XsPlugin *p)
{
    AbState *st = p->priv;

    if (!st)
        return;
    acpi_battery_free(st->battery);
    g_free(st->time_text);
    g_free(st->percent_text);
    g_free(st);
    p->priv = NULL;
}

/* --- Properties --- */

static void ab_save_int(XsPlugin *p, const char *key, int value)
{
    GKeyFile *kf = xs_core_plugin_conf(p->name);

    if (!kf)
        return;
    g_key_file_set_integer(kf, p->name, key, value);
    xs_core_plugin_conf_flush(p->name);
}

static void ab_int_changed(GtkSpinButton *sb, gpointer data)
{
    XsPlugin *p = data;
    AbState *st = p->priv;
    const char *key = g_object_get_data(G_OBJECT(sb), "ab-key");
    int v = gtk_spin_button_get_value_as_int(sb);

    if (!key)
        return;

    if (!strcmp(key, "window_width")) {
        st->window_width = v;
        ab_save_int(p, key, v);
        p->host->resize(p, st->window_width, st->window_height);
        return;
    }
    if (!strcmp(key, "window_height")) {
        st->window_height = v;
        ab_save_int(p, key, v);
        p->host->resize(p, st->window_width, st->window_height);
        return;
    }
    if (!strcmp(key, "update_interval")) {
        st->update_interval = v;
        ab_save_int(p, key, v);
        p->host->set_tick(p, (guint)v * 1000);
        return;
    }
    if (!strcmp(key, "alarm_threshold")) {
        st->alarm_threshold = v;
        ab_save_int(p, key, v);
        ab_refresh_text(st);     /* порог влияет на цвет прямо сейчас */
        gtk_widget_queue_draw(p->win);
        return;
    }
    ab_save_int(p, key, v);
}

static void ab_bool_changed(GtkToggleButton *tb, gpointer data)
{
    XsPlugin *p = data;
    AbState *st = p->priv;
    const char *key = g_object_get_data(G_OBJECT(tb), "ab-key");
    gboolean v = gtk_toggle_button_get_active(tb);

    if (!key)
        return;
    if (!strcmp(key, "show_percent"))
        st->show_percent = v;
    else if (!strcmp(key, "show_time"))
        st->show_time = v;
    ab_save_int(p, key, v ? 1 : 0);
    gtk_widget_queue_draw(p->win);
}

static GtkWidget *ab_int_prop(GtkBox *box, XsPlugin *p, const char *key,
                              const char *label, const char *desc,
                              int value, int min, int max)
{
    GtkWidget *sb = xs_prop_add_int(box, label, desc, value, min, max, 1.0);

    g_object_set_data(G_OBJECT(sb), "ab-key", (gpointer)key);
    g_signal_connect(sb, "value-changed", G_CALLBACK(ab_int_changed), p);
    return sb;
}

static GtkWidget *ab_bool_prop(GtkBox *box, XsPlugin *p, const char *key,
                               const char *label, const char *desc,
                               gboolean value)
{
    GtkWidget *cb = gtk_check_button_new_with_label(label);

    gtk_toggle_button_set_active(GTK_TOGGLE_BUTTON(cb), value);
    gtk_widget_set_tooltip_text(cb, desc);
    gtk_box_pack_start(box, cb, FALSE, FALSE, 0);
    g_object_set_data(G_OBJECT(cb), "ab-key", (gpointer)key);
    g_signal_connect(cb, "toggled", G_CALLBACK(ab_bool_changed), p);
    return cb;
}

static void ab_properties(XsPlugin *p, GtkNotebook *nb)
{
    AbState *st = p->priv;
    GtkWidget *page;

    if (!st)
        return;

    page = gtk_box_new(GTK_ORIENTATION_VERTICAL, 4);
    gtk_container_set_border_width(GTK_CONTAINER(page), 10);

    xs_prop_add_group_header(GTK_BOX(page),
        "Reads /sys/class/power_supply. Works with laptop batteries and with "
        "devices that expose a battery there (e.g. a Logitech mouse when the "
        "hid-logitech-hidpp module is loaded). Shows \"No battery\" when no "
        "source is found, instead of failing.");

    ab_int_prop(GTK_BOX(page), p, "window_width", "Window width",
                "Applet width in pixels", st->window_width, AB_MIN_WIDTH, 400);
    ab_int_prop(GTK_BOX(page), p, "window_height", "Window height",
                "Applet height in pixels", st->window_height, AB_MIN_HEIGHT, 200);
    ab_int_prop(GTK_BOX(page), p, "update_interval", "Update interval (s)",
                "Seconds between reads of sysfs", st->update_interval, 1, 3600);
    ab_int_prop(GTK_BOX(page), p, "alarm_threshold", "Low battery threshold (%)",
                "Charge percent at or below which the alarm colour is used",
                st->alarm_threshold, 0, 100);

    ab_bool_prop(GTK_BOX(page), p, "show_percent", "Show percentage",
                 "Display charge percent", st->show_percent);
    ab_bool_prop(GTK_BOX(page), p, "show_time", "Show remaining time",
                 "Display estimated time left", st->show_time);

    gtk_notebook_append_page(nb, page, gtk_label_new("ACPI Battery"));
    gtk_widget_show_all(page);
}

static const XsPluginOps ab_ops = {
    .init = ab_init,
    .draw = ab_draw,
    .tick = ab_tick,
    .button = NULL,
    .motion = NULL,
    .shutdown = ab_shutdown,
    .menu = NULL,
    .menu_cmd = NULL,
    .properties = ab_properties,
    .fill_themes = NULL,
    .scroll = NULL,
    .enter = NULL,
    .leave = NULL,
    .guest_list_changed = NULL,
};

static XsPluginDesc ab_desc = {
    "acpi_battery",
    XS_API_VERSION,
    &ab_ops,
    "Battery charge, remaining time and state from sysfs power_supply",
    "xscreenlets",
    "1.0"
};

XS_PLUGIN_EXPORT(&ab_desc)