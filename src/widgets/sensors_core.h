/* sensors_core.h — разбор hwmon для плагина sensors.
 *
 * Плагин НЕ вызывает sensors(1): читает /sys/class/hwmon напрямую.
 * Причины конкретные, не про стиль:
 *
 *  - sensors держит в памяти кэш (по умолчанию значений не кэширует, но
 *    форматирует и парсит), а для 173 строк вывода это заметная работа
 *    каждые update_interval;
 *  - его вывод — для человека («+36.0°C  (low = ...»). Число приходится
 *    доставать grep/awk, как в конфигах conky пользователя:
 *      sensors i350bb-pci-0500 | grep -iE '^(loc1)' | awk '{print $2}'
 *    Это shell в апплете и отдельный процесс на каждый кадр;
 *  - имя чипа в sensors — это СКЛЕЕННЫЙ вид: hwmon/name даёт «i350bb»,
 *    а sensors печатает «i350bb-pci-0500» (доктрина PCI-адреса из
 *    sysfs/device). В sysfs доктрину надо собирать самому, если она нужна.
 *
 * Формат sysfs однозначен: tempN_input — целое в МИЛЛИГРАДУСАХ.
 * Одно число, без единиц, без знака градуса. Разбирать там нечего, и
 * ловить нечего — в отличие от вывода sensors.
 *
 * Свой core по правилу проекта: шаблонный плагин не редактируется.
 */
#ifndef SENSORS_CORE_H
#define SENSORS_CORE_H

#include <glib.h>
#include <cairo.h>

/* Один канал. Сенсоры идут в порядке sysfs: hwmon0..hwmonN, внутри
 * temp1, temp2, … Сортировка по имени файла обязательна: ls выдаёт
 * temp1, temp10, temp2 по алфавиту, и в выводе пользователя каналы
 * окажутся переставлены. */
typedef struct {
    char *chip;     /* hwmon/name: "i350bb", "nvme", "drivetemp" */
    char *label;    /* tempN_label, если есть; иначе "tempN" */
    char *device;   /* доктрина sysfs/device ("0000:05:00.0", "nvme0"),
                     * может быть пустым */
    gint channel;   /* N из tempN_input */
    gdouble celsius;/* tempN_input / 1000.0 */
    gboolean valid; /* FALSE если канал отдаёт «нет данных» (SENSOR_INVALID_MILLI) */
    /* Файл канала не удалось прочитать. На серверной матери с BMC чип
     * сидит на общем i2c-адаптере с ipmi, и при параллельном обращении
     * драйвер отдаёт -EAGAIN: g_file_get_contents() возвращает FALSE.
     * Это НЕ «канала нет» и НЕ 0 градусов — значение неизвестно, и
     * показывать надо прочерк, сохранив строку. Различать эти три
     * состояния обязательно, иначе один сбой чтения на кадр убирает
     * строку из вывода, и она «мигает». */
    gboolean read_error;
} SensorReading;

/* Сенсор целиком: один hwmon, его имя и все каналы.
 *
 * Идентификатор — device (устойчивый путь), а НЕ номер hwmon. Номер
 * hwmonN — это индекс порядка регистрации драйверов, и он меняется
 * между загрузками: тот же nvme после перезагрузки может оказаться
 * hwmon2 вместо hwmon0, и строка конфига «nvme/Composite» молча
 * переехала бы на другой диск. Тот же приём, что в disk_monitor с
 * /dev/disk/by-id: храним путь, который выводится из enumeration
 * устройства, а не из порядка выдачи. */
typedef struct {
    char *chip;
    char *device;   /* basename последнего значимого сегмента: «nvme0»,
                      * «0000:05:00.0», «1-002d», «0:0:2:0» */
    char *dev_path; /* полный путь /sys/devices/... — уникальный ключ,
                      * одинаков для всех каналов чипа */
    GPtrArray *readings; /* SensorReading*, NULL-terminated не нужен,
                          * считаем через ->len */
} SensorChip;

/* Весь список. */
typedef struct {
    GPtrArray *chips; /* SensorChip* */
} SensorList;

/* Значение «канала нет» в sysfs. Linux отдаёт -128000 (= -128 °C) для
 * канала, который физически не подключён — это НЕ минус 128 градусов,
 * это маркер. nct7904 на этой машине отдаёт ровно -128000 на temp1,6,7.
 * Если suchое значение отрисовать, в списке появится «-128.0°C» рядом с
 * настоящими значениями и пользователь будет искать, где оно взялось. */
#define SENSOR_INVALID_MILLI (-128000)

void sensor_list_free(SensorList *list);
void sensor_reading_free(gpointer data);
void sensor_chip_free(gpointer data);

/* Разбор цвета из конфига. Принимает ВСЕ формы, которые пишут конфиги
 * этого проекта: «0.2,0.75,1.0,1.0» (legacy network_monitor), «rgb(r,g,b)»
 * и «rgba(r,g,b,a)» с компонентами 0..255. Возвращает FALSE на мусоре —
 * вызывающий обязан оставить свой дефолт, а не записать нули: тихий
 * чёрный на месте задуманного бирюзового читается как «цвет не работает».
 * Расхождение парсеров было реальной ошибкой: UI писал rgba(), а парсер
 * понимал только legacy — кнопка цвета молча не применялась. */
gboolean sensor_parse_rgba(const char *text, gdouble out[4]);
char *sensor_format_rgba(const gdouble color[4]);

/* Скругление окна. Формулы взяты из network_monitor_core.c (nm_rounded_*) —
 * там уже отлажена геометрия среза угла, и своя копия разошлась бы с
 * существующей при первом же расхождении в толщине рамки. Своё ядро
 * плагина — это про ДАННЫЕ (парсер sysfs), а не про дублирование
 * формул скругления, которые обязаны совпадать с остальными апплетами.
 *
 * Округление НЕ включает прозрачность: cairo-клип в draw() ограничивает
 * только рисование плагина, а само X-окно остаётся прямоугольным, пока
 * не задан shape. Поэтому форму вызывают дважды — в draw() (клип) и в
 * sensor_apply_shape() (реальная форма окна, чтобы клики в углы не
 * попадали). */
double sensor_corner_radius_value(int value);
gboolean sensor_corner_radius_is_rounded(double radius);
cairo_region_t *sensor_rounded_region(int width, int height, int radius);

/* Прочитать /sys/class/hwmon (или другой каталог — для тестов).
 * root = "/sys/class/hwmon" на живой машине, фикстура под temps/ в тестах.
 * Возвращает NULL при ошибке; список без каналов — не ошибка. */
SensorList *sensor_list_read(const char *root);

/* Найти сенсор по имени чипа. Точное совпадение: префикс НЕ подходит.
 * «nvme» не должен ловить «nvme-pci-8100» и наоборот — иначе в списке
 * схлопнутся четыре разных nvme в один пункт настроек. */
SensorChip *sensor_list_find(const SensorList *list, const char *chip);

/* Тестовая строка по умолчанию.
 *
 * Пока пользователь не выбрал ни одного сенсора, апплет показывает
 * ровно одну строку «dummy» с ФИКСТИРОВНЫМ значением 36.6 °C. Она не
 * читается ниоткуда: это маркер «плагин работает, значение выводится»,
 * а не данные какого-то датчика. Любая настоящая температура появляется
 * только когда пользователь отметил её галочкой в Настройках. */
#define SEN_DUMMY_LABEL  "dummy"
#define SEN_DUMMY_SOURCE "dummy"
#define SEN_DUMMY_CELSIUS 36.6

/* Устойчивый идентификатор строки: «<chip>/<device>». По нему ищем
 * канал, он не зависит от номера hwmon. */
char *sensor_chip_id(const SensorChip *chip);
char *sensor_row_id(const char *chip, const char *device, const char *label);

/* Найти канал по устойчивому идентификатору. chip в config — это то,
 * что видит пользователь («nvme»); он может совпадать у нескольких
 * устройств, поэтому device в идентификаторе обязателен. */
SensorReading *sensor_find_reading(const SensorList *list, const char *row_id);

/* Найти по имени sensors, как его печатает sensors(1):
 * «i350bb-pci-0500», «nvme-pci-0200», «nct7904-i2c-1-2d». Нужно один раз
 * при первом заполнении, чтобы показать пользователю те же имена, что в
 * его conky-конфигах, и чтобы он мог опознать сенсор глазами. */
SensorChip *sensor_list_find_sensors_name(const SensorList *list,
                                          const char *sensors_name);
char *sensor_chip_sensors_name(const SensorChip *chip);
/* Тип шины: «pci», «scsi», «i2c», «isa», либо «none» если шины нет. */
char *sensor_bus_kind(const char *dev_path, const char *chip);

/* Имя группы для таблицы диалога: «drivetemp-scsi», «nvme-pci»,
 * «coretemp-isa». ВКЛЮЧАЕТ имя чипа: два coretemp дают слоты «0000» и
 * «0001», и без чипа в группе пользователь не отличил бы строки. */
char *sensor_group_name(const char *chip, const char *dev_path);

/* Укороченный слот без чипа и шины: «1-10», «0500», «0000», «1-2d». */
char *sensor_bus_slot(const char *dev_path);

/* Температура с округлением до десятых, с единицей измерения.
 * Единица вынесена наружу, потому что conky-конфиги пользователя
 * переключают её через temperature_unit, а в applet выбор должен
 * быть в настройках, а не зашит в парсер. */
gdouble sensor_to_display_unit(gdouble celsius, gboolean fahrenheit);
const char *sensor_unit_suffix(gboolean fahrenheit);

/* Форматирование «58.0°C» / «118.4°F». celsius уже в °C. */
char *sensor_format_value(gdouble celsius, gboolean fahrenheit,
                          gboolean with_unit);

/* Строки конфига, задающие список. Читаются как список, разделённый
 * «;» — формат совпадает с тем, как xs_api пишет списки, и переживает
 * пробелы в подписях, в отличие от запятой. */
GPtrArray *sensor_config_list(const char *text);   /* char* */
char      *sensor_config_join(const GPtrArray *items); /* g_free */
/* Склейка пар «подпись|источник» в значение ключа rows. Подпись
 * редактирует пользователь, источник обязан остаться тем же. */
char      *sensor_config_join_pair(const GPtrArray *labels,
                                   const GPtrArray *sources); /* g_free */

#endif /* SENSORS_CORE_H */
