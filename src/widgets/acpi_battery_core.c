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
        return ACPI_BATTERY_FULL;   /* заряжена, но не заряжается */

    /* "Unknown" и всё прочее — данных нет. */
    return ACPI_BATTERY_UNKNOWN;
}

static void battery_read(AcpiBattery *bat, const char *dir)
{
    gboolean ok_cap, ok_now, ok_full, ok_pow;
    gint64 cap, now, full, pow;

    char *present = read_str_file(dir, "present");
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

    if (ok_cap && cap >= 0) {
        bat->percent = (gint)cap;
        /* Драйверы иногда отдают 255 как «неизвестно», а не -1. */
        if (bat->percent > 100)
            bat->percent = -1;
    } else {
        bat->percent = -1;
    }

    if (ok_now && now >= 0) {
        bat->energy_now = now;
    }
    if (ok_full && full >= 0) {
        bat->energy_full = full;
    }
    if (ok_pow && pow >= 0) {
        bat->power_now = pow;
    }

    /* Время считаем только когда есть все три величины. power_now у
     * разряжающейся батареи нормально положителен; у заряжающейся идёт
     * со знаком минус в некоторых драйверах — берём модуль, иначе время
     * до полного заряда получится отрицательным. */
    bat->has_energy = (bat->energy_now >= 0 && bat->energy_full >= 0);
    if (bat->has_energy && pow > 0 && bat->energy_full > 0) {
        gint64 power = pow > 0 ? pow : -pow;
        if (bat->state == ACPI_BATTERY_CHARGING)
            bat->minutes_left = (gint)((bat->energy_full - bat->energy_now)
                                       * 60 / power);
        else
            bat->minutes_left = (gint)(bat->energy_now * 60 / power);
        if (bat->minutes_left < 0)
            bat->minutes_left = -1;
    } else {
        bat->minutes_left = -1;
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

    /* Приоритет: настоящая батарея, потом устройство с батарейкой
     * (мышь/гарнитура). Порядок каталогов в sysfs алфавитный и от
     * загрузки не зависит, но имя «hidpp_battery_0» не должно
     * перебивать «BAT0», поэтому сортируем по типу, а не по позиции. */
    for (i = 0; i < list->items->len; i++) {
        AcpiBattery *bat = g_ptr_array_index(list->items, i);
        if (bat->type && g_ascii_strcasecmp(bat->type, "Battery") == 0)
            return bat;
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