/* acpi_battery_core.h — чтение батареи для плагина acpi_battery.
 *
 * Плагин НЕ читает /proc/acpi/battery/, как оригинальный
 * ACPIBatteryScreenlet.py. Причины конкретные:
 *
 *  - /proc/acpi/battery отсутствует на любой современной системе: путь
 *    закрыт ещё в Linux 3.8 в пользу sysfs. Модуль battery.ko больше не
 *    существует, /proc/acpi удалён целиком. Оригинал на этой машине
 *    падает с OSError на listdir() — автор оставил там пометку
 *    «TODO: raise exception!!!», то есть случай отсутствия батареи
 *    был незакрыт;
 *  - данные в sysfs идут ЧИСЛАМИ по одному на файл (capacity — «87»,
 *    status — «Discharging»), без парсинга строк на слова. Ровно тот
 *    аргумент, что в sensors_core против вывода sensors(1): формат
 *    однозначен, разбирать нечего и ловить нечего.
 *
 * Тот же источник читает upower, который и показывает штатные
 * уведомления GNOME о разряде, — то есть значения у нас и у системы
 * одинаковые.
 *
 * Свой core по правилу проекта: шаблонный плагин не редактируется.
 */
#ifndef ACPI_BATTERY_CORE_H
#define ACPI_BATTERY_CORE_H

#include <glib.h>

/* Состояние заряда. Порядок важен: ACPI_BATTERY_UNKNOWN — «данных нет»,
 * и applet обязан отличать его от «заряжен на 100%». В оригинале это
 * строка 'NA', и она же попадала в ветку alarm — то есть отсутствие
 * данных изображалось как низкий заряд. */
typedef enum {
    ACPI_BATTERY_UNKNOWN = 0,
    ACPI_BATTERY_CHARGING,
    ACPI_BATTERY_DISCHARGING,
    ACPI_BATTERY_FULL,
    /* Сеть подключена, но зарядка не идёт: сработал порог заряда в BIOS,
     * батарея полна при нулевом токе, зарядка питается не от сети. Это НЕ
     * «заряжена»: при заряде ниже 90 % батарея ещё разряжается. UPower
     * для этого состояния тоже отдельный тип — PENDING_CHARGE. */
    ACPI_BATTERY_PENDING_CHARGE,
    ACPI_BATTERY_NOT_PRESENT
} AcpiBatteryState;

/* Один источник питания. */
typedef struct {
    char          *name;      /* каталог в sysfs: "BAT0", "BAT1", "hidpp_battery_0" */
    char          *type;      /* "Battery", "Mouse", "USB"… из файла type */
    char          *status;    /* сырое значение status, для диагностики */
    AcpiBatteryState state;
    gint           percent;   /* capacity, -1 если неизвестно */
    gint64         energy_now;      /* energy_now, микроватт-часы, -1 */
    gint64         energy_full;     /* energy_full, -1 */
    /* Остаточное время в минутах, -1 если вычислить нельзя.
     *
     * Считается из энергии и мощности, а НЕ из capacity: проценты в
     * sysfs округлены и не дают времени. Оригинал делил remaining на
     * present_rate — те же данные в другой форме. */
    gint           minutes_left;
    gboolean       has_energy;      /* есть energy_now/energy_full/power_now */
    gint64         power_now;       /* микроВатты, -1 */
    /* Ток заряда/разряда, микроАмперы. Может быть ОТРИЦАТЕЛЬНЫМ — у
     * ряда драйверов (axp20x и подобные) разряд пишется отрицательным
     * током при статусе «charging», то есть драйвер врёт. Признак
     * используется в quirk-правиле: отрицательный ток означает, что
     * батарея разряжается, даже если status говорит «charging». */
    gint64         current_now;     /* -1 */
    /* Уровень из файла capacity_level, когда capacity == 0. Диапазоны
     * фиксированы ядром: Unknown/Normal/High/Low/Critical. Одно число
     * брать нельзя: Critical это 1 %, Low 10 %, High 70 % — поэтому
     * храним текст и разбираем в quirk-правиле. */
    char          *capacity_level;
    /* Устройство есть в sysfs, но не читается (права, отказ драйвера).
     * НЕ то же, что percent == -1: строка не должна исчезать. */
    gboolean       read_error;
} AcpiBattery;

/* Список найденных источников. */
typedef struct {
    GPtrArray *items; /* AcpiBattery* */
} AcpiBatteryList;

/* Корень sysfs. На живой машине "/sys/class/power_supply", в тестах —
 * фикстура под temps/. Читать можно без root: файлы там 0444. */
#define ACPI_BATTERY_SYSFS_ROOT "/sys/class/power_supply"

/* Разобрать значение файла status. Неизвестная строка — не ошибка, а
 * ACPI_BATTERY_UNKNOWN: драйверы пишут и "Unknown", и пустую строку,
 * и регистр плавает между версиями ядра. */
AcpiBatteryState acpi_battery_parse_status(const char *text);

/* Прочитать корень sysfs. Возвращает NULL при ошибке открытия каталога
 * (нет /sys/class/power_supply — например, на этой машине без батареи).
 * Пустой список — НЕ ошибка: applet покажет «No battery», как и задумано. */
AcpiBatteryList *acpi_battery_list_read(const char *root);

/* Первый источник, который реально пригоден для показа. */
AcpiBattery *acpi_battery_list_primary(const AcpiBatteryList *list);

/* Имя типа, как его показывает applet: "Battery", "Mouse", "USB"… */
const char *acpi_battery_type_label(const AcpiBattery *bat);

/* Форматирование времени: 95 -> "1:35", 600 -> "10:00". Часы не
 * ограничиваем: оригинал тоже печатал «99:59» при rate около нуля. */
char *acpi_battery_format_minutes(gint minutes);

void acpi_battery_free(AcpiBattery *bat);
void acpi_battery_list_free(AcpiBatteryList *list);

#endif /* ACPI_BATTERY_CORE_H */