/* acpi_battery_core.c — чтение /sys/class/power_supply.
 *
 * Формат sysfs однозначен: в каждом файле одно число без единиц и без
 * знака, состояние — одно слово. Разбирать нечего, и ловить нечего —
 * в отличие от вывода acpi(1) или файлов /proc/acpi/battery/info, где
 * оригинал искал число перебором split(' ') до isdigit().
 */
#include "acpi_battery_core.h"

#include <string.h>
#include <stdlib.h>

/* Прочитать целое из файла. -1 при любой неудаче: отсутствии файла,
 * мусоре в нём или недостатке прав. Отличать эти случаи отдельно не
 * надо — все три означают «значения нет». */
static gint64 read_int_file(const char *dir, const char *fname, gboolean *ok)
{
    char *path, *text = NULL;
    gint64 value = -1;
    GError *err = NULL;

    *ok = FALSE;
    path = g_build_filename(dir, fname, NULL);
    if (g_file_get_contents(path, &text, NULL, &err)) {
        /* sysfs отдаёт «87\n» — хвостовой перевод строки обязателен,
         * g_ascii_strtoll64 его не примет. Могут быть и ведущие пробелы. */
        gchar *end = NULL;
        value = g_ascii_strtoll(text, &end, 10);
        if (end == text)
            value = -1;              /* мусор вместо числа */
        else
            *ok = TRUE;
        g_free(text);
    }
    if (err)
        g_error_free(err);
    g_free(path);
    return value;
}

/* Прочитать строку и обрезать пробелы. */
static char *read_str_file(const char *dir, const char *fname)
{
    char *path, *text = NULL, *trimmed = NULL;
    GError *err = NULL;

    path = g_build_filename(dir, fname, NULL);
    if (g_file_get_contents(path, &text, NULL, &err)) {
        /* Многострочные значения в sysfs не встречаются, но status
         * теоретически может вернуть пустую строку — g_strchomp на
         * пустой строке безопасен. */
        trimmed = g_strdup(g_strchomp(text));
        g_free(text);
    }
    if (err)
        g_error_free(err);
    g_free(path);
    return trimmed;
}

AcpiBatteryState acpi_battery_parse_status(const char *text)
{
    /* Регистр плавает между версиями ядра, а драйверы для HID-устройств
     * пишут иначе, чем для ACPI-батареи, поэтому сравнение без учёта
     * регистра — не перестраховка, а необходимость. */
    if (!text || !*text)
        return ACPI_BATTERY_UNKNOWN;

    if (g_ascii_strcasecmp(text, "Charging") == 0)
        return ACPI_BATTERY_CHARGING;
    if (g_ascii_strcasecmp(text, "Discharging") == 0)
        return ACPI_BATTERY_DISCHARGING;
    if (g_ascii_strcasecmp(text, "Full") == 0 ||
        g_ascii_strcasecmp(text, "Fully charged") == 0 ||
        g_ascii_strcasecmp(text, "Charge complete") == 0)
        return ACPI_BATTERY_FULL;
    if (g_ascii_strcasecmp(text, "Not charging") == 0)
        return ACPI_BATTERY_PENDING_CHARGE;

    /* "Unknown" и всё прочее — данных нет. */
    return ACPI_BATTERY_UNKNOWN;
}

/* Порог, выше которого состояние принудительно считается «заряжена».
 *
 * Не 100 %, а 90: UPower использует именно 90 (UP_FULLY_CHARGED_THRESHOLD),
 * и это не щедрость. Реальные батареи подходят к 100 очень долго, а
 * система давно перестаёт их заряжать — 95 % это уже «заряжена» по любым
 * ощущениям пользователя. Совпадение с UPower обязательно: иначе applet
 * покажет аларм на батарее, для которой GNOME и MATE уведомлений не
 * показывают. */
#define ACPI_BATTERY_FULLY_CHARGED_PCT 90

/* Аппроксимация capacity_level в проценты. Только для случая capacity==0.
 * Ядро отдаёт пять уровней; точные проценты в нём не заданы, поэтому
 * берём середины диапазонов, а не края: Unknown — это середина, а не 0,
 * иначе applet покажет аларм на батарее, уровень которой драйвер просто
 * не смог определить. */
static gint acpi_battery_level_to_percent(const char *level)
{
    if (!level)
        return -1;
    if (g_ascii_strcasecmp(level, "Full") == 0)
        return 100;
    if (g_ascii_strcasecmp(level, "High") == 0)
        return 70;
    if (g_ascii_strcasecmp(level, "Normal") == 0)
        return 55;
    if (g_ascii_strcasecmp(level, "Low") == 0)
        return 10;
    if (g_ascii_strcasecmp(level, "Critical") == 0)
        return 1;
    /* "Unknown" и всё прочее — данных нет. Возврат -1 обязателен:
     * подстановка 0 дала бы «заряд кончился» вместо «не знаю». */
    return -1;
}

/* Quirk-правила, скопированные из UPower (up-device-supply.c,
 * up-device-battery.c). Без них applet расходится с системными иконками
 * и уведомлениями: пользователь увидит «заряжается» для батареи, которая
 * разряжается, и аларм на полностью заряженной.
 *
 * Порядок правил значим и повторяет порядок UPower.
 */
static void apply_battery_quirks(AcpiBattery *bat)
{
    gboolean full_charge_claimed;

    if (bat->state == ACPI_BATTERY_NOT_PRESENT)
        return;

    /* capacity == 0 значит «драйвер не смог посчитать», а не «заряд
     * кончился»: ядро само пишет 0 при unavailable. Пробуем уровень. */
    if (bat->percent == 0) {
        gint from_level = acpi_battery_level_to_percent(bat->capacity_level);
        if (from_level >= 0)
            bat->percent = from_level;
        else
            bat->percent = -1;
    }

    /* Ток и статус могут врать друг другу. Отрицательный current_now при
     * статусе «charging» — известный случай в драйверах axp20x: разряд
     * отдаётся как отрицательный ток, а status остаётся charging. */
    if (bat->state == ACPI_BATTERY_CHARGING && bat->current_now < 0)
        bat->state = ACPI_BATTERY_DISCHARGING;

    /* Not charging при заряде ниже 90 % — это НЕ «заряжена»: сеть
     * подключена, зарядка не идёт (порог заряда в BIOS, полная батарея
     * при нулевом токе), а батарея при этом всё равно разряжается. В
     * applet показываем время до разряда, индикатор — «using».
     *
     * Проверка идёт ДО общего порога 90 %, иначе батарея с любым
     * состоянием при высоком заряде объявлялась бы полной, и до сюда
     * дело не дошло бы. */
    if (bat->state == ACPI_BATTERY_PENDING_CHARGE) {
        /* При >=90 % батарея уже полна, и Not charging тут означает
         * «зарядка не идёт по причине, а не разряд». Часть устройств
         * вообще вечно держит PENDING_CHARGE, поэтому UPower и здесь
         * смотрит на процент. */
        bat->state = (bat->percent >= ACPI_BATTERY_FULLY_CHARGED_PCT)
                   ? ACPI_BATTERY_FULL
                   : ACPI_BATTERY_DISCHARGING;
        return;
    }

    /* «Заряжена» определяем по порогу, а не по status: батареи очень часто
     * пишут discharging даже при полном заряде (зарядка отключилась по
     * порогу заряда в BIOS), и UPower принудительно показывает Full.
     *
     * НО порог 90 % нельзя применять при разряде. Раньше здесь стояло
     * просто percent >= 90, и ноутбук на 90 % показывал "Full" вместо
     * времени: батарея отключена от сетки, status=Discharging, заряд
     * тает - а applet писал, что батарея полная. Порог означает «вот
     * скоро полностью», а не «заряда нет».
     *
     * Поэтому: 100 % - всегда Full, а 90..99 % - только когда разряда
     * фактически нет. Ток > 0 означает, что батарея отдаёт энергию, и
     * время до разряда полезнее, чем слово Full. */
    if (bat->percent >= 100) {
        bat->state = ACPI_BATTERY_FULL;
        return;
    }
    if (bat->percent < ACPI_BATTERY_FULLY_CHARGED_PCT)
        return;

    /* Разряда фактически нет в двух случаях: батарея не пишет
     * Discharging, либо ток не положительный. Второе условие важно для
     * батарей без файла current_now: там current_now == -1 со
     * смыслом "файла нет" (см. battery_read), и это НЕ повод объявлять
     * разряд - иначе applet начал бы считать время у батареи, которая
     * на самом деле стоит на зарядке.
     *
     * Что отсекается: status=Discharging при current_now > 0. Это ровно
     * тот случай, где 90 % означало "Full" вместо времени. */
    full_charge_claimed = (bat->state != ACPI_BATTERY_DISCHARGING ||
                           bat->current_now <= 0);
    if (full_charge_claimed)
        bat->state = ACPI_BATTERY_FULL;
}

/* --- чтение каталога --- */

static void battery_read(AcpiBattery *bat, const char *dir)
{
    gboolean ok_cap, ok_now, ok_full, ok_pow, ok_cur;
    gint64 cap, now, full, pow, cur;
    char *present;

    present = read_str_file(dir, "present");
    if (present && g_ascii_strcasecmp(present, "no") == 0)
        bat->state = ACPI_BATTERY_NOT_PRESENT;
    g_free(present);

    bat->status = read_str_file(dir, "status");
    if (bat->state != ACPI_BATTERY_NOT_PRESENT)
        bat->state = acpi_battery_parse_status(bat->status);
    g_free(bat->type);
    bat->type = read_str_file(dir, "type");

    cap  = read_int_file(dir, "capacity", &ok_cap);
    now  = read_int_file(dir, "energy_now", &ok_now);
    full = read_int_file(dir, "energy_full", &ok_full);
    pow  = read_int_file(dir, "power_now", &ok_pow);
    /* current_now читаем ДО проверки на >=0: отрицательное значение здесь
     * значимо, это признак разряда при статусе charging. */
    cur  = read_int_file(dir, "current_now", &ok_cur);

    if (ok_cap && cap >= 0) {
        bat->percent = (gint)cap;
        /* Драйверы иногда отдают 255 как «неизвестно», а не -1. */
        if (bat->percent > 100)
            bat->percent = -1;
    } else {
        bat->percent = -1;
    }

    if (ok_now && now >= 0)
        bat->energy_now = now;
    if (ok_full && full >= 0)
        bat->energy_full = full;
    if (ok_pow && pow > 0)
        bat->power_now = pow;
    if (ok_cur)
        bat->current_now = cur;

    g_free(bat->capacity_level);
    bat->capacity_level = read_str_file(dir, "capacity_level");

    apply_battery_quirks(bat);

    /* Время считаем ПОСЛЕ quirk: состояние уже приведено к истинному, и
     * для «заряжена» время не считается вовсе. */
    bat->has_energy = (bat->energy_now >= 0 && bat->energy_full >= 0);
    bat->minutes_left = -1;
    if (bat->state == ACPI_BATTERY_CHARGING ||
        bat->state == ACPI_BATTERY_DISCHARGING) {
        if (bat->has_energy && bat->energy_full > 0) {
            gint64 power = bat->power_now > 0 ? bat->power_now
                                               : -bat->current_now;
            if (power > 0) {
                if (bat->state == ACPI_BATTERY_CHARGING)
                    bat->minutes_left = (gint)((bat->energy_full
                                                - bat->energy_now) * 60 / power);
                else
                    bat->minutes_left = (gint)(bat->energy_now * 60 / power);
                if (bat->minutes_left < 0)
                    bat->minutes_left = -1;
            }
        }
    }
}

AcpiBatteryList *acpi_battery_list_read(const char *root)
{
    AcpiBatteryList *list;
    GDir *dir;
    const char *entry;

    if (!root)
        root = ACPI_BATTERY_SYSFS_ROOT;

    dir = g_dir_open(root, 0, NULL);
    if (!dir)
        return NULL;   /* нет sysfs-каталога вообще */

    list = g_new0(AcpiBatteryList, 1);
    list->items = g_ptr_array_new_with_free_func(
        (GDestroyNotify)acpi_battery_free);

    while ((entry = g_dir_read_name(dir)) != NULL) {
        char *path = g_build_filename(root, entry, NULL);
        AcpiBattery *bat;

        /* Каталог источника обязателен, а не файл: некоторые драйверы
         * кладут рядом свои файлы и в другие подкаталоги. */
        if (!g_file_test(path, G_FILE_TEST_IS_DIR)) {
            g_free(path);
            continue;
        }
        g_free(path);

        bat = g_new0(AcpiBattery, 1);
        bat->name = g_strdup(entry);
        bat->percent = -1;
        bat->energy_now = -1;
        bat->energy_full = -1;
        bat->power_now = -1;
        bat->minutes_left = -1;
        /* current_now = -1 означает «файла нет», а отрицательное
         * значение — реальный разряд. Различать обязательно: иначе
         * quirk «отрицательный ток значит разряд» срабатывал бы на
         * каждой батарее без файла current_now. */
        bat->current_now = -1;

        {
            char *d = g_build_filename(root, entry, NULL);
            battery_read(bat, d);
            g_free(d);
        }

        /* Источник без type — не батарея (в каталоге бывают device-model
         * каталоги вроде «power supplies», «i2c-dev»). */
        if (bat->type && g_ascii_strcasecmp(bat->type, "Battery") == 0) {
            g_ptr_array_add(list->items, bat);
        } else if (bat->type && (g_ascii_strcasecmp(bat->type, "Mouse") == 0 ||
                                 g_ascii_strcasecmp(bat->type, "Bluetooth") == 0)) {
            /* Мышь/гарнитура с батарейкой: тип не Battery, но заряд
             * показывать есть что. Такие появляются только с модулем
             * hid-logitech-hidpp и подобными. */
            g_ptr_array_add(list->items, bat);
        } else {
            acpi_battery_free(bat);
        }
    }
    g_dir_close(dir);
    return list;
}

AcpiBattery *acpi_battery_list_primary(const AcpiBatteryList *list)
{
    guint i;

    if (!list || !list->items)
        return NULL;

    /* Приоритет: батарея с ИЗВЕСТНЫМ зарядом, затем настоящая батарея,
     * потом устройство с батарейкой (мышь/гарнитура).
     *
     * Одной сортировки по типу мало. На ноутбуке с hid-logitech-hidpp
     * в /sys/class/power_supply лежат ДВА узла с type=Battery: настоящая
     * BAT0 и hidpp_battery_0 (батарейка мыши). Порядок readdir() не
     * задан, и на практике hidpp_battery_0 приходил ПЕРВЫМ. Он тоже
     * type=Battery, поэтому проверка «тип == Battery» его пропускала, и
     * апплет показывал NA, хотя BAT0 рядом имел capacity=100.
     *
     * Порядок sysfs воспроизводим между сессиями, но не между
     * загрузками, поэтому полагаться на него нельзя. */
    {
        AcpiBattery *fallback = NULL;

        for (i = 0; i < list->items->len; i++) {
            AcpiBattery *bat = g_ptr_array_index(list->items, i);

            if (!bat->type ||
                g_ascii_strcasecmp(bat->type, "Battery") != 0)
                continue;

            /* Заряд известен - это то, что нужно показать. */
            if (bat->percent >= 0)
                return bat;

            /* Иначе запоминаем и продолжаем искать: вдруг дальше есть
             * настоящая батарея с capacity. */
            if (!fallback)
                fallback = bat;
        }
        if (fallback)
            return fallback;
    }
    return list->items->len ? g_ptr_array_index(list->items, 0) : NULL;
}

const char *acpi_battery_type_label(const AcpiBattery *bat)
{
    if (!bat || !bat->type || !*bat->type)
        return "Battery";
    return bat->type;
}

char *acpi_battery_format_minutes(gint minutes)
{
    if (minutes < 0)
        return g_strdup("   NA");
    /* Оригинал печатал «%02i:%02i» и для часов, поэтому 10:05, не 10:5. */
    return g_strdup_printf("%02d:%02d", minutes / 60, minutes % 60);
}

void acpi_battery_free(AcpiBattery *bat)
{
    if (!bat)
        return;
    g_free(bat->name);
    g_free(bat->type);
    g_free(bat->status);
    g_free(bat->capacity_level);
    g_free(bat);
}

void acpi_battery_list_free(AcpiBatteryList *list)
{
    if (!list)
        return;
    if (list->items)
        g_ptr_array_unref(list->items);
    g_free(list);
}