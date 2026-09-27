/* sensors_core.c — чтение hwmon из sysfs. Без GTK, без состояния. */
#include "sensors_core.h"

#include <errno.h>
#include <math.h>
#include <string.h>
#include <stdlib.h>

/* ---------------------------------------------------------------- список */

/* Порядок каналов: temp2 обязан идти раньше temp10. Сравнение строк
 * даёт «temp10» < «temp2», то есть ровно тот порядок, в котором каналы
 * оказываются переставленными, если полагаться на алфавит. */
static gint sensor_reading_cmp(gconstpointer a, gconstpointer b)
{
    const SensorReading *ra = *(const SensorReading *const *)a;
    const SensorReading *rb = *(const SensorReading *const *)b;

    if (ra->channel != rb->channel)
        return (ra->channel < rb->channel) ? -1 : 1;
    return g_strcmp0(ra->label, rb->label);
}

static void sensor_reading_free(gpointer data)
{
    SensorReading *r = data;

    if (!r)
        return;
    g_free(r->chip);
    g_free(r->label);
    g_free(r->device);
    g_free(r);
}

static void sensor_chip_free(gpointer data)
{
    SensorChip *c = data;

    if (!c)
        return;
    g_free(c->chip);
    g_free(c->device);
    g_free(c->dev_path);
    if (c->readings)
        g_ptr_array_unref(c->readings);
    g_free(c);
}

void sensor_list_free(SensorList *list)
{
    if (!list)
        return;
    if (list->chips)
        g_ptr_array_unref(list->chips);
    g_free(list);
}

/* ---------------------------------------------------------------- чтение */

/* Прочитать первую строку целого файла. */
/* Чтение одного значения sysfs.
 *
 * Возвращает SEN_READ_OK / SEN_READ_MISSING / SEN_READ_ERROR — различать
 * обязательно, и вот почему.
 *
 * Параллельный доступ к драйверу: на серверной матери с BMC чип вроде
 * nct7904 висит на том же i2c-адаптере, что и IPMI (на этой машине
 * hwmon7 = nct7904 на i2c-1, и там же ipmi_si IPI0001:00). Если в этот
 * момент ipmitool или другой читатель дёргает тот же адаптер, read()
 * отдаёт -EAGAIN, и g_file_get_contents() возвращает FALSE. Раньше это
 * молча выглядело как «канала нет», то есть строка исчезала из вывода
 * на кадр и появлялась снова. Теперь различаем «файла нет/канал отключён»
 * и «прочитать не удалось из-за драйвера» — во втором случае канал
 * показывается как есть, только с прочерком вместо числа.
 *
 * Само значение при ошибке чтения НЕ подставляется: подставить 0 —
 * значит показать правдоподобную неправду, и на матери с десятками
 * каналов «0.0°C» выглядит как реальное измерение. */
typedef enum {
    SEN_READ_OK = 0,
    SEN_READ_MISSING,   /* файла нет: канал не подключён */
    SEN_READ_ERROR      /* файл есть, но драйвер не отдал значение */
} SenReadResult;

static SenReadResult read_long(const char *path, glong *out)
{
    gchar *text = NULL;
    gchar *end = NULL;
    gboolean parsed;

    if (!g_file_get_contents(path, &text, NULL, NULL))
        return g_file_test(path, G_FILE_TEST_EXISTS) ? SEN_READ_ERROR
                                                      : SEN_READ_MISSING;
    /* sysfs отдаёт «58000\n». g_ascii_strtoll терпит хвост, поэтому
     * проверяем именно факт разбора — молчаливый 0 на нечитаемом файле
     * выглядел бы как настоящие 0 градусов. */
    errno = 0;
    *out = g_ascii_strtoll(text, &end, 10);
    parsed = (end != text) && (errno == 0);
    g_free(text);
    return parsed ? SEN_READ_OK : SEN_READ_ERROR;
}

static char *read_text(const char *path)
{
    gchar *text = NULL;

    if (!g_file_get_contents(path, &text, NULL, NULL))
        return NULL;
    /* хвостовой перевод строки отбрасываем, чтобы имя в настройках
     * не отличалось от имени в выводе на невидимом символе */
    g_strstrip(text);
    if (!*text) {
        g_free(text);
        return NULL;
    }
    return text;
}

/* Устройство за hwmon: sysfs/device — симлинк. Возвращаем basename
 * («nvme0», «0000:05:00.0», «1-002d», «0:0:2:0») и полный путь.
 *
 * Именно этот путь, а не номер hwmon, идентифицирует сенсор: номера
 * hwmonN — индекс порядка регистрации драйверов и меняются между
 * загрузками, а путь выводится из enumeration шины. Все 21 путь на этой
 * машине уникальны, и каждый содержит имя чипа.
 *
 * Спецслучай coretemp: путь /sys/devices/platform/coretemp.0, то есть
 * компонента «platform» — не шина. Имя sensors у него собирается из
 * имени устройства, поэтому для платформенных путей device берём из
 * последнего сегмента с префиксом «coretemp». */
static char *read_device_name(const char *hwmon_dir, char **out_path)
{
    char *link = g_build_filename(hwmon_dir, "device", NULL);
    char *target = g_file_read_link(link, NULL);
    char *base = NULL;

    if (out_path)
        *out_path = NULL;
    g_free(link);
    if (!target)
        return NULL;
    /* Симлинк sysfs/device ОТНОСИТЕЛЬНЫЙ («../../nvme0»), поэтому
     * realpath() отрабатывает относительно текущего каталога процесса и
     * возвращает мусор вида «../../../1-002d» — путь без каталога шины,
     * из которого нечего взять слот, и имя sensors не собирается.
     * Канонизируем относительно каталога hwmon: склеиваем и просим
     * realpath развернуть всю цепочку от корня. */
    if (target[0] != '/') {
        char *joined = g_build_filename(hwmon_dir, target, NULL);
        char *real = realpath(joined, NULL);

        g_free(joined);
        if (real) {
            g_free(target);
            target = real;
        } else {
            /* symlink битый: оставляем target, имя sensors будет без
             * слота, но сенсор не потеряется */
        }
    } else {
        char *real = realpath(target, NULL);

        if (real) {
            g_free(target);
            target = real;
        }
    }
    base = g_path_get_basename(target);
    if (out_path)
        *out_path = g_strdup(target);
    g_free(target);
    return base;
}

/* Каналы одного hwmon. Возвращает GPtrArray* (может быть пустым) или
 * NULL, если каталог недоступен. */
static GPtrArray *read_chip_channels(const char *hwmon_dir,
                                     const char *chip, const char *device)
{
    GDir *dir = g_dir_open(hwmon_dir, 0, NULL);
    GPtrArray *out;
    const char *name;

    if (!dir)
        return NULL;
    out = g_ptr_array_new_with_free_func(sensor_reading_free);
    while ((name = g_dir_read_name(dir)) != NULL) {
        SensorReading *r;
        char *input_path;
        glong milli = 0;
        SenReadResult rr;

        /* Только tempN_input. Вентиляторы (fanN_input, об/мин) и
         * напряжения (inN_input, мВ) в этом апплете не показываются:
         * единица другая и формат строки был бы другой, а подпись
         * «X: 58.0°C» для них бессмыслична. */
        if (!g_str_has_prefix(name, "temp"))
            continue;
        if (!g_str_has_suffix(name, "_input"))
            continue;

        input_path = g_build_filename(hwmon_dir, name, NULL);
        rr = read_long(input_path, &milli);
        g_free(input_path);
        /* MISSING — файла нет, канала физически нет: строка не нужна.
         * ERROR — канал есть, но драйвер не отдал значение (параллельный
         * доступ к i2c с ipmi на серверной матери): строка нужна, число
         * неизвестно. Раньше оба случая сводились к continue, и один
         * конкурентный читатель убирал строку на кадр — она мигала. */
        if (rr == SEN_READ_MISSING)
            continue;

        r = g_new0(SensorReading, 1);
        r->chip = g_strdup(chip);
        r->device = g_strdup(device);
        r->celsius = (gdouble) milli / 1000.0;
        r->valid = (rr == SEN_READ_OK) && (milli != SENSOR_INVALID_MILLI);
        r->read_error = (rr != SEN_READ_OK);
        r->channel = 0;
        {
            const char *digits = name + 4; /* после «temp» */
            r->channel = (gint) g_ascii_strtoll(digits, NULL, 10);
        }
        /* Метка канала: tempN_label есть не у всех драйверов. У i350bb
         * он есть и несёт смысл («loc1»), у nct7904 меток нет вовсе и
         * каналы различаются только номером. */
        {
            char *label = g_strdup_printf("temp%d", r->channel);
            char *lp = g_strdup_printf("%s/temp%d_label", hwmon_dir,
                                        r->channel);
            char *from_sys = read_text(lp);
            g_free(lp);
            if (from_sys) {
                g_free(label);
                label = from_sys;
            }
            r->label = label;
        }
        g_ptr_array_add(out, r);
    }
    g_dir_close(dir);
    g_ptr_array_sort(out, sensor_reading_cmp);
    return out;
}

static gint chip_cmp(gconstpointer a, gconstpointer b)
{
    const SensorChip *ca = *(const SensorChip *const *)a;
    const SensorChip *cb = *(const SensorChip *const *)b;
    gint rc = g_strcmp0(ca->chip, cb->chip);

    if (rc != 0)
        return rc;
    /* один и тот же драйвер на нескольких устройствах: разводим по
     * доктрине, иначе два nct7904 склеятся в один пункт */
    return g_strcmp0(ca->device, cb->device);
}

SensorList *sensor_list_read(const char *root)
{
    GDir *dir;
    const char *name;
    SensorList *list;
    GPtrArray *hwmons;

    if (!root)
        return NULL;
    dir = g_dir_open(root, 0, NULL);
    if (!dir)
        return NULL;

    list = g_new0(SensorList, 1);
    list->chips = g_ptr_array_new_with_free_func(sensor_chip_free);
    hwmons = g_ptr_array_new_with_free_func(g_free);

    while ((name = g_dir_read_name(dir)) != NULL) {
        if (!g_str_has_prefix(name, "hwmon"))
            continue;
        g_ptr_array_add(hwmons, g_strdup(name));
    }
    g_dir_close(dir);

    /* g_dir_read_name не гарантирует порядок, а hwmon0..hwmon20 должны
     * идти по возрастанию: это порядок, в котором sensors их печатает,
     * и на него рассчитывает пользователь, перенося конфиг. */
    g_ptr_array_sort_values(hwmons, (GCompareFunc) g_strcmp0);

    for (guint i = 0; i < hwmons->len; i++) {
        const char *dirname = g_ptr_array_index(hwmons, i);
        char *dirpath = g_build_filename(root, dirname, NULL);
        char *chip = read_text(g_build_filename(dirpath, "name", NULL));
        char *dev_path = NULL;
        char *device = read_device_name(dirpath, &dev_path);
        GPtrArray *channels;

        if (!chip)
            chip = g_strdup(dirname); /* имя есть не у всех hwmon */
        if (!device) {
            /* hwmon без device (не бывает на этой машине, но каталог
             * может быть битый): падаем на имя каталога. Такой ключ
             * неустойчив, и это лучше, чем не показывать сенсор. */
            device = g_strdup(dirname);
        }
        channels = read_chip_channels(dirpath, chip, device);
        if (channels && channels->len > 0) {
            SensorChip *c = g_new0(SensorChip, 1);
            c->chip = chip;
            c->device = device;
            c->dev_path = dev_path;
            c->readings = channels;
            g_ptr_array_add(list->chips, c);
            chip = NULL;   /* перешло в SensorChip */
            device = NULL;
            dev_path = NULL;
        } else if (channels) {
            g_ptr_array_unref(channels);
        }
        g_free(chip);
        g_free(device);
        g_free(dev_path);
        g_free(dirpath);
    }
    g_ptr_array_unref(hwmons);
    if (list->chips)
        g_ptr_array_sort(list->chips, chip_cmp);
    return list;
}

SensorChip *sensor_list_find(const SensorList *list, const char *chip)
{
    if (!list || !list->chips || !chip)
        return NULL;
    for (guint i = 0; i < list->chips->len; i++) {
        SensorChip *c = g_ptr_array_index(list->chips, i);
        if (g_strcmp0(c->chip, chip) == 0)
            return c;
    }
    return NULL;
}

/* --------------------------------------------------- устойчивые ключи */

char *sensor_chip_id(const SensorChip *chip)
{
    if (!chip)
        return NULL;
    return g_strdup_printf("%s/%s", chip->chip, chip->device);
}

char *sensor_row_id(const char *chip, const char *device, const char *label)
{
    if (!chip || !device || !label)
        return NULL;
    return g_strdup_printf("%s/%s/%s", chip, device, label);
}

/* Имя в стиле sensors(1), чтобы пользователь опознавал сенсор по тому
 * же имени, что в своих conky-конфигах: «i350bb-pci-0500»,
 * «nvme-pci-0200», «nct7904-i2c-1-2d», «coretemp-isa-0001»,
 * «drivetemp-scsi-2-0».
 *
 * Шина и слот берутся из dev_path, а не вычисляются: путь уже содержит
 * «pci0000:00/0000:00:03.2/0000:05:00.0» или «i2c-1/1-002d», то есть
 * ровно те компоненты, из которых sensors склеивает суффикс.
 *
 * Два наблюдения, важные для совпадения с sensors:
 *
 *  - у NVME слот берётся из ПОСЛЕДНЕГО PCI-компонента, а не первого:
 *    путь /pci0000:00/0000:00:02.0/0000:02:00.0/nvme/nvme0 — это мост
 *    00:02.0 и сам контроллер 02:00.0, и sensors печатает «pci-0200»;
 *  - у coretemp путь /sys/devices/platform/coretemp.0 не содержит слота
 *    вообще. sensors берёт его из coretempN (isa-0000 для N=0), поэтому
 *    для платформенного пути слот синтезируется из номера в имени
 *    устройства, иначе получилось бы «coretemp-isa-0000» для обоих
 *    процессоров. */
static char *sensors_slot_from_path(const char *dev_path)
{
    /* Пустая строка — не то же, что NULL: g_strsplit("", "/") даёт
     * массив из одного пустого элемента, и обращение к последнему
     * сегменту уходит за границу. Такое значение приходит из теста и
     * может прийти из конфига, поэтому проверяем явно. */
    if (!dev_path || !*dev_path)
        return NULL;
    /* последний компонент, начинающийся с PCI-адреса или с i2c-адреса */
    {
        char **parts = g_strsplit(dev_path, "/", -1);
        int n = (int) g_strv_length(parts);
        char *found = NULL;
        /* Сначала ПРОВЕРЯЕМ SCSI-ХВОСТ, и только потом ищем PCI-мост.
         *
         * Путь диска длинный и содержит оба вида адресов:
         *   .../pci0000:00/0000:07:00.0/host0/.../target0:0:2/0:0:2:0
         * PCI-мост 0000:07:00.0 стоит РАНЬШЕ SCSI-хвоста 0:0:2:0, так что
         * обход с конца находит SCSI первым — но только если SCSI стоит
         * последним сегментом. Обход с конца по всем сегментам брал бы
         * мост, и drivetemp назывался бы «drivetemp-pci-0700». */
        {
            const char *last = parts[n - 1];
            int colons = 0;

            for (const char *q = last; *q; q++)
                if (*q == ':')
                    colons++;
            if (colons == 3 && g_ascii_isdigit(last[0])) {
                /* g_strdup ДО g_strfreev: возврат g_strdup(last) после
                 * освобождения массива отдавал указатель на освобождённую
                 * память, и имя получалось мусорным («drivetemp-i2c-??7U»).
                 * Имена не выглядели бы повреждёнными при беглом взгляде,
                 * но не совпали бы ни с одним сенсором. */
                char *scsi = g_strdup(last);

                g_strfreev(parts);
                return scsi;
            }
        }
        /* Полный PCI-адрес домена:dev.fn содержит ДВОЕ двоеточия
         * («0000:05:00.0»). Различать надо по их ЧИСЛУ, а не по факту
         * наличия: SCSI-адрес отличается именно количеством. */
        for (int i = n - 1; i >= 0; i--) {
            const char *seg = parts[i];
            int colons = 0;

            for (const char *p = seg; *p; p++)
                if (*p == ':')
                    colons++;
            if (colons == 2 && g_ascii_isdigit(seg[0])) {
                found = g_strdup(seg);
                break;
            }
            if (g_str_has_prefix(seg, "i2c-") && i + 1 < n) {
                found = g_strdup(parts[i + 1]);
                break;
            }
        }
        g_strfreev(parts);
        if (found)
            return found;
    }
    /* платформа: слот синтезируем из имени устройства, sensors так же */
    {
        const char *base = strrchr(dev_path, '/');
        base = base ? base + 1 : dev_path;
        if (g_str_has_prefix(base, "coretemp.")) {
            guint n = (guint) g_ascii_strtoull(base + 9, NULL, 10);
            return g_strdup_printf("isa-%04x", n);
        }
    }
    return NULL;
}

/* ------------------------------------------------- разбор по типу и слоту
 *
 * Имя в стиле sensors — это «чип-<шина>-<слот>». Для диалога нужно то же
 * самое, но РАЗДЕЛЁННОЕ: тип группы («drivetemp», «scsi», «pci», «i2c»)
 * и укороченный слот («1-10» вместо «scsi-1-10»). Дублировать разбор
 * пути в плагине нельзя: формулы SCSI и i2c выведены сверкой с живым
 * sensors(1), и вторая копия разъедется с первой при первом же
 * новом устройстве. */
char *sensor_bus_kind(const char *dev_path, const char *chip)
{
    char *slot = sensors_slot_from_path(dev_path);
    char *kind;
    int colons = 0;

    if (slot) {
        for (const char *p = slot; *p; p++)
            if (*p == ':')
                colons++;
        /* Шина по форме слота, а не по порядку веток: SCSI-адрес
         * «0:0:2:0» начинается с цифры, как PCI, но содержит ТРИ
         * двоеточия против двух. */
        if (colons == 2)
            kind = g_strdup("pci");
        else if (colons == 3)
            kind = g_strdup("scsi");
        else if (g_str_has_prefix(slot, "isa-"))
            kind = g_strdup("isa");
        else
            kind = g_strdup("i2c");
        g_free(slot);
        return kind;
    }
    /* Чип без распознанной шины: настоящей шины у него нет, и sensors
     * ничего похожего на «-pci-» не печатает. */
    (void) chip;
    return g_strdup("none");
}

/* Имя группы для диалога: «drivetemp-scsi», «nvme-pci», «coretemp-isa».
 *
 * Группа ВКЛЮЧАЕТ имя чипа, иначе в одной таблице окажутся несвязанные
 * устройства: два coretemp (coretemp.0 и coretemp.1) дают слоты «0000» и
 * «0001», и пользователь видит две строки с именами ядер, не понимая,
 * что они с разных процессоров. С шиной без чипа таблица была бы
 * общей для всех isa-сенсоров машины.
 *
 * Слот внутри группы уникален — проверено на живой машине. */
char *sensor_group_name(const char *chip, const char *dev_path)
{
    char *kind = sensor_bus_kind(dev_path, chip);
    char *group;

    if (g_strcmp0(kind, "none") == 0)
        group = g_strdup(chip ? chip : "unknown");
    else
        group = g_strdup_printf("%s-%s", chip ? chip : "unknown", kind);
    g_free(kind);
    return group;
}

/* Укороченный слот без имени чипа и без названия шины: «1-10», «0500»,
 * «0000», «1-2d». Это то, что видит пользователь в таблице. */
char *sensor_bus_slot(const char *dev_path)
{
    char *slot = sensors_slot_from_path(dev_path);

    if (!slot)
        return g_strdup("");
    {
        int colons = 0;

        for (const char *p = slot; *p; p++)
            if (*p == ':')
                colons++;
        if (colons == 2) {
            /* PCI «0000:05:00.0» -> «0500»: домен и функция отброшены,
             * bus и device склеены. Ведущие нули сохраняются — sensors
             * печатает именно «0500». */
            const char *colon = strchr(slot, ':');
            char *busdev = g_strdup(colon + 1);
            char *dot = strchr(busdev, '.');
            char *second;

            if (dot)
                *dot = '\0';
            second = strchr(busdev, ':');
            if (second)
                memmove(second, second + 1, strlen(second));
            g_free(slot);
            return busdev;
        }
        if (colons == 3) {
            /* SCSI «0:0:2:0» -> «1-10»: host и target*10+lun, channel
             * отброшен. Числом, а не склейкой цифр: конкатенация дала
             * «00» там, где sensors печатает «0». */
            gchar **p = g_strsplit(slot, ":", -1);
            char *out;

            if (g_strv_length(p) >= 4) {
                guint target = (guint) g_ascii_strtoull(p[2], NULL, 10);
                guint lun = (guint) g_ascii_strtoull(p[3], NULL, 10);

                out = g_strdup_printf("%s-%u", p[0], target * 10 + lun);
            } else {
                out = g_strdup(slot);
            }
            g_strfreev(p);
            g_free(slot);
            return out;
        }
        if (g_str_has_prefix(slot, "isa-")) {
            char *out = g_strdup(slot + 4);

            g_free(slot);
            return out;
        }
        /* i2c «1-002d» -> «1-2d»: адрес без ведущих нулей, шина
         * остаётся. Слепим обратно в «1-2d» — это и есть укороченное
         * представление, которое видит пользователь. */
        {
            gchar **p = g_strsplit(slot, "-", -1);
            char *out;

            if (g_strv_length(p) >= 2) {
                char *z = p[1];

                while (*z == '0' && z[1])
                    z++;
                out = g_strdup_printf("%s-%s", p[0], z);
            } else {
                out = g_strdup(slot);
            }
            g_strfreev(p);
            g_free(slot);
            return out;
        }
    }
}

/* Полное имя «чип-шина-слот», собранное из двух частей. Формулы SCSI и
 * i2c те же, что проверены сверкой с sensors(1) на этой машине. */
char *sensor_chip_sensors_name(const SensorChip *chip)
{
    char *group, *slot, *name;

    if (!chip)
        return NULL;
    if (!chip->dev_path)
        return g_strdup(chip->chip ? chip->chip : "");
    group = sensor_group_name(chip->chip, chip->dev_path);
    slot = sensor_bus_slot(chip->dev_path);
    if (*slot)
        name = g_strdup_printf("%s-%s", group, slot);
    else
        name = g_strdup(group);
    g_free(group);
    g_free(slot);
    return name;
}

SensorReading *sensor_find_reading(const SensorList *list, const char *row_id)
{
    char *chip = NULL;
    const char *rest;
    char *device = NULL;
    const char *slash2;
    const char *label;
    guint i;

    if (!list || !list->chips || !row_id)
        return NULL;
    rest = strchr(row_id, '/');
    if (!rest)
        return NULL;
    chip = g_strndup(row_id, rest - row_id);
    rest++;
    slash2 = strchr(rest, '/');
    if (!slash2) {
        g_free(chip);
        return NULL;
    }
    device = g_strndup(rest, slash2 - rest);
    label = slash2 + 1;

    for (i = 0; i < list->chips->len; i++) {
        SensorChip *c = g_ptr_array_index(list->chips, i);

        if (g_strcmp0(c->chip, chip) != 0)
            continue;
        if (g_strcmp0(c->device, device) != 0)
            continue;
        for (guint j = 0; j < c->readings->len; j++) {
            SensorReading *r = g_ptr_array_index(c->readings, j);

            if (g_strcmp0(r->label, label) == 0) {
                g_free(chip);
                g_free(device);
                return r;
            }
        }
    }
    g_free(chip);
    g_free(device);
    return NULL;
}

SensorChip *sensor_list_find_sensors_name(const SensorList *list,
                                          const char *sensors_name)
{
    if (!list || !list->chips || !sensors_name)
        return NULL;
    for (guint i = 0; i < list->chips->len; i++) {
        SensorChip *c = g_ptr_array_index(list->chips, i);
        char *name = sensor_chip_sensors_name(c);
        gboolean hit = (g_strcmp0(name, sensors_name) == 0);

        g_free(name);
        if (hit)
            return c;
    }
    return NULL;
}

/* ------------------------------------------------------------- значения */

gdouble sensor_to_display_unit(gdouble celsius, gboolean fahrenheit)
{
    if (!fahrenheit)
        return celsius;
    return celsius * 9.0 / 5.0 + 32.0;
}

const char *sensor_unit_suffix(gboolean fahrenheit)
{
    return fahrenheit ? "°F" : "°C";
}

char *sensor_format_value(gdouble celsius, gboolean fahrenheit,
                          gboolean with_unit)
{
    gdouble v = sensor_to_display_unit(celsius, fahrenheit);

    if (with_unit)
        return g_strdup_printf("%.1f%s", v, sensor_unit_suffix(fahrenheit));
    return g_strdup_printf("%.1f", v);
}

/* ----------------------------------------------------------------- цвет */

gboolean sensor_parse_rgba(const char *text, gdouble out[4])
{
    gdouble v[4] = { 0, 0, 0, 1 };
    int n = 0;
    gboolean ok = FALSE;

    if (!text || !out)
        return FALSE;
    while (*text == ' ')
        text++;
    if (!*text)
        return FALSE;

    if (strchr(text, '(')) {
        /* rgb(51,191,255) / rgba(51,191,255,255) — компоненты 0..255.
         * Пробелы внутри скобок допустимы. */
        gboolean has_alpha = g_ascii_strncasecmp(text, "rgba", 4) == 0;
        const char *open = strchr(text, '(');
        const char *close = strrchr(text, ')');
        char *inner;
        gchar **parts;
        guint want = has_alpha ? 4 : 3;

        if (!open || !close || close < open)
            return FALSE;
        inner = g_strndup(open + 1, close - open - 1);
        parts = g_strsplit(inner, ",", -1);
        for (guint i = 0; parts[i] && n < 4; i++) {
            g_strstrip(parts[i]);
            if (!*parts[i])
                continue;
            v[n] = g_ascii_strtod(parts[i], NULL) / 255.0;
            n++;
        }
        g_strfreev(parts);
        g_free(inner);
        ok = (n == (int) want);
        if (!ok && n == 3)
            ok = TRUE; /* rgba без альфы — тоже принимаем */
    } else {
        /* legacy «0.2,0.75,1.0,1.0» — компоненты уже 0..1 */
        gchar **parts = g_strsplit(text, ",", -1);

        for (guint i = 0; parts[i] && n < 4; i++) {
            g_strstrip(parts[i]);
            if (!*parts[i])
                continue;
            v[n] = g_ascii_strtod(parts[i], NULL);
            n++;
        }
        g_strfreev(parts);
        ok = (n >= 3);
    }
    if (!ok)
        return FALSE;
    for (int i = 0; i < 4; i++) {
        if (v[i] < 0.0)
            v[i] = 0.0;
        if (v[i] > 1.0)
            v[i] = 1.0;
        out[i] = v[i];
    }
    return TRUE;
}

char *sensor_format_rgba(const gdouble color[4])
{
    return g_strdup_printf("%.6f,%.6f,%.6f,%.6f", color[0], color[1], color[2],
                           color[3]);
}

/* ------------------------------------------------------------ скругление */
/* Формулы идентичны network_monitor_core.c: nm_rounded_region(). Проверка
 * «глубина среза максимальна в угловой колонке и исчезает при i=scaled»
 * относится к геометрии окружности, а не к этому плагину, и второй
 * вариант обязан вести себя так же — иначе скруглённые апплеты будут
 * выглядеть по-разному при одинаковом corner_radius. */

double sensor_corner_radius_value(int value)
{
    return value > 0 ? (double)value : 0.0;
}

gboolean sensor_corner_radius_is_rounded(double radius)
{
    return radius > 0.5;
}

cairo_region_t *sensor_rounded_region(int width, int height, int radius)
{
    const double r = sensor_corner_radius_value(radius);
    cairo_region_t *region;
    cairo_rectangle_int_t box;
    double scaled;

    if (width <= 0 || height <= 0)
        return NULL;
    if (!sensor_corner_radius_is_rounded(r))
        return NULL;

    scaled = MIN(r, MIN(width, height) / 2.0);
    region = cairo_region_create();
    if (!region)
        return NULL;

    for (int i = 0; i <= (int) ceil(scaled); i++) {
        double d = fabs(i - scaled);
        int cut = 0;

        if (d <= scaled)
            cut = (int) floor(scaled - sqrt(scaled * scaled - d * d));
        box.x = i;
        box.y = cut;
        box.width = 1;
        box.height = height - 2 * cut;
        if (box.height > 0)
            cairo_region_union_rectangle(region, &box);
        box.x = width - 1 - i;
        if (box.height > 0)
            cairo_region_union_rectangle(region, &box);
    }
    {
        int mid = (int) ceil(scaled);

        box.x = mid;
        box.y = 0;
        box.width = width - 2 * mid;
        box.height = height;
        if (box.width > 0)
            cairo_region_union_rectangle(region, &box);
    }
    return region;
}

/* -------------------------------------------------------- список в conf */

/* Разделитель «;» вместо «,»: подписи вроде «Package id 1» пробелов не
 * содержат, но «Sensor 1» — да, и запятая внутри элемента сделала бы
 * список неоднозначным. */
/* Склейка пар «подпись|источник».
 *
 * Два массива, а не один: источник нужен для поиска в sysfs и меняться
 * не должен никогда, а подпись пользователь правит свободно. Склеивать
 * их где-то ещё (в init и в обработчике галочек) означало две
 * расходящиеся версии формата — и обработчик затирал источники, стоило
 * один раз двинуть галочку. */
char *sensor_config_join_pair(const GPtrArray *labels,
                              const GPtrArray *sources)
{
    GString *s = g_string_new(NULL);
    guint n = labels ? labels->len : 0;

    for (guint i = 0; i < n; i++) {
        const char *label = g_ptr_array_index(labels, i);
        const char *source = (sources && i < sources->len)
                           ? g_ptr_array_index(sources, i) : NULL;

        if (i > 0)
            g_string_append_c(s, ';');
        if (source && *source)
            g_string_append_printf(s, "%s|%s", label, source);
        else
            g_string_append(s, label);
    }
    return g_string_free(s, FALSE);
}

GPtrArray *sensor_config_list(const char *text)
{
    GPtrArray *out = g_ptr_array_new_with_free_func(g_free);
    gchar **parts;
    guint n;

    if (!text || !*text)
        return out;
    parts = g_strsplit(text, ";", -1);
    for (n = 0; parts[n]; n++) {
        g_strstrip(parts[n]);
        if (!*parts[n])
            continue;
        g_ptr_array_add(out, g_strdup(parts[n]));
    }
    g_strfreev(parts);
    return out;
}

char *sensor_config_join(const GPtrArray *items)
{
    GString *s = g_string_new(NULL);

    if (items) {
        for (guint i = 0; i < items->len; i++) {
            const char *v = g_ptr_array_index(items, i);
            if (i > 0)
                g_string_append_c(s, ';');
            g_string_append(s, v);
        }
    }
    return g_string_free(s, FALSE);
}
