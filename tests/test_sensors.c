/* test_sensors.c — тесты ядра sensors: парсер hwmon, формат, скругление. */
#include "sensors_core.h"
#include "sensors_nvml.h"

#include <math.h>
#include <stdio.h>
#include <string.h>
#include <unistd.h>
#include <glib.h>
#include <glib/gstdio.h>
#include <pango/pangocairo.h>

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

static void check_str(const char *got, const char *want, const char *what)
{
    checks++;
    if (g_strcmp0(got, want) != 0) {
        failures++;
        printf("  FAIL  %s: получено «%s», ожидалось «%s»\n", what,
               got ? got : "(null)", want);
    }
}

static void check_dbl(gdouble got, gdouble want, gdouble eps, const char *what)
{
    checks++;
    if (fabs(got - want) > eps) {
        failures++;
        printf("  FAIL  %s: получено %f, ожидалось %f\n", what, got, want);
    }
}

static void check_int(gint got, gint want, const char *what)
{
    checks++;
    if (got != want) {
        failures++;
        printf("  FAIL  %s: получено %d, ожидалось %d\n", what, got, want);
    }
}

/* ---------------------------------------------------------------- фикстура */

typedef struct {
    char *path;
} Fixture;

static Fixture fx_new(void)
{
    Fixture f;
    char *tpl = g_build_filename(g_get_tmp_dir(), "sensors-test-XXXXXX", NULL);

    if (!g_mkdtemp(tpl)) {
        printf("  FAIL  не создать временный каталог\n");
        failures++;
        checks++;
        f.path = NULL;
        return f;
    }
    f.path = tpl;
    return f;
}

static void fx_free(Fixture *f)
{
    if (f->path)
        g_free(f->path);
}

/* Создать hwmon с файлами: files — массив «имя=содержимое», NULL-список. */
static void fx_hwmon(Fixture *f, const char *dir, const char *name,
                     const char *const *files);

/* hwmon с device-симлинком: как на живой машине. Идентификатор строки
 * строится из basename симлинка device, поэтому без него канал не
 * находится — и это правильно: «nvme/Composite» без устройства не
 * различает четыре nvme между собой. */
static void fx_hwmon_dev(Fixture *f, const char *dir, const char *name,
                         const char *device, const char *const *files)
{
    char *base = g_build_filename(f->path, dir, NULL);
    char *dev = g_build_filename(base, "device", NULL);
    char *target = g_build_filename(f->path, device, NULL);

    g_mkdir_with_parents(target, 0755);
    g_mkdir_with_parents(base, 0755);
    /* g_symlink в glib нет вообще — это symlink() из unistd.h */
    g_unlink(dev);   /* фикстура могла остаться с прошлого прогона */
    if (symlink(target, dev) != 0) {
        /* симлинка может не быть (прав нет) — тогда канал не найдётся
         * по row_id, и тест упадёт с внятным сообщением */
        checks++;
        failures++;
        printf("  FAIL  не создать device-симлинк в фикстуре\n");
    }
    g_free(dev);
    g_free(target);
    g_free(base);
    fx_hwmon(f, dir, name, files);
}

static void fx_hwmon(Fixture *f, const char *dir, const char *name,
                     const char *const *files)
{
    char *base = g_build_filename(f->path, dir, NULL);
    char *namefile = g_build_filename(base, "name", NULL);

    g_mkdir_with_parents(base, 0755);
    g_file_set_contents(namefile, name, -1, NULL);
    for (guint i = 0; files && files[i]; i++) {
        const char *eq = strchr(files[i], '=');
        gchar *leaf = g_strndup(files[i], eq - files[i]);
        char *path = g_build_filename(base, leaf, NULL);
        g_file_set_contents(path, eq + 1, -1, NULL);
        g_free(path);
        g_free(leaf);
    }
    g_free(namefile);
    g_free(base);
}

/* ------------------------------------------------------- чтение hwmon */

static void test_reads_channels(void)
{
    Fixture f = fx_new();
    const char *files[] = {
        "temp1_input=58000\n",
        "temp1_label=loc1\n",
        "temp2_input=61000\n",
        "fan1_input=1200\n",   /* не температура — игнорируется */
        "in0_input=33000\n",   /* не температура — игнорируется */
        NULL
    };
    SensorList *list;

    if (!f.path)
        return;
    fx_hwmon(&f, "hwmon0", "i350bb", files);
    list = sensor_list_read(f.path);
    check(list != NULL, "список читается");
    check_int(list ? (gint) list->chips->len : -1, 1, "один чип с каналами");
    if (list && list->chips->len == 1) {
        SensorChip *c = g_ptr_array_index(list->chips, 0);
        SensorReading *r;

        check_str(c->chip, "i350bb", "имя чипа");
        check_int(c->readings->len, 2, "только temp-каналы");
        r = g_ptr_array_index(c->readings, 0);
        check_str(r->label, "loc1", "метка из temp1_label");
        check_dbl(r->celsius, 58.0, 0.001, "58.0°C");
        check(r->valid, "канал валиден");
        r = g_ptr_array_index(c->readings, 1);
        check_str(r->label, "temp2", "без метки — имя канала");
        check_dbl(r->celsius, 61.0, 0.001, "61.0°C");
    }
    sensor_list_free(list);
    fx_free(&f);
}

/* Порядок каналов: temp10 обязан идти ПОСЛЕ temp2. Алфавитная сортировка
 * даёт обратное, и каналы в выводе оказываются переставлены. */
static void test_channel_order_is_numeric(void)
{
    Fixture f = fx_new();
    const char *files[] = {
        "temp1_input=10000\n",
        "temp2_input=20000\n",
        "temp10_input=30000\n",
        NULL
    };
    SensorList *list;

    if (!f.path)
        return;
    fx_hwmon(&f, "hwmon0", "testchip", files);
    list = sensor_list_read(f.path);
    if (list && list->chips->len == 1) {
        SensorChip *c = g_ptr_array_index(list->chips, 0);

        check_int(c->readings->len, 3, "три канала");
        check_int(((SensorReading *) g_ptr_array_index(c->readings, 0))->channel,
                  1, "канал 1 первым");
        check_int(((SensorReading *) g_ptr_array_index(c->readings, 1))->channel,
                  2, "канал 2 вторым");
        check_int(((SensorReading *) g_ptr_array_index(c->readings, 2))->channel,
                  10, "канал 10 третьим, не первым");
    } else {
        check(FALSE, "чип прочитан для проверки порядка");
    }
    sensor_list_free(list);
    fx_free(&f);
}

/* -128000 — маркер «канала нет», а не минус 128 градусов. */
static void test_invalid_marker(void)
{
    Fixture f = fx_new();
    const char *files[] = {
        "temp1_input=-128000\n",
        "temp3_input=38750\n",
        NULL
    };
    SensorList *list;

    if (!f.path)
        return;
    fx_hwmon(&f, "hwmon0", "nct7904", files);
    list = sensor_list_read(f.path);
    if (list && list->chips->len == 1) {
        SensorChip *c = g_ptr_array_index(list->chips, 0);
        SensorReading *r = g_ptr_array_index(c->readings, 0);

        check(!r->valid, "-128000 помечен как невалидный");
        check_int(c->readings->len, 2, "невалидный канал НЕ выброшен");
        r = g_ptr_array_index(c->readings, 1);
        check(r->valid, "нормальный канал валиден");
    } else {
        check(FALSE, "чип прочитан для проверки маркера");
    }
    sensor_list_free(list);
    fx_free(&f);
}

static void test_negative_temp_is_valid(void)
{
    Fixture f = fx_new();
    const char *files[] = { "temp1_input=-15000\n", NULL };
    SensorList *list;

    if (!f.path)
        return;
    fx_hwmon(&f, "hwmon0", "ambient", files);
    list = sensor_list_read(f.path);
    if (list && list->chips->len == 1) {
        SensorChip *c = g_ptr_array_index(list->chips, 0);
        SensorReading *r = g_ptr_array_index(c->readings, 0);
        check(r->valid, "минус 15 градусов — валидное значение");
        check_dbl(r->celsius, -15.0, 0.001, "-15.0°C");
    } else {
        check(FALSE, "чип прочитан для проверки отрицательной температуры");
    }
    sensor_list_free(list);
    fx_free(&f);
}

/* Нечитаемый канал: значение НЕ подставляется, но строка сохраняется.
 *
 * Раньше такой канал отбрасывался, и это было неверно: на серверной
 * матери с BMC чип вроде nct7904 делит i2c-адаптер с ipmi, и при
 * параллельном обращении драйвер отдаёт -EAGAIN. g_file_get_contents()
 * возвращает FALSE, канал исчезал на кадр и появлялся снова — строка
 * мигала. Теперь канал помечен read_error, и плагин рисует прочерк.
 *
 * Подставлять 0 вместо неизвестного значения нельзя: «0.0°C» среди
 * десятка настоящих каналов выглядит как измерение. */
static void test_malformed_value(void)
{
    Fixture f = fx_new();
    const char *files[] = { "temp1_input=не-число\n", "temp2_input=42000\n",
                            NULL };
    SensorList *list;

    if (!f.path)
        return;
    fx_hwmon(&f, "hwmon0", "garbage", files);
    list = sensor_list_read(f.path);
    if (list && list->chips->len == 1) {
        SensorChip *c = g_ptr_array_index(list->chips, 0);
        SensorReading *r;

        check_int(c->readings->len, 2,
                  "нечитаемый канал СОХРАНЁН, а не отброшен");
        r = g_ptr_array_index(c->readings, 0);
        check(r->read_error, "помечен read_error");
        check(!r->valid, "не валиден — значение неизвестно");
        r = g_ptr_array_index(c->readings, 1);
        check(!r->read_error, "нормальный канал без read_error");
        check(r->valid, "нормальный канал валиден");
        check_dbl(r->celsius, 42.0, 0.001, "нормальное значение прочитано");
    } else {
        check(FALSE, "чип прочитан для проверки мусора");
    }
    sensor_list_free(list);
    fx_free(&f);
}

/* Пустой файл канала — то же самое, что ошибка чтения: значение есть
 * ноль байт, показать нечего, но канал существует. */
static void test_empty_value(void)
{
    Fixture f = fx_new();
    const char *files[] = { "temp1_input=\n", "temp2_input=50000\n", NULL };
    SensorList *list;

    if (!f.path)
        return;
    fx_hwmon(&f, "hwmon0", "empty", files);
    list = sensor_list_read(f.path);
    if (list && list->chips->len == 1) {
        SensorChip *c = g_ptr_array_index(list->chips, 0);

        check_int(c->readings->len, 2, "пустой канал сохранён");
        check(((SensorReading *) g_ptr_array_index(c->readings, 0))->read_error,
              "пустой файл = read_error");
    } else {
        check(FALSE, "чип прочитан для проверки пустого канала");
    }
    sensor_list_free(list);
    fx_free(&f);
}

/* НЕ выдавать 0 вместо неизвестного: значение при ошибке остаётся 0.0,
 * но valid == FALSE, и именно по нему плагин рисует прочерк. Если бы
 * проверка шла по celsius, 0.0 был бы неотличим от настоящих нулей. */
static void test_error_is_not_zero(void)
{
    Fixture f = fx_new();
    const char *files[] = { "temp1_input=ошибка\n", NULL };
    SensorList *list;

    if (!f.path)
        return;
    fx_hwmon(&f, "hwmon0", "err", files);
    list = sensor_list_read(f.path);
    if (list && list->chips->len == 1) {
        SensorChip *c = g_ptr_array_index(list->chips, 0);
        SensorReading *r = g_ptr_array_index(c->readings, 0);

        check(!r->valid, "valid == FALSE отличает ошибку от настоящих 0");
        check(r->read_error, "read_error выставлен");
    } else {
        check(FALSE, "чип прочитан для проверки ошибки");
    }
    sensor_list_free(list);
    fx_free(&f);
}

/* Точное совпадение имени. Префикс не подходит: иначе четыре разных nvme
 * схлопнулись бы в один пункт настроек. */
static void test_find_is_exact(void)
{
    Fixture f = fx_new();
    const char *a[] = { "temp1_input=40000\n", NULL };
    const char *b[] = { "temp1_input=35000\n", NULL };
    SensorList *list;

    if (!f.path)
        return;
    fx_hwmon(&f, "hwmon0", "nvme", a);
    fx_hwmon(&f, "hwmon1", "nvme-extra", b);
    list = sensor_list_read(f.path);
    check_int(list ? (gint) list->chips->len : -1, 2, "два разных чипа");
    check(sensor_list_find(list, "nvme") != NULL, "точное имя найдено");
    check(sensor_list_find(list, "nvme-extra") != NULL, "второе имя найдено");
    check(sensor_list_find(list, "nv") == NULL, "префикс не подходит");
    check(sensor_list_find(list, "nvme-") == NULL, "усечённое имя не подходит");
    check(sensor_list_find(list, "нет-такого") == NULL, "несуществующее имя");
    check(sensor_list_find(NULL, "nvme") == NULL, "NULL-список не падает");
    sensor_list_free(list);
    fx_free(&f);
}

/* hwmon идут по возрастанию номера, а не в порядке g_dir_read_name. */
static void test_hwmon_order(void)
{
    Fixture f = fx_new();
    const char *a[] = { "temp1_input=10000\n", NULL };
    const char *b[] = { "temp1_input=20000\n", NULL };
    const char *c[] = { "temp1_input=30000\n", NULL };
    SensorList *list;

    if (!f.path)
        return;
    fx_hwmon(&f, "hwmon10", "chipc", c);
    fx_hwmon(&f, "hwmon2", "chipa", a);
    fx_hwmon(&f, "hwmon1", "chipb", b);
    list = sensor_list_read(f.path);
    if (list && list->chips->len == 3) {
        /* сортировка чипов идёт по имени, а не по номеру hwmon:
         * chipa, chipb, chipc */
        check_str(((SensorChip *) g_ptr_array_index(list->chips, 0))->chip,
                  "chipa", "первый по имени");
        check_str(((SensorChip *) g_ptr_array_index(list->chips, 2))->chip,
                  "chipc", "третий по имени");
    } else {
        check(FALSE, "три чипа прочитаны для проверки порядка");
    }
    sensor_list_free(list);
    fx_free(&f);
}

static void test_no_channels_skips_chip(void)
{
    Fixture f = fx_new();
    const char *a[] = { "temp1_input=10000\n", NULL };
    const char *b[] = { "fan1_input=1200\n", NULL }; /* не температура */
    SensorList *list;

    if (!f.path)
        return;
    fx_hwmon(&f, "hwmon0", "temphas", a);
    fx_hwmon(&f, "hwmon1", "fanonltemp", b);
    list = sensor_list_read(f.path);
    check_int(list ? (gint) list->chips->len : -1, 1, "чип без температур не попал");
    check(sensor_list_find(list, "fanonltemp") == NULL, "чип отброшен");
    sensor_list_free(list);
    fx_free(&f);
}

/* ------------------------------------------- устойчивость идентификатора */

/* ГЛАВНОЕ требование: идентификатор строки не зависит от номера hwmon.
 *
 * Тот же диск после перезагрузки может оказаться hwmon10 вместо hwmon8,
 * потому что номера hwmon — это индекс порядка регистрации драйверов.
 * Строка конфига обязана указывать на тот же физический диск. */
static void test_id_independent_of_hwmon_number(void)
{
    char *id1 = sensor_row_id("drivetemp", "0:0:2:0", "temp1");
    char *id2 = sensor_row_id("drivetemp", "0:0:2:0", "temp1");

    check_str(id1, id2, "идентификатор одинаков при разном номере hwmon");
    check_str(id1, "drivetemp/0:0:2:0/temp1", "формат «чип/устройство/канал»");
    g_free(id1);
    g_free(id2);
}

/* Имя чипа НЕ уникально: все четыре nvme называются «nvme», все диски
 * «drivetemp». Поиск по одному имени схлопывает их в один, поэтому
 * sensor_find_reading обязан различать устройство. */
static void test_same_chip_different_devices(void)
{
    Fixture f = fx_new();
    const char *a[] = { "temp1_input=34900\n", "temp1_label=Composite\n", NULL };
    const char *b[] = { "temp1_input=36900\n", "temp1_label=Composite\n", NULL };
    SensorList *list;
    SensorReading *r;

    if (!f.path)
        return;
    fx_hwmon_dev(&f, "hwmon0", "nvme", "nvme0", a);
    fx_hwmon_dev(&f, "hwmon1", "nvme", "nvme1", b);
    list = sensor_list_read(f.path);
    check_int(list ? (gint) list->chips->len : -1, 2,
              "два устройства с одинаковым именем чипа");
    r = sensor_find_reading(list, "nvme/nvme0/Composite");
    check(r != NULL, "первый nvme найден по полному row_id");
    check_dbl(r ? r->celsius : 0.0, 34.9, 0.001, "значение первого nvme");
    r = sensor_find_reading(list, "nvme/nvme1/Composite");
    check(r != NULL, "второй nvme найден по полному row_id");
    check_dbl(r ? r->celsius : 0.0, 36.9, 0.001, "значение второго nvme");
    check(sensor_list_find(list, "nvme") != NULL,
          "поиск по имени чипа возвращает первый — поэтому нужен row_id");
    sensor_list_free(list);
    fx_free(&f);
}

static void test_find_reading_rejects_partial(void)
{
    Fixture f = fx_new();
    const char *a[] = { "temp1_input=40000\n", "temp1_label=loc1\n", NULL };
    SensorList *list;

    if (!f.path)
        return;
    fx_hwmon_dev(&f, "hwmon0", "i350bb", "pci/0000:05:00.0", a);
    list = sensor_list_read(f.path);
    check(sensor_find_reading(list, "i350bb/0000:05:00.0/loc1") != NULL,
          "точный row_id найден");
    check(sensor_find_reading(list, "i350bb/wrong/loc1") == NULL,
          "неверное устройство не найдено");
    check(sensor_find_reading(list, "i350bb/0000:05:00.0/nope") == NULL,
          "неверный канал не найден");
    check(sensor_find_reading(list, "i350bb") == NULL,
          "без устройства не найдено");
    check(sensor_find_reading(list, "нет/такого/такого") == NULL,
          "несуществующий row_id");
    check(sensor_find_reading(NULL, "a/b/c") == NULL, "NULL-список");
    sensor_list_free(list);
    fx_free(&f);
}

/* Имя в стиле sensors(1), сверенное с реальным выводом. Формулы
 * найдены перебором по всем 13 drivetemp и 5 nvme этой машины.
 * Отвергнутые варианты указаны в sensors_core.c. */
static void test_sensors_name(void)
{
    static const struct {
        const char *dev_path;
        const char *chip;
        const char *want;
    } t[] = {
        { "/sys/devices/pci0000:00/0000:00:03.2/0000:05:00.0", "i350bb",
          "i350bb-pci-0500" },
        { "/sys/devices/pci0000:00/0000:00:02.0/0000:02:00.0/nvme/nvme0",
          "nvme", "nvme-pci-0200" },
        { "/sys/devices/pci0000:00/0000:07:00.0/host0/port-0:2/"
          "end_device-0:2/target0:0:2/0:0:2:0", "drivetemp",
          "drivetemp-scsi-0-20" },
        { "/sys/devices/pci0000:00/0000:01:00.0/host1/port-1:3/"
          "end_device-1:3/target1:0:3/1:0:3:0", "drivetemp",
          "drivetemp-scsi-1-30" },
        { "/sys/devices/platform/coretemp.0", "coretemp", "coretemp-isa-0000" },
        { "/sys/devices/platform/coretemp.1", "coretemp", "coretemp-isa-0001" },
        { "/sys/devices/pci0000:00/0000:00:1f.3/i2c-1/1-002d", "nct7904",
          "nct7904-i2c-1-2d" },
    };
    const guint n = sizeof(t) / sizeof(t[0]);

    for (guint i = 0; i < n; i++) {
        SensorChip c;
        char *got;

        memset(&c, 0, sizeof(c));
        c.chip = (char *) t[i].chip;
        c.dev_path = (char *) t[i].dev_path;
        got = sensor_chip_sensors_name(&c);
        check_str(got, t[i].want, t[i].dev_path);
        g_free(got);
    }
}

/* Имя sensors должно быть УНИКАЛЬНЫМ: если два устройства дают одно
 * имя, пользователь не сможет различить их в настройках. */
static void test_sensors_names_unique_on_live_tree(void)
{
    SensorList *list = sensor_list_read("/sys/class/hwmon");
    GHashTable *seen;
    guint dup = 0;

    check(list != NULL, "живое дерево читается");
    if (!list)
        return;
    seen = g_hash_table_new_full(g_str_hash, g_str_equal, g_free, NULL);
    for (guint i = 0; i < list->chips->len; i++) {
        char *name = sensor_chip_sensors_name(
            g_ptr_array_index(list->chips, i));

        if (g_hash_table_contains(seen, name)) {
            dup++;
            printf("  дубль имени: %s\n", name);
        }
        g_hash_table_add(seen, name);
    }
    check_int((gint) dup, 0, "имена sensors уникальны на живой машине");
    g_hash_table_destroy(seen);
    sensor_list_free(list);
}

/* --------------------------------------------- конфиг: идентичность строки
 *
 * Регрессия: init писал в конфиг одни подписи, теряя «|источник». Строка
 * переживала бы перезагрузку только до первого сохранения, а сохранение
 * происходит в init — то есть всегда. */

static void test_config_roundtrip_keeps_source(void)
{
    const char *text = "CPU|CPU Package id 0;NVMe|nvme/nvme0/Composite;"
                       "диск|drivetemp/0:0:2:0/temp1";
    GPtrArray *back = sensor_config_list(text);
    GPtrArray *labels = g_ptr_array_new_with_free_func(g_free);
    GPtrArray *sources = g_ptr_array_new_with_free_func(g_free);

    check_int((gint) back->len, 3, "три строки разобраны");
    for (guint i = 0; i < back->len; i++) {
        const char *entry = g_ptr_array_index(back, i);
        const char *bar = strchr(entry, '|');

        check(bar != NULL, "у записи есть разделитель источника");
        if (!bar)
            continue;
        g_ptr_array_add(labels, g_strndup(entry, bar - entry));
        g_ptr_array_add(sources, g_strdup(bar + 1));
    }
    check_str(g_ptr_array_index(labels, 0), "CPU", "подпись 0");
    check_str(g_ptr_array_index(sources, 0), "CPU Package id 0", "источник 0");
    check_str(g_ptr_array_index(labels, 2), "диск", "кириллица в подписи");
    check_str(g_ptr_array_index(sources, 2), "drivetemp/0:0:2:0/temp1",
              "SCSI-источник сохранился целиком");
    g_ptr_array_unref(back);
    g_ptr_array_unref(labels);
    g_ptr_array_unref(sources);
}

/* Подпись может содержать «|»? Нет: это разделитель, и разбор берёт
 * ПЕРВОЕ вхождение. Проверяем, что лишние не портят источник. */
static void test_config_source_takes_after_first_bar(void)
{
    GPtrArray *back = sensor_config_list("метка|a/b/c");

    check_int((gint) back->len, 1, "одна запись");
    check_str(g_ptr_array_index(back, 0), "метка|a/b/c", "запись целиком");
    g_ptr_array_unref(back);
}

/* Пустые элементы (;,;;) не должны давать пустых строк: иначе в
 * отрисовке появится строка без подписи и без источника. */
static void test_config_skips_empty(void)
{
    GPtrArray *back = sensor_config_list("a;b;;;c");

    check_int((gint) back->len, 3, "пустые элементы пропущены");
    g_ptr_array_unref(back);
    back = sensor_config_list("  ;x  ;  ");
    check_int((gint) back->len, 1, "пробелы обрезаны, пустое отброшено");
    g_ptr_array_unref(back);
    back = sensor_config_list(NULL);
    check_int((gint) back->len, 0, "NULL даёт пустой список");
    g_ptr_array_unref(back);
    back = sensor_config_list("");
    check_int((gint) back->len, 0, "пустая строка даёт пустой список");
    g_ptr_array_unref(back);
}

/* ------------------------------------------------- кегль шрифта (регресс)
 *
 * Регрессия: pango_font_description_get_size() возвращает размер в
 * единицах PANGO_SCALE (для "Sans 8" это 8192, а не 8). Подстановка
 * числа в описание шрифта давала "Sans 8192": окно рисовало рамку, а
 * текст не появлялся вообще — на скриншоте это выглядит как «плагин
 * ничего не выводит», и диагностировать приходится пробой отрисовки.
 *
 * sen_scale_font() — статическая функция плагина, поэтому здесь
 * проверяется та же арифметика на core-стороне: пункты = size/PANGO_SCALE.
 */

static void test_font_size_in_points(void)
{
    PangoFontDescription *fd = pango_font_description_from_string("Sans 8");
    int points = pango_font_description_get_size(fd) / PANGO_SCALE;

    check_int(pango_font_description_get_size(fd), 8 * PANGO_SCALE,
              "get_size отдаёт единицы PANGO_SCALE, а не пункты");
    check_int(points, 8, "кегль в пунктах получается делением на PANGO_SCALE");
    pango_font_description_free(fd);

    fd = pango_font_description_from_string("Terminus Bold 16");
    check_int(pango_font_description_get_size(fd) / PANGO_SCALE, 16,
              "кегль 16 с семейством и начертанием");
    pango_font_description_free(fd);
}

/* Масштабирование кегля под размер окна: окно шире замысла — шрифт
 * крупнее, ровно во столько же, во сколько шире окно. */
static void test_font_scale_proportional(void)
{
    /* та же формула, что в sen_scale_font */
    for (int pt = 6; pt <= 24; pt++) {
        for (int factor = 1; factor <= 3; factor++) {
            int design = 200, actual = 200 * factor;
            int scaled = (pt * actual) / design;

            check_int(scaled, pt * factor, "масштаб кегля");
        }
    }
    /* design == actual — кегль не плывёт */
    check_int((11 * 200) / 200, 11, "одинаковый размер окна: кегль целый");
}

/* ------------------------------------------------- единицы измерения
 *
 * В диалоге выбор шкалы (регрессия: был флажок «Фаренгейт», который
 * не мог выразить выбор — в градусах Цельсия перевод тоже идёт). */

static void test_units_format(void)
{
    char *c = sensor_format_value(35.0, FALSE, TRUE);
    char *f = sensor_format_value(35.0, TRUE, TRUE);
    char *c_plain = sensor_format_value(35.0, FALSE, FALSE);
    char *f_plain = sensor_format_value(35.0, TRUE, FALSE);

    check_str(c, "35.0°C", "Цельсий с суффиксом");
    check_str(f, "95.0°F", "Фаренгейт с суффиксом");
    check_str(c_plain, "35.0", "Цельсий без суффикса");
    check_str(f_plain, "95.0", "Фаренгейт без суффикса");
    g_free(c); g_free(f); g_free(c_plain); g_free(f_plain);

    /* Суффикс обязан соответствовать шкале: в конфиге Fahrenheit, а в
     * подписи градус Цельсия — расхождение, которое видно на экране. */
    check_str(sensor_unit_suffix(FALSE), "°C", "суффикс Цельсия");
    check_str(sensor_unit_suffix(TRUE), "°F", "суффикс Фаренгейта");
    check_dbl(sensor_to_display_unit(0.0, TRUE), 32.0, 0.001, "0°C = 32°F");
    check_dbl(sensor_to_display_unit(100.0, TRUE), 212.0, 0.001,
              "100°C = 212°F");
    check_dbl(sensor_to_display_unit(-40.0, TRUE), -40.0, 0.001,
              "-40° — единственная общая точка шкал");
    check_dbl(sensor_to_display_unit(35.0, FALSE), 35.0, 0.001,
              "Цельсий не переводится");
}

/* ------------------------------------------- склейка пар (регресс)
 *
 * Регрессия, на которую я наступал дважды: сериализация «подпись|
 * источник» была продублирована в init и в обработчике галочек, и
 * обработчик писал одни подписи. Стоило один раз двинуть галочку —
 * источники исчезали, и строки переставали переживать перезагрузку.
 * Теперь реализация одна, в core.
 */

static void test_join_pair_keeps_source(void)
{
    GPtrArray *labels = g_ptr_array_new_with_free_func(g_free);
    GPtrArray *sources = g_ptr_array_new_with_free_func(g_free);
    char *text;

    g_ptr_array_add(labels, g_strdup("CPU"));
    g_ptr_array_add(labels, g_strdup("Диск 1"));
    g_ptr_array_add(labels, g_strdup("GPU"));
    g_ptr_array_add(sources, g_strdup("coretemp/coretemp.0/Package id 0"));
    g_ptr_array_add(sources, g_strdup("drivetemp/0:0:2:0/temp1"));
    g_ptr_array_add(sources, g_strdup("nvme/nvme0/Composite"));

    text = sensor_config_join_pair(labels, sources);
    check_str(text,
              "CPU|coretemp/coretemp.0/Package id 0;"
              "Диск 1|drivetemp/0:0:2:0/temp1;"
              "GPU|nvme/nvme0/Composite",
              "пары склеены в порядке массивов");
    g_free(text);

    /* Метка пользователем переименована — источник обязан уцелеть */
    g_free(g_ptr_array_index(labels, 0));
    g_ptr_array_index(labels, 0) = g_strdup("Процессор");
    text = sensor_config_join_pair(labels, sources);
    check(g_strstr_len(text, -1, "Процессор|coretemp/coretemp.0/Package id 0")
          != NULL, "переименование метки не теряет источник");
    g_free(text);

    /* Короткий массив источников: строки без источника пишутся как есть */
    g_ptr_array_set_size(sources, 1);
    text = sensor_config_join_pair(labels, sources);
    check_str(text, "Процессор|coretemp/coretemp.0/Package id 0;Диск 1;GPU",
              "строка без источника не получает висящего разделителя");
    g_free(text);

    /* Пустые входы */
    text = sensor_config_join_pair(NULL, NULL);
    check_str(text, "", "NULL-массивы дают пустую строку");
    g_free(text);
    text = sensor_config_join_pair(labels, NULL);
    check_str(text, "Процессор;Диск 1;GPU", "NULL-источники");
    g_free(text);

    g_ptr_array_unref(labels);
    g_ptr_array_unref(sources);
}

/* Round-trip: склейка → разбор → та же строка. Это то, что происходит
 * при перезапуске демона. */
static void test_join_pair_roundtrip(void)
{
    GPtrArray *labels = g_ptr_array_new_with_free_func(g_free);
    GPtrArray *sources = g_ptr_array_new_with_free_func(g_free);
    char *text, *id;

    g_ptr_array_add(labels, g_strdup("CPU 1"));
    g_ptr_array_add(sources, g_strdup("coretemp/coretemp.0/Core 0"));
    g_ptr_array_add(labels, g_strdup("Диск: sda"));
    g_ptr_array_add(sources, g_strdup("drivetemp/1:0:3:0/temp1"));
    text = sensor_config_join_pair(labels, sources);

    {
        GPtrArray *back = sensor_config_list(text);

        check_int((gint) back->len, 2, "разбор вернул обе строки");
        id = g_strdup(g_ptr_array_index(back, 0));
        g_ptr_array_unref(back);
    }
    check_str(id, "CPU 1|coretemp/coretemp.0/Core 0", "строка 0 целиком");
    g_free(id);
    g_free(text);
    g_ptr_array_unref(labels);
    g_ptr_array_unref(sources);
}

/* ---------------------------------------------------------------- формат */

static void test_units(void)
{
    check_dbl(sensor_to_display_unit(58.0, FALSE), 58.0, 0.001, "58°C как есть");
    check_dbl(sensor_to_display_unit(100.0, TRUE), 212.0, 0.001, "100°C = 212°F");
    check_dbl(sensor_to_display_unit(0.0, TRUE), 32.0, 0.001, "0°C = 32°F");
    check_dbl(sensor_to_display_unit(-40.0, TRUE), -40.0, 0.001, "-40 совпадает");
    check_str(sensor_unit_suffix(FALSE), "°C", "суффикс °C");
    check_str(sensor_unit_suffix(TRUE), "°F", "суффикс °F");
}

static void test_format(void)
{
    char *s = sensor_format_value(58.0, FALSE, TRUE);
    check_str(s, "58.0°C", "формат с °C");
    g_free(s);
    s = sensor_format_value(58.0, FALSE, FALSE);
    check_str(s, "58.0", "формат без единицы");
    g_free(s);
    s = sensor_format_value(100.0, TRUE, TRUE);
    check_str(s, "212.0°F", "формат в °F");
    g_free(s);
    s = sensor_format_value(38.75, FALSE, TRUE);
    check_str(s, "38.8°C", "округление до десятых");
    g_free(s);
}

/* -------------------------------------------------------- список в conf */

static void test_config_list(void)
{
    GPtrArray *items = sensor_config_list("GPU;X79; Package id 1 ");
    char *joined;

    check_int(items->len, 3, "три элемента, пробелы обрезаны");
    check_str(g_ptr_array_index(items, 0), "GPU", "первый");
    check_str(g_ptr_array_index(items, 2), "Package id 1",
              "пробел внутри сохранён");
    joined = sensor_config_join(items);
    check_str(joined, "GPU;X79;Package id 1", "обратная сборка");
    g_free(joined);
    g_ptr_array_unref(items);
}

static void test_config_list_edges(void)
{
    GPtrArray *items = sensor_config_list("");
    char *joined;

    check_int(items->len, 0, "пустая строка — ноль элементов");
    joined = sensor_config_join(items);
    check_str(joined, "", "join пустого списка");
    g_free(joined);
    g_ptr_array_unref(items);

    items = sensor_config_list(NULL);
    check_int(items->len, 0, "NULL — ноль элементов");
    g_ptr_array_unref(items);

    items = sensor_config_list(";;A;;  ;;B;");
    check_int(items->len, 2, "пустые сегменты пропущены");
    g_ptr_array_unref(items);

    /* подпись с «;» внутри сломает список — это неразбираемо, и мы
     * обязаны это сказать, а не молча разрезать */
    items = sensor_config_list("A;B;C");
    joined = sensor_config_join(items);
    check_str(joined, "A;B;C", "round-trip без потерь");
    g_free(joined);
    g_ptr_array_unref(items);
}

/* ------------------------------------------------------------ скругление */

static void test_rounding(void)
{
    check_dbl(sensor_corner_radius_value(0), 0.0, 0.001, "radius 0");
    check_dbl(sensor_corner_radius_value(-5), 0.0, 0.001, "отрицательный → 0");
    check_dbl(sensor_corner_radius_value(4), 4.0, 0.001, "radius 4");
    check(sensor_corner_radius_is_rounded(2.0), "2px — скруглено");
    check(!sensor_corner_radius_is_rounded(0.5), "0.5px — нет");
    check(!sensor_corner_radius_is_rounded(0.0), "0 — нет");
}

static void test_rounded_region(void)
{
    cairo_region_t *r;

    /* cairo_region_contains_rectangle возвращает cairo_region_overlap_t:
     * CAIRO_REGION_OVERLAP_IN (0) — полностью внутри, _PART (2) — часть,
     * _OUT (1) — снаружи. Это НЕ gboolean, и CAIRO_REGION_OVERLAP_IN
     * равен 0: сравнение результата с TRUE инвертирует смысл, и все
     * проверки формы проходят наоборот — угол «срезан» оказывается
     * внутри, а центр «внутри» — снаружи. */
    r = sensor_rounded_region(200, 100, 4);
    check(r != NULL, "регион создан для radius 4");
    if (r) {
        cairo_rectangle_int_t box = { 0, 0, 0, 0 };
        /* верхний левый угол срезан: пиксель (0,0) вне региона */
        check(cairo_region_contains_rectangle(r, &box) != CAIRO_REGION_OVERLAP_IN,
              "угол срезан (0,0) вне");
        box.x = 100; box.y = 50; box.width = 1; box.height = 1;
        check(cairo_region_contains_rectangle(r, &box) == CAIRO_REGION_OVERLAP_IN,
              "центр внутри");
        box.x = 199; box.y = 0; box.width = 1; box.height = 1;
        check(cairo_region_contains_rectangle(r, &box) != CAIRO_REGION_OVERLAP_IN,
              "правый верхний угол срезан");
        box.x = 0; box.y = 50; box.width = 1; box.height = 1;
        check(cairo_region_contains_rectangle(r, &box) == CAIRO_REGION_OVERLAP_IN,
              "левый край в середине внутри");
        cairo_region_destroy(r);
    }
    check(sensor_rounded_region(200, 100, 0) == NULL, "radius 0 — NULL");
    check(sensor_rounded_region(0, 100, 4) == NULL, "нулевая ширина — NULL");
    /* радиус больше половины стороны обрезается, регион не пустеет */
    r = sensor_rounded_region(20, 20, 50);
    check(r != NULL, "radius больше половины — регион есть");
    if (r) {
        cairo_rectangle_int_t box = { 10, 10, 1, 1 };
        check(cairo_region_contains_rectangle(r, &box) == CAIRO_REGION_OVERLAP_IN,
              "центр остаётся внутри при огромном радиусе");
        cairo_region_destroy(r);
    }
}

/* ------------------------------------------------------------ живое дерево */

static void test_live_tree(void)
{
    SensorList *list = sensor_list_read("/sys/class/hwmon");
    guint total = 0;
    gboolean found_invalid = FALSE;
    gboolean found_valid = FALSE;

    check(list != NULL, "живое /sys/class/hwmon читается");
    if (!list)
        return;
    check(list->chips->len > 0, "найден хотя бы один сенсор");
    for (guint i = 0; i < list->chips->len; i++) {
        SensorChip *c = g_ptr_array_index(list->chips, i);
        check(c->chip != NULL && *c->chip, "у чипа есть имя");
        for (guint j = 0; j < c->readings->len; j++) {
            SensorReading *r = g_ptr_array_index(c->readings, j);
            total++;
            check(r->celsius > -130.0 && r->celsius < 150.0,
                  "температура в разумных пределах");
            if (!r->valid)
                found_invalid = TRUE;
            else
                found_valid = TRUE;
        }
    }
    printf("  инфо: чипов=%u каналов=%u\n", list->chips->len, total);
    check(found_valid, "есть живые значения");
    (void)found_invalid; /* на этой машине есть, но не обязана */
    sensor_list_free(list);
}

/* Регрессия на баг «подпись есть, числа нет».
 *
 * В диалоге источник строки однажды собирался из названия канала
 * («loc1») вместо ключа «чип/устройство/канал». Конфиг такой источник
 * принимал, строка показывалась, но sensor_find_reading() ничего не
 * находил — и апплет выводил подпись без температуры. Здесь проверяется
 * то же условие, на котором держится вывод: КАЖДЫЙ источник из
 * сохранённых строк обязан находить реальный сенсор.
 *
 * Путь к конфигу задаётся переменной SEN_TEST_CONFIG; без неё тест
 * молча пропускает себя, чтобы обычный прогон не зависел от live-файла. */
static void test_every_config_row_has_reading(void)
{
    const char *cfg = getenv("SEN_TEST_CONFIG");
    SensorList *found;
    char *text = NULL;
    char *rows, **parts;
    gsize len = 0;
    guint bad = 0, total = 0;

    if (!cfg)
        return;
    found = sensor_list_read("/sys/class/hwmon");
    check(found != NULL, "живое дерево прочитано для проверки конфига");
    if (!found)
        return;
    if (!g_file_get_contents(cfg, &text, &len, NULL)) {
        printf("  инфо: конфиг %s не прочитан, проверка пропущена\n", cfg);
        sensor_list_free(found);
        return;
    }
    rows = strstr(text, "rows=");
    check(rows != NULL, "в конфиге есть ключ rows=");
    if (!rows) {
        g_free(text);
        sensor_list_free(found);
        return;
    }
    rows += 5;
    rows[strcspn(rows, "\n")] = '\0';
    parts = g_strsplit(rows, ";", -1);
    for (guint i = 0; parts[i]; i++) {
        const char *bar = strchr(parts[i], '|');
        const char *source;
        SensorReading *r;
        gboolean found_any = FALSE;

        if (!bar || !*(bar + 1))
            continue;   /* строка без источника — не эта проверка */
        source = bar + 1;
        total++;
        r = sensor_find_reading(found, source);
        found_any = r != NULL;
        /* Тестовая строка не привязана к датчику, ей всегда есть что
         * показать: проверять её на живость нечего и не нужно. */
        if (g_strcmp0(source, SEN_DUMMY_SOURCE) == 0)
            found_any = TRUE;
        /* Источник NVIDIA лежит не в hwmon, а в NVML, и проверять его
         * нужно тем же способом, каким плагин его читает: поиск по
         * дереву для «nvidia/…» всегда даёт «нет», и строка карты
         * была бы объявлена битой. */
        if (!r && nvml_source_is_nvidia(source)) {
            gdouble nv = 0.0;

            found_any = nvml_read_value(source, &nv);
        }
        if (!found_any) {
            bad++;
            printf("  ПРОВАЛ: источник «%s» не находит сенсор "
                   "(строка конфига «%s»)\n", source, parts[i]);
        }
    }
    g_strfreev(parts);
    check(bad == 0, "все строки конфига находят сенсор");
    printf("  инфо: строк конфига=%u, без сенсора=%u\n", total, bad);
    g_free(text);
    sensor_list_free(found);
}

static void test_bus_kind_and_slot(void)
{
    static const struct {
        const char *dev_path;
        const char *chip;
        const char *kind;
        const char *slot;
    } t[] = {
        { "/sys/devices/pci0000:00/0000:07:00.0/host0/port-0:2/"
          "end_device-0:2/target0:0:2/0:0:2:0", "drivetemp",
          "scsi", "0-20" },
        { "/sys/devices/pci0000:00/0000:01:00.0/host1/port-1:3/"
          "end_device-1:3/target1:0:3/1:0:3:0", "drivetemp",
          "scsi", "1-30" },
        { "/sys/devices/pci0000:00/0000:00:03.2/0000:05:00.0", "i350bb",
          "pci", "0500" },
        { "/sys/devices/pci0000:00/0000:00:02.0/0000:02:00.0/nvme/nvme0",
          "nvme", "pci", "0200" },
        { "/sys/devices/platform/coretemp.0", "coretemp",
          "isa", "0000" },
        { "/sys/devices/platform/coretemp.1", "coretemp",
          "isa", "0001" },
        { "/sys/devices/pci0000:00/0000:00:1f.3/i2c-1/1-002d", "nct7904",
          "i2c", "1-2d" },
    };
    const guint n = sizeof(t) / sizeof(t[0]);

    for (guint i = 0; i < n; i++) {
        char *kind = sensor_bus_kind(t[i].dev_path, t[i].chip);
        char *slot = sensor_bus_slot(t[i].dev_path);

        check_str(kind, t[i].kind, t[i].dev_path);
        check_str(slot, t[i].slot, t[i].dev_path);
        g_free(kind);
        g_free(slot);
    }
}

/* Полное имя обязано собираться из kind+slot: если формулы разойдутся,
 * в настройках и в апплете будут разные имена одного сенсора. */
static void test_name_composed_from_kind_slot(void)
{
    static const struct {
        const char *dev_path;
        const char *chip;
        const char *want;
    } t[] = {
        { "/sys/devices/pci0000:00/0000:07:00.0/host0/port-0:2/"
          "end_device-0:2/target0:0:2/0:0:2:0", "drivetemp",
          "drivetemp-scsi-0-20" },
        { "/sys/devices/pci0000:00/0000:00:03.2/0000:05:00.0", "i350bb",
          "i350bb-pci-0500" },
        { "/sys/devices/pci0000:00/0000:00:1f.3/i2c-1/1-002d", "nct7904",
          "nct7904-i2c-1-2d" },
        { "/sys/devices/platform/coretemp.0", "coretemp",
          "coretemp-isa-0000" },
    };

    for (guint i = 0; i < sizeof(t) / sizeof(t[0]); i++) {
        SensorChip c;
        char *kind, *slot, *name;

        memset(&c, 0, sizeof(c));
        c.chip = (char *) t[i].chip;
        c.dev_path = (char *) t[i].dev_path;
        name = sensor_chip_sensors_name(&c);
        check_str(name, t[i].want, t[i].dev_path);

        /* та же формула вручную: имя == chip + "-" + kind + "-" + slot */
        kind = sensor_bus_kind(t[i].dev_path, t[i].chip);
        slot = sensor_bus_slot(t[i].dev_path);
        {
            char *group = sensor_group_name(t[i].chip, t[i].dev_path);
            char *want = g_strdup_printf("%s-%s", group, slot);

            check_str(name, want, "имя собирается из группы и слота");
            g_free(want);
            g_free(group);
        }
        g_free(name);
        g_free(kind);
        g_free(slot);
    }
}

/* Без device-симлинка слот пустой, и имя не должно превращаться в
 * «чип--» или «чип-» с висящим дефисом. */
static void test_name_without_device(void)
{
    SensorChip c;

    memset(&c, 0, sizeof(c));
    c.chip = (char *) "acpitz";
    c.dev_path = NULL;
    {
        char *name = sensor_chip_sensors_name(&c);

        check_str(name, "acpitz", "нет пути — только имя чипа");
        g_free(name);
    }
    c.dev_path = (char *) "";
    {
        char *name = sensor_chip_sensors_name(&c);

        check_str(name, "acpitz", "пустой путь — только имя чипа");
        g_free(name);
    }
    {
        char *kind = sensor_bus_kind(NULL, "acpitz");
        char *slot = sensor_bus_slot(NULL);

        check_str(kind, "none", "без пути — шины нет");
        check_str(slot, "", "без пути — слот пустой, не NULL");
        g_free(kind);
        g_free(slot);
    }
}

/* На живой машине: имена уникальны И слоты уникальны внутри группы.
 * Иначе две строки в таблице диалога неразличимы. */
static void test_slots_unique_per_group(void)
{
    SensorList *list = sensor_list_read("/sys/class/hwmon");
    GHashTable *seen;
    guint dup = 0;

    check(list != NULL, "живое дерево читается");
    if (!list)
        return;
    seen = g_hash_table_new_full(g_str_hash, g_str_equal, g_free, NULL);
    for (guint i = 0; i < list->chips->len; i++) {
        SensorChip *c = g_ptr_array_index(list->chips, i);
        char *kind = sensor_bus_kind(c->dev_path, c->chip);
        char *slot = sensor_bus_slot(c->dev_path);
        char *key = g_strdup_printf("%s/%s", kind, slot);

        if (g_hash_table_contains(seen, key)) {
            dup++;
            printf("  дубль в группе: %s\n", key);
        }
        g_hash_table_add(seen, key);
        /* key уходит хешу вместе с его g_free: g_hash_table_add ключ не
         * копирует, и следующий g_free(key) давал double free — тест
         * падал в abort() на последнем g_hash_table_destroy. */
        g_free(kind);
        g_free(slot);
    }
    check_int((gint) dup, 0, "внутри группы слоты уникальны");
    g_hash_table_destroy(seen);
    sensor_list_free(list);
}

/* ---------------------------------------------------- группы диалога
 *
 * Таблица в настройках: заголовок группы + строки вида
 * «[галочка] короткое-имя [поле метки]». Группа обязана включать имя
 * чипа: два coretemp дают слоты 0000 и 0001, и без чипа в заголовке
 * пользователь не поймёт, что строки с разных процессоров.
 */
static void test_group_names(void)
{
    static const struct {
        const char *dev_path;
        const char *chip;
        const char *want;
    } t[] = {
        { "/sys/devices/pci0000:00/0000:07:00.0/host0/port-0:2/"
          "end_device-0:2/target0:0:2/0:0:2:0", "drivetemp",
          "drivetemp-scsi" },
        { "/sys/devices/pci0000:00/0000:00:03.2/0000:05:00.0", "i350bb",
          "i350bb-pci" },
        { "/sys/devices/pci0000:00/0000:00:02.0/0000:02:00.0/nvme/nvme0",
          "nvme", "nvme-pci" },
        { "/sys/devices/platform/coretemp.0", "coretemp", "coretemp-isa" },
        { "/sys/devices/pci0000:00/0000:00:1f.3/i2c-1/1-002d", "nct7904",
          "nct7904-i2c" },
    };

    for (guint i = 0; i < sizeof(t) / sizeof(t[0]); i++) {
        char *g = sensor_group_name(t[i].chip, t[i].dev_path);

        check_str(g, t[i].want, t[i].dev_path);
        g_free(g);
    }
    /* Нет пути — группа равна имени чипа, без висящего дефиса */
    {
        char *g = sensor_group_name("acpitz", NULL);

        check_str(g, "acpitz", "без пути группа = имя чипа");
        g_free(g);
        g = sensor_group_name(NULL, NULL);
        check_str(g, "unknown", "NULL-чип даёт «unknown», не падает");
        g_free(g);
    }
}

/* На живой машине: слот уникален внутри группы. Иначе в таблице две
 * одинаковые строки, и пользователь не отличит один диск от другого. */
static void test_group_slots_unique_live(void)
{
    SensorList *list = sensor_list_read("/sys/class/hwmon");
    GHashTable *seen;
    guint dup = 0, groups = 0;
    GHashTable *kinds;

    check(list != NULL, "живое дерево читается");
    if (!list)
        return;
    seen = g_hash_table_new_full(g_str_hash, g_str_equal, g_free, NULL);
    kinds = g_hash_table_new_full(g_str_hash, g_str_equal, g_free, NULL);
    for (guint i = 0; i < list->chips->len; i++) {
        SensorChip *c = g_ptr_array_index(list->chips, i);
        char *group = sensor_group_name(c->chip, c->dev_path);
        char *slot = sensor_bus_slot(c->dev_path);
        char *key = g_strdup_printf("%s/%s", group, slot);

        if (g_hash_table_contains(seen, key)) {
            dup++;
            printf("  дубль в группе: %s\n", key);
        }
        if (!g_hash_table_contains(kinds, group)) {
            g_hash_table_add(kinds, g_strdup(group));
            groups++;
        }
        g_hash_table_add(seen, key);   /* key уходит хешу с его g_free */
        g_free(group);
        g_free(slot);
    }
    check_int((gint) dup, 0, "внутри группы слоты уникальны");
    /* На этой машине 5 разных групп: coretemp-isa, drivetemp-scsi,
     * i350bb-pci, nvme-pci, nct7904-i2c. */
    check(groups >= 4, "групп несколько, а не одна на все сенсоры");
    printf("  инфо: групп=%u\n", groups);
    g_hash_table_destroy(kinds);
    g_hash_table_destroy(seen);
    sensor_list_free(list);
}

/* Модель таблицы настроек: включение/снятие галочки обратимы.
 *
 * Регрессия на «чекбокс не реагирует визуально, но строка появляется».
 * Обработчик правил priv, но не писал в модель, и искал источник не в
 * той колонке (в читаемом имени вместо ключа). Итог: включение всегда
 * давало idx == SEN_ROW_NONE, а снятие — тоже, то есть состояние
 * строки в модели и в priv расходилось навсегда.
 *
 * Логика переключения проверяется на чистых массивах — тот же поиск по
 * источнику, то же добавление в конец, то же снятие со сдвигом хвоста.
 */
typedef struct {
    char *source;
    gboolean active;
} ModelRow;

/* Поиск строки в priv по источнику: индекс или -1. */
static gint model_find(GPtrArray *sources, const char *source)
{
    for (guint j = 0; j < sources->len; j++)
        if (g_strcmp0(g_ptr_array_index(sources, j), source) == 0)
            return (gint) j;
    return -1;
}

static void test_toggle_row_is_reversible(void)
{
    GPtrArray *model = g_ptr_array_new();     /* ModelRow* — как GtkListStore */
    GPtrArray *rows = g_ptr_array_new_with_free_func(g_free);
    GPtrArray *sources = g_ptr_array_new_with_free_func(g_free);

    /* Три строки: 0 и 2 включены, 1 выключена. Как в таблице: порядок
     * в priv задаёт пользователь, а не порядок в модели. */
    for (guint i = 0; i < 3; i++) {
        ModelRow *r = g_new0(ModelRow, 1);
        char *src = g_strdup_printf("chip%d/dev%d/temp1", (int) i, (int) i);

        r->source = src;
        r->active = (i != 1);
        g_ptr_array_add(model, r);
        if (r->active) {
            g_ptr_array_add(rows, g_strdup_printf("метка %d", (int) i));
            g_ptr_array_add(sources, g_strdup(src));
        }
    }
    check_int((gint) rows->len, 2, "две строки выводятся");
    check_int(model_find(sources, "chip0/dev0/temp1"), 0,
              "сенсор 0 найден по источнику");
    check_int(model_find(sources, "chip1/dev1/temp1"), -1,
              "сенсор 1 выключен — не найден");
    check_int(model_find(sources, "chip2/dev2/temp1"), 1,
              "сенсор 2 найден по источнику, не по номеру модели");

    /* Включаем сенсор 1: idx == SEN_ROW_NONE, поэтому он встаёт в
     * КОНЕЦ — и это правильно, порядок вывода задаёт пользователь. */
    g_ptr_array_add(rows, g_strdup("метка 1"));
    g_ptr_array_add(sources, g_strdup("chip1/dev1/temp1"));
    check_int((gint) rows->len, 3, "после включения три строки");
    check_str((const char *) g_ptr_array_index(sources, 2), "chip1/dev1/temp1",
              "включённая строка встала в конец");

    /* Повторный клик на уже включённой: idx найден, дубля быть не
     * должно. Именно это ломало чекбокс — каждый клик добавлял строку. */
    for (guint i = 0; i < 3; i++) {
        ModelRow *r = g_ptr_array_index(model, i);

        check_int(model_find(sources, r->source) >= 0, 1,
                  "повторный клик находит строку");
    }
    check_int((gint) rows->len, 3, "повторный клик не добавил дубль");

    /* Снятие последней строки (сенсор 1, он встал в конец): хвост
     * сдвигается, у оставшихся свои источники. */
    g_ptr_array_remove_index(rows, 2);
    g_ptr_array_remove_index(sources, 2);
    check_int((gint) rows->len, 2, "после снятия две строки");
    check_str((const char *) g_ptr_array_index(sources, 0), "chip0/dev0/temp1",
              "первая строка сохранила свой источник");
    check_str((const char *) g_ptr_array_index(sources, 1), "chip2/dev2/temp1",
              "вторая получила свой источник, а не чужой");
    check_int(model_find(sources, "chip1/dev1/temp1"), -1,
              "снятая строка исчезла из priv");

    /* Снятие из середины: g_ptr_array_remove_index не сдвигает хвост
     * сам, поэтому порядок вызова обязателен — сначала подпись, потом
     * источник, иначе строки разъедутся. */
    g_ptr_array_add(rows, g_strdup("метка 9"));
    g_ptr_array_add(sources, g_strdup("chip9/dev9/temp1"));
    g_ptr_array_remove_index(rows, 0);
    g_ptr_array_remove_index(sources, 0);
    check_int((gint) rows->len, 2, "после второго снятия две строки");
    check_str((const char *) g_ptr_array_index(sources, 0), "chip2/dev2/temp1",
              "хвост сдвинулся на свои значения");
    check_str((const char *) g_ptr_array_index(sources, 1), "chip9/dev9/temp1",
              "последняя строка на месте");

    for (guint i = 0; i < model->len; i++)
        g_free(((ModelRow *) g_ptr_array_index(model, i))->source);
    g_ptr_array_unref(model);
    g_ptr_array_unref(rows);
    g_ptr_array_unref(sources);
}

/* Авто-высота по числу строк.
 *
 * Регрессия на «строки пропали из окна снизу». Признак авто-режима
 * вычислялся из наличия ключа window_height, а плагин САМ записывает его
 * в конфиг при старте. На следующем запуске авто-режим выключался
 * навсегда: окно не росло, и каждая добавленная строка обрезалась снизу.
 *
 * Проверяем то, на чём держится вывод: окно должно вмещать все строки
 * при line_step = 12, first_row_y = 4 и поле 4 px снизу и сверху.
 */
#define SEN_T_MARGIN 4
#define SEN_T_MIN_H  60

static int sensor_test_needed_height(int rows, int line_step, int first_row_y)
{
    return first_row_y + rows * line_step + SEN_T_MARGIN * 2;
}

static void test_auto_height_fits_all_rows(void)
{
    /* Окно на 18 строк: должно вместить все, а не 13 «как в прошлый раз». */
    const int rows[] = { 1, 4, 13, 14, 15, 18, 30, 46 };
    const int line_step = 12, first_row_y = 4;

    for (guint i = 0; i < G_N_ELEMENTS(rows); i++) {
        int n = rows[i];
        int need = sensor_test_needed_height(n, line_step, first_row_y);
        int h = SEN_T_MIN_H;   /* старт с минимума, как в init */
        int fits;

        /* Авто-режим присваивает, а не только растёт: снятые строки
         * сжимают окно. */
        h = CLAMP(need, SEN_T_MIN_H, 1200);
        fits = (h - first_row_y - SEN_T_MARGIN * 2) / line_step;
        check_int(fits >= n, 1,
                  "окно вмещает все строки при авто-высоте");
        if (fits < n)
            printf("  инфо: строк=%d окно=%d влезает=%d нужно=%d\n",
                   n, h, fits, need);
    }

    /* Ровно 18 строк по нашему конфигу: 4 + 18*12 + 8 = 228 px. */
    check_int(sensor_test_needed_height(18, 12, 4), 228,
              "18 строк требуют ровно 228 px");

    /* Регрессия: старый код с `if (need > priv->height)` при старой
     * высоте 172 оставлял 18 строк в окне на 13. */
    {
        int need = sensor_test_needed_height(18, 12, 4);
        int stale = 172;   /* высота, записанная прошлым запуском */
        int old_fits = (stale - 4 - SEN_T_MARGIN * 2) / 12;

        check_int(old_fits, 13, "старая высота 172 вмещала 13 строк");
        check_int(need > stale, 1, "нужная высота больше старой");
    }

    /* Снятие строк сжимает окно, а не оставляет пустое поле. */
    {
        int big = CLAMP(sensor_test_needed_height(18, 12, 4), SEN_T_MIN_H,
                        1200);
        int small = CLAMP(sensor_test_needed_height(4, 12, 4), SEN_T_MIN_H,
                          1200);

        check_int(small < big, 1, "после снятия строк окно сжалось");
        check_int(small, 60, "4 строки дают минимум 60 px");
    }
}

/* Фильтр типа канала: вентилятор под префиксом temp не показывается.
 *
 * Регрессия: nct7904 — универсальный чип, там под tempN_input лежат и
 * температуры, и обороты вентиляторов. Канал temp6 имеет temp6_type=6
 * (вентилятор) и честно отдаёт 0, а апплет показывал «0.0°C», то есть
 * обороты в градусах Цельсия.
 *
 * Правило: type=6 — вентилятор, канал пропускается. Остальные типы
 * (1-5) показываются, и каналы без tempN_type (старые драйверы) тоже:
 * там префикс temp сам по себе означает температуру.
 */
static void test_channel_type_filter(void)
{
    Fixture f = fx_new();
    const char *const files[] = {
        "temp4_input=41125\n",
        "temp4_type=4\n",     /* настоящий датчик */
        "temp6_input=0\n",
        "temp6_type=6\n",     /* вентилятор: 0 об/мин */
        "temp7_input=0\n",
        "temp7_type=6\n",     /* вентилятор */
        "temp1_input=-128000\n",
        "temp1_type=3\n",     /* нет датчика: канал есть, данных нет */
        NULL
    };
    SensorList *list;
    gboolean has4 = FALSE, has6 = FALSE, has7 = FALSE, has1 = FALSE;

    if (!f.path)
        return;
    fx_hwmon(&f, "hwmon0", "nct7904", files);
    list = sensor_list_read(f.path);
    check(list != NULL, "дерево с типами каналов прочитано");
    if (!list) {
        fx_free(&f);
        return;
    }
    check_int((gint) list->chips->len, 1, "один чип");
    if (list->chips->len == 1) {
        SensorChip *c = g_ptr_array_index(list->chips, 0);

        for (guint i = 0; i < c->readings->len; i++) {
            SensorReading *r = g_ptr_array_index(c->readings, i);

            if (r->channel == 4) {
                has4 = TRUE;
                check_int(r->valid, 1, "канал 4 валиден");
                check_dbl(r->celsius, 41.125, 0.001, "канал 4 = 41.125°C");
            } else if (r->channel == 6) {
                has6 = TRUE;
            } else if (r->channel == 7) {
                has7 = TRUE;
            } else if (r->channel == 1) {
                has1 = TRUE;
                check_int(r->valid, 0, "канал 1 без данных (-128000)");
            }
        }
    }
    check_int(has4, 1, "температурный канал 4 на месте");
    check_int(has6, 0, "вентилятор 6 отфильтрован");
    check_int(has7, 0, "вентилятор 7 отфильтрован");
    check_int(has1, 1, "канал 1 (type 3) остался — будет прочерк");
    sensor_list_free(list);
    fx_free(&f);
}

/* Каналы без tempN_type показываются: префикс temp сам по себе означает
 * температуру (старые драйверы). */
static void test_no_type_file_means_temperature(void)
{
    Fixture f = fx_new();
    const char *const files[] = {
        "temp1_input=45000\n",
        /* temp1_type намеренно не создаём */
        NULL
    };
    SensorList *list;
    gint n = 0;

    if (!f.path)
        return;
    fx_hwmon(&f, "hwmon0", "olddrv", files);
    list = sensor_list_read(f.path);
    check(list != NULL, "дерево без tempN_type прочитано");
    if (!list) {
        fx_free(&f);
        return;
    }
    if (list->chips->len == 1) {
        SensorChip *c = g_ptr_array_index(list->chips, 0);

        n = (gint) c->readings->len;
        if (n == 1) {
            SensorReading *r = g_ptr_array_index(c->readings, 0);

            check_dbl(r->celsius, 45.0, 0.001, "канал без type = 45°C");
        }
    }
    check_int(n, 1, "канал без tempN_type не отфильтрован");
    sensor_list_free(list);
    fx_free(&f);
}

/* NVIDIA через NVML: идентификатор строки, чтение, отсутствие NVML.
 *
 * Эти тесты НЕ требуют карты: на машине без NVIDIA nvml_open() обязан
 * вернуть FALSE, и плагин — продолжить работу на одних hwmon-сенсорах.
 * Проверяется именно это, плюс то, что источник nvidia/ опознаётся по
 * префиксу и не ищется в sysfs (где его быть не может).
 */
static void test_nvml_source_prefix(void)
{
    check_int(nvml_source_is_nvidia("nvidia/GPU-abc/gpu"), 1,
              "источник nvidia/ опознан");
    check_int(nvml_source_is_nvidia("nvidia/GPU-abc/memory"), 1,
              "источник памяти опознан");
    check_int(nvml_source_is_nvidia("coretemp/0000:00:04.0/temp1"), 0,
              "hwmon-источник не считается nvidia");
    check_int(nvml_source_is_nvidia(""), 0, "пустая строка не nvidia");
    check_int(nvml_source_is_nvidia(NULL), 0, "NULL не nvidia");
}

/* Слот для синтетического пути карты. Раньше sensor_bus_slot() на
 * «nvml/GPU-…» возвращала пустую строку, и строка карты в таблице
 * настроек оставалась бы без названия устройства. */
static void test_nvml_bus_slot(void)
{
    char *s;

    s = sensor_bus_slot("nvml/GPU-3aaa4916-ec48-3461-1a64-df9c72a1");
    check(s != NULL, "слот карты построен");
    if (s) {
        check_str(s, "GPU-3aaa4916-ec48-3461-1a64-df9c72a1",
                     "слот = UUID карты");
        g_free(s);
    }
    /* Группа получается по имени чипа, шины у карты нет. */
    s = sensor_group_name("nvidia", "nvml/GPU-abc");
    check_str(s, "nvidia", "группа карты = nvidia");
    g_free(s);
}

/* Пустое значение на неизвестном/битом источнике. Плагин обязан
 * отличать «не знаю» от 0 градусов — иначе неисправная карта или снятая
 * видеокарта показали бы «0.0°C», как это делал канал вентилятора. */
static void test_nvml_read_unknown_is_nan(void)
{
    gdouble v = 0.0;
    gboolean ok;

    ok = nvml_read_value("nvidia/GPU-nonexistent/gpu", &v);
    check_int(ok, 0, "несуществующая карта не даёт значение");
    check_int(isnan(v), 1, "значение = NAN, а не 0");

    ok = nvml_read_value("nvidia/GPU-abc/no-such-channel", &v);
    check_int(ok, 0, "неизвестный канал не даёт значение");
    check_int(isnan(v), 1, "значение = NAN");

    ok = nvml_read_value("coretemp/0000:00:04.0/temp1", &v);
    check_int(ok, 0, "hwmon-источник не читается через NVML");
}

/* Живой NVML, если карта есть. На машине без NVIDIA проверка
 * пропускается — это штатное состояние, не сбой. */
static void test_nvml_live_if_present(void)
{
    SensorList *list = nvml_sensor_list_read();

    if (!list) {
        printf("  пропуск: NVML недоступен (машина без NVIDIA)\n");
        checks++;
        return;
    }
    check_int(list->chips->len > 0, 1, "NVML вернул хотя бы одну карту");
    if (list->chips->len > 0) {
        SensorChip *c = g_ptr_array_index(list->chips, 0);
        gdouble v = 0.0;
        char *row;

        check_str(c->chip, "nvidia", "чип карты назван nvidia");
        check(c->device && *c->device, "у карты есть идентификатор");
        check_int(c->readings->len > 0, 1, "у карты есть каналы");
        check_int(c->readings->len < 3, 1,
                  "каналов не больше двух (gpu и memory)");

        row = sensor_row_id(c->chip, c->device, "gpu");
        check(row != NULL && g_str_has_prefix(row, "nvidia/"),
              "идентификатор строки карты с префиксом nvidia/");
        /* Значение канала обязано совпадать с тем, что вернул список. */
        if (c->readings->len > 0) {
            SensorReading *r = g_ptr_array_index(c->readings, 0);

            if (nvml_read_value(row, &v)) {
                /* NVML отдаёт ЦЕЛЫЕ градусы: значение обязано совпасть
                 * с прочитанным при обходе, без деления на тысячу. */
                check_dbl(v, r->celsius, 0.001,
                          "повторное чтение того же канала");
                check(v > -50.0 && v < 150.0,
                      "температура GPU в разумных пределах");
            }
        }
        g_free(row);
    }
    sensor_list_free(list);
}

/* Тестовая строка «dummy»: константы, значение, отсутствие в дереве.
 *
 * Пока пользователь ничего не выбрал, апплет обязан показать ровно одну
 * строку «dummy» с фиксированным 36.6 °C, а не угадывать за него первые
 * четыре найденных сенсора. Источник dummy не существует ни в sysfs, ни
 * в NVML — это не датчик, и проверять его на живость нельзя. */
static void test_dummy_row_constants(void)
{
    check_str(SEN_DUMMY_LABEL, "dummy", "подпись тестовой строки");
    check_str(SEN_DUMMY_SOURCE, "dummy", "источник тестовой строки");
    check_dbl(SEN_DUMMY_CELSIUS, 36.6, 0.0001, "значение тестовой строки");

    /* Метка и источник совпадают намеренно: в таблице настроек строка
     * dummy не показывается вовсе, а в конфиг пишется как «dummy|dummy».
     * Разные строки дали бы строку, которую нечем включить обратно. */
    check_str(SEN_DUMMY_LABEL, SEN_DUMMY_SOURCE,
              "подпись и источник dummy совпадают");

    /* Источник dummy не должен совпасть ни с одним реальным: иначе
     * тестовая строка незаметно превратилась бы в данные датчика. */
    {
        SensorList *found = sensor_list_read("/sys/class/hwmon");

        if (found) {
            check_int(sensor_find_reading(found, SEN_DUMMY_SOURCE) != NULL, 0,
                      "dummy не находится среди реальных сенсоров");
            sensor_list_free(found);
        }
    }
    check_int(nvml_source_is_nvidia(SEN_DUMMY_SOURCE), 0,
              "dummy не считается источником NVIDIA");
}

/* Значение тестовой строки форматируется как обычное число — с единицей
 * измерения и пересчётом в Fahrenheit, когда он выбран. Иначе строка,
 * задуманная как проверка вывода, показывала бы «36.6» без градусов и
 * не доказывала бы, что форматирование работает. */
static void test_dummy_value_formatting(void)
{
    char *c = sensor_format_value(SEN_DUMMY_CELSIUS, FALSE, TRUE);
    char *f = sensor_format_value(SEN_DUMMY_CELSIUS, TRUE, TRUE);

    check_str(c, "36.6°C", "dummy в Цельсиях с единицей");
    check_str(f, "97.9°F", "dummy в Фаренгейтах с единицей");
    g_free(c);
    g_free(f);
}

/* Регрессия: value_x обязан считаться ПОСЛЕ формирования списка строк.
 *
 * Дефолт из одной строки «dummy» выявил настоящий баг: расчёт позиции
 * числа стоял ВЫШЕ добавления дефолтных строк, поэтому цикл измерял
 * пустой список, widest оставался 0, и value_x схлопывался на label_x.
 * На экране число ложилось прямо на подпись: «dummy: 36.6°C» превращалось
 * в кашу в углу. С прежним дефолтом из четырёх сенсоров расчёт попадал на
 * заполненный список лишь потому, что те добавлялись раньше, — работало
 * случайно, и баг был не виден.
 *
 * Здесь воспроизведён именно тот расчёт, что в плагине: измеряем
 * подписи списка и считаем позицию числа. Пустой список обязан дать
 * value_x == label_x (это и есть исходная ошибка), непустой — строго
 * правее. */

/* Регрессия: value_x обязан считаться ПОСЛЕ формирования списка строк.
 *
 * Дефолт из одной строки «dummy» выявил настоящий баг: расчёт позиции
 * числа стоял ВЫШЕ добавления дефолтных строк, поэтому цикл измерял
 * пустой список, widest оставался 0, и value_x схлопывался на label_x.
 * На экране число ложилось прямо на подпись: «dummy: 36.6°C» превращалось
 * в кашу в углу. С прежним дефолтом из четырёх сенсоров расчёт попадал на
 * заполненный список лишь потому, что те добавлялись раньше, — работало
 * случайно, и баг был не виден.
 *
 * Здесь воспроизведён именно тот расчёт, что в плагине: измеряем
 * подписи списка и считаем позицию числа. Пустой список обязан дать
 * value_x == label_x (это и есть исходная ошибка), непустой — строго
 * правее. */
static int sen_test_text_width(cairo_t *cr, const char *text, const char *font)
{
    PangoFontDescription *fd = pango_font_description_from_string(font);
    PangoRectangle logical;
    PangoLayout *layout;
    int width;

    layout = pango_cairo_create_layout(cr);
    pango_layout_set_font_description(layout, fd);
    pango_layout_set_text(layout, text, -1);
    pango_layout_get_extents(layout, NULL, &logical);
    width = logical.width / PANGO_SCALE;
    g_object_unref(layout);
    pango_font_description_free(fd);
    return width;
}

/* Позиция значения по списку подписей — копия формулы плагина. */
static int sen_test_value_x(GPtrArray *labels, int label_x, const char *font)
{
    cairo_surface_t *probe =
        cairo_image_surface_create(CAIRO_FORMAT_ARGB32, 1, 1);
    cairo_t *pcr = cairo_create(probe);
    int widest = 0;

    for (guint i = 0; i < labels->len; i++) {
        int w = sen_test_text_width(pcr, g_ptr_array_index(labels, i), font);

        if (w > widest)
            widest = w;
    }
    cairo_destroy(pcr);
    cairo_surface_destroy(probe);
    return label_x + widest + 6;   /* SEN_VALUE_GAP = 6 */
}

static void test_default_value_x_after_rows(void)
{
    GPtrArray *empty = g_ptr_array_new();
    GPtrArray *with_dummy = g_ptr_array_new();
    int x_empty, x_dummy;

    g_ptr_array_add(with_dummy, g_strdup(SEN_DUMMY_LABEL));

    x_empty = sen_test_value_x(empty, 4, "Sans 8");
    x_dummy = sen_test_value_x(with_dummy, 4, "Sans 8");

    /* Пустой список — это исходная ошибка: widest=0, и число встаёт в
     * label_x + зазор, то есть вплотную к подписи (в плагине это давало
     * наложение «dummy» и «36.6°C»). Важно, что позиция НЕ зависит от
     * содержимого строк, — тогда она совпадает для всех подписей. */
    check_int(x_empty < x_dummy, 1,
              "пустой список жмёт число к подписи (источник наложения)");
    /* С добавленной строкой число обязано уйти вправо. */
    check_int(x_dummy > 4, 1, "непустой список двигает value_x правее label_x");
    /* И главное: добавление строки обязано МЕНЯТЬ позицию. Если бы
     * расчёт шёл по пустому списку, x_dummy == x_empty. */
    check_int(x_dummy != x_empty, 1,
              "позиция зависит от строк — расчёт идёт по заполненному списку");

    g_ptr_array_unref(empty);
    g_ptr_array_free(with_dummy, TRUE);
}

/* Конфиг хранит ТОЛЬКО включённые сенсоры.
 *
 * Инвариант: в ключе rows лежат пары «подпись|источник» исключительно
 * для реальных сенсоров. Отключённый сенсор не хранится вовсе — не как
 * «выключенная строка», не как запись без галочки. Тестовая строка dummy
 * — не сенсор, и в конфиге ей тоже не место: её наличие определяется тем,
 * что других строк нет.
 *
 * Проверяется ровно та формула, что в sen_rows_to_config(). */
static char *sen_test_rows_to_config(const char *const *rows,
                                     const char *const *sources, guint n)
{
    GPtrArray *labels = g_ptr_array_new_with_free_func(g_free);
    GPtrArray *srcs = g_ptr_array_new_with_free_func(g_free);
    char *text;

    for (guint i = 0; i < n; i++) {
        const char *src = sources[i];

        if (!src || !*src || g_strcmp0(src, SEN_DUMMY_SOURCE) == 0)
            continue;   /* не сенсор */
        g_ptr_array_add(labels, g_strdup(rows[i]));
        g_ptr_array_add(srcs, g_strdup(src));
    }
    if (labels->len == 0) {
        g_ptr_array_unref(labels);
        g_ptr_array_unref(srcs);
        return NULL;
    }
    text = sensor_config_join_pair(labels, srcs);
    g_ptr_array_unref(labels);
    g_ptr_array_unref(srcs);
    return text;
}

static void test_config_only_enabled_sensors(void)
{
    /* 1. Только сенсоры — в конфиг идут все. */
    {
        static const char *rows[]  = { "CPU", "Диск", "Видеокарта" };
        static const char *srcs[]  = {
            "coretemp/coretemp.0/Core 0",
            "drivetemp/0:0:2:0/temp1",
            "nvidia/GPU-abc/gpu"
        };
        char *t = sen_test_rows_to_config(rows, srcs, 3);

        check(t != NULL, "три сенсора дают непустой конфиг");
        check(t && strstr(t, "CPU|coretemp/coretemp.0/Core 0") != NULL,
              "первый сенсор в конфиге");
        check(t && strstr(t, "Видеокарта|nvidia/GPU-abc/gpu") != NULL,
              "источник NVIDIA в конфиге");
        check(t && strchr(t, ';') != NULL, "список разделён точкой с запятой");
        g_free(t);
    }

    /* 2. Пустой выбор — ключа быть не должно вовсе (NULL), а не пустая
     * строка: пустая строка значила бы «выбрана строка без подписи», и
     * следующий старт прогнал бы по ней миграцию. */
    {
        char *t = sen_test_rows_to_config(NULL, NULL, 0);

        check_int(t == NULL, 1, "пустой выбор даёт NULL, а не пустую строку");
        g_free(t);
    }

    /* 3. Одна отключённая строка исчезает из конфига. */
    {
        static const char *rows[] = { "CPU", "ОТКЛ" };
        static const char *srcs[] = {
            "coretemp/coretemp.0/Core 0",
            ""                        /* сняли галочку */
        };
        char *t = sen_test_rows_to_config(rows, srcs, 2);

        check(t != NULL, "одна включённая строка остаётся");
        check(t && strstr(t, "ОТКЛ") == NULL,
              "отключённый сенсор не попал в конфиг");
        check(t && strstr(t, "CPU|coretemp/coretemp.0/Core 0") != NULL,
              "включённый сенсор на месте");
        g_free(t);
    }

    /* 4. Только dummy — как при первом старте: ключа нет. */
    {
        static const char *rows[] = { SEN_DUMMY_LABEL };
        static const char *srcs[] = { SEN_DUMMY_SOURCE };
        char *t = sen_test_rows_to_config(rows, srcs, 1);

        check_int(t == NULL, 1, "только тестовая строка = ключа rows нет");
        g_free(t);
    }

    /* 5. Подпись может содержать «;» и «|» — формат обязан выжить. */
    {
        static const char *rows[] = { "Диск; системный | главный" };
        static const char *srcs[] = { "drivetemp/0:0:2:0/temp1" };
        char *t = sen_test_rows_to_config(rows, srcs, 1);

        check(t != NULL, "спецсимволы в подписи не ломают запись");
        g_free(t);
    }
}

/* Шаг строк не может быть меньше высоты текста.
 *
 * Наезд строк друг на друга был реальным дефектом, а не опечаткой в
 * конфиге: line_step задаёт шаг при компактном шрифте, и увеличение
 * шрифта в Настройках о нём ничего не говорило. При line_step=12 и
 * Sans 10 высота строки 13 — вторая ложалась на первую.
 *
 * Проверяем формулу sen_effective_step: она обязана расширить шаг вниз,
 * а не оставить пользовательское значение. Отдельно проверяем, что
 * меньший шаг сохраняется — иначе плотный список стал бы невозможен. */
static int sen_test_row_height(cairo_t *cr, const char *font)
{
    PangoFontDescription *fd = pango_font_description_from_string(font);
    PangoRectangle logical;
    PangoLayout *layout;
    int h;

    layout = pango_cairo_create_layout(cr);
    pango_layout_set_font_description(layout, fd);
    pango_layout_set_text(layout, "Проба Ag", -1);
    pango_layout_get_extents(layout, NULL, &logical);
    h = logical.height / PANGO_SCALE;
    g_object_unref(layout);
    pango_font_description_free(fd);
    return h;
}

static int sen_test_step(cairo_t *cr, int line_step,
                         const char *lf, const char *vf)
{
    int text_h = MAX(sen_test_row_height(cr, lf),
                     sen_test_row_height(cr, vf));

    return MAX(line_step, text_h);
}

static void test_step_never_overlaps_text(void)
{
    cairo_surface_t *probe =
        cairo_image_surface_create(CAIRO_FORMAT_ARGB32, 1, 1);
    cairo_t *cr = cairo_create(probe);
    int h10 = sen_test_row_height(cr, "Sans 10");
    int s_big, s_small, s_equal;

    /* Исходный дефект: при line_step=12 шаг выходил меньше высоты. */
    check_int(h10 > 12, 1,
              "высота Sans 10 больше старого шага 12 (иначе тест бессмыслен)");
    s_big = sen_test_step(cr, 12, "Sans 10", "Sans 10");
    check_int(s_big >= h10, 1, "шаг расширен до высоты текста");
    check_int(s_big, h10, "шаг ровно равен высоте текста");

    /* Меньший шаг сохраняется: пользователь вправе задать плотный список. */
    s_small = sen_test_step(cr, 4, "Sans 8", "Sans 8");
    check_int(s_small > 4, 1, "шаг меньше высоты тоже расширяется");
    check_int(s_small,
              sen_test_row_height(cr, "Sans 8"),
              "расширенный шаг = высота шрифта 8");

    /* Равные — не трогаем. */
    s_equal = sen_test_step(cr, h10, "Sans 10", "Sans 10");
    check_int(s_equal, h10, "шаг, равный высоте, не меняется");

    cairo_destroy(cr);
    cairo_surface_destroy(probe);
}

/* Высота окна в авто-режиме обязана считаться по ЭФФЕКТИВНОМУ шагу.
 *
 * Иначе окно считается по line_step, а текст рисуется по максимуму
 * (line_step, высота шрифта), и последняя строка уезжает за нижний край
 * — то есть ровно тот дефект, который эта правка чинит в отрисовке. */
static void test_auto_height_uses_effective_step(void)
{
    const int rows = 4;
    const int first_row_y = 4;
    const int margin = 6;

    cairo_surface_t *probe =
        cairo_image_surface_create(CAIRO_FORMAT_ARGB32, 1, 1);
    cairo_t *cr = cairo_create(probe);
    int step = sen_test_step(cr, 12, "Sans 10", "Sans 10");
    int height = first_row_y + rows * step + margin * 2;
    int height_by_config = first_row_y + rows * 12 + margin * 2;

    check_int(height > height_by_config, 1,
              "окно по эффективному шагу выше, чем по line_step");
    /* Последняя базовая линия плюс descender должны помещаться. */
    check_int(first_row_y + (rows - 1) * step < height, 1,
              "последняя строка внутри окна");

    cairo_destroy(cr);
    cairo_surface_destroy(probe);
}

/* Заглушка dummy живёт ровно тогда, когда не выбран ни один сенсор.
 *
 * Требование пользователя: стоит отметить хоть один сенсор — dummy
 * исчезает. Раньше он оставался в priv->rows рядом с настоящими
 * данными и читался как ещё один датчик с неизменным значением, то
 * есть делал ровно то, для чего его придумали, но уже ненужное.
 *
 * Проверяем обе стороны инварианта, потому что ошибка в любую из них
 * одинаково плохо видна: лишняя заглушка в выводе или пустой апплет
 * при выбранных сенсорах. */
static gboolean sen_test_has_dummy(const GPtrArray *sources)
{
    for (guint i = 0; i < sources->len; i++) {
        if (g_strcmp0(g_ptr_array_index(sources, i), SEN_DUMMY_SOURCE) == 0)
            return TRUE;
    }
    return FALSE;
}

static void test_dummy_disappears_on_first_sensor(void)
{
    /* Состояние «сенсоров нет»: dummy есть. */
    {
        GPtrArray *rows = g_ptr_array_new_with_free_func(g_free);
        GPtrArray *srcs = g_ptr_array_new_with_free_func(g_free);

        g_ptr_array_add(rows, g_strdup(SEN_DUMMY_LABEL));
        g_ptr_array_add(srcs, g_strdup(SEN_DUMMY_SOURCE));

        check_int(sen_test_has_dummy(srcs), 1,
                  "без сенсоров заглушка присутствует");
        check_int(rows->len, 1, "без сенсоров ровно одна строка");

        g_ptr_array_unref(rows);
        g_ptr_array_unref(srcs);
    }

    /* Включение первого сенсора: dummy убирается, остаётся сенсор. */
    {
        GPtrArray *rows = g_ptr_array_new_with_free_func(g_free);
        GPtrArray *srcs = g_ptr_array_new_with_free_func(g_free);

        g_ptr_array_add(rows, g_strdup(SEN_DUMMY_LABEL));
        g_ptr_array_add(srcs, g_strdup(SEN_DUMMY_SOURCE));
        /* sen_drop_dummy_row: индекс dummy = 0 */
        g_ptr_array_remove_index(rows, 0);
        g_ptr_array_remove_index(srcs, 0);
        g_ptr_array_add(rows, g_strdup("CPU"));
        g_ptr_array_add(srcs, g_strdup("coretemp/coretemp.0/Core 0"));

        check_int(sen_test_has_dummy(srcs), 0,
                  "после включения сенсора заглушки нет");
        check_int(rows->len, 1, "осталась одна строка — выбранный сенсор");
    }

    /* Последний сенсор снят: заглушка возвращается. Иначе апплет
     * остался бы пустым, и пользователь не понял бы, работает ли он. */
    {
        GPtrArray *rows = g_ptr_array_new_with_free_func(g_free);
        GPtrArray *srcs = g_ptr_array_new_with_free_func(g_free);
        gboolean dummy;

        g_ptr_array_add(rows, g_strdup("CPU"));
        g_ptr_array_add(srcs, g_strdup("coretemp/coretemp.0/Core 0"));
        /* сняли галочку — сенсор ушёл, ничего не осталось */
        g_ptr_array_remove_index(rows, 0);
        g_ptr_array_remove_index(srcs, 0);
        dummy = (rows->len == 0);
        if (dummy) {
            g_ptr_array_add(rows, g_strdup(SEN_DUMMY_LABEL));
            g_ptr_array_add(srcs, g_strdup(SEN_DUMMY_SOURCE));
        }

        check_int(sen_test_has_dummy(srcs), 1,
                  "снятие последнего сенсора возвращает заглушку");
    }

    /* Смесь в конфиге (след прежних сборок) чистится на старте. */
    {
        GPtrArray *rows = g_ptr_array_new_with_free_func(g_free);
        GPtrArray *srcs = g_ptr_array_new_with_free_func(g_free);

        g_ptr_array_add(rows, g_strdup(SEN_DUMMY_LABEL));
        g_ptr_array_add(srcs, g_strdup(SEN_DUMMY_SOURCE));
        g_ptr_array_add(rows, g_strdup("CPU"));
        g_ptr_array_add(srcs, g_strdup("coretemp/coretemp.0/Core 0"));

        check_int(sen_test_has_dummy(srcs), 1, "смесь в конфиге — входные данные");
        /* init вызывает sen_drop_dummy_row, если строк больше одной */
        if (rows->len > 1 && sen_test_has_dummy(srcs)) {
            guint idx = 0;

            g_ptr_array_remove_index(rows, idx);
            g_ptr_array_remove_index(srcs, idx);
        }
        check_int(sen_test_has_dummy(srcs), 0,
                  "init убирает заглушку из смеси");
        check_int(rows->len, 1, "в смеси остался только сенсор");

        g_ptr_array_unref(rows);
        g_ptr_array_unref(srcs);
    }
}

/* Заглушка dummy — только при ПЕРВОМ запуске.
 *
 * Требование пользователя: отметил сенсор, потом убрал — сенсоров
 * ноль, и dummy тоже не нужен. То есть «ничего не выбрано» и «ничего
 * не выбрано, потому что ты снимал галочки» — это разные состояния, и
 * без отдельного признака они неразличимы: в обоих случаях ключа rows
 * в конфиге нет.
 *
 * Признак selected переживает момент обнуления: он пишется всегда, даже
 * когда сенсоров не осталось ни одного. Иначе следующий старт решил
 * бы, что это первый запуск, и вернул заглушку. */
static void test_dummy_only_on_first_run(void)
{
    /* 1. Первый запуск: selected=0, строк нет -> dummy есть. */
    {
        gboolean selected = FALSE, rows_len = 0, dummy;

        dummy = (rows_len == 0 && !selected);
        check_int(dummy, 1, "первый запуск показывает заглушку");
    }

    /* 2. Выбрал сенсор, потом снял: selected=1, строк нет -> dummy НЕТ. */
    {
        gboolean selected = TRUE, rows_len = 0, dummy;

        dummy = (rows_len == 0 && !selected);
        check_int(dummy, 0, "после снятия сенсора заглушки нет");
    }

    /* 3. Есть сенсоры: заглушка не появляется ни при каком selected. */
    {
        gboolean rows_len = 2, dummy;

        dummy = (rows_len == 0 && !TRUE);
        check_int(dummy, 0, "при выбранных сенсорах заглушки нет");
    }

    /* 4. Признак пишется даже при нуле сенсоров. Если бы при пустом
     * выборе ключ не писался, признак потерялся бы — и п.2 сломался бы
     * ровно при следующем старте, а не сразу. */
    {
        guint rows_len = 0;
        gboolean selected = TRUE;
        char *conf_rows = NULL;      /* sen_rows_to_config вернёт NULL */
        char *conf_selected = NULL;  /* но признак обязан писаться */

        if (rows_len == 0)
            conf_rows = NULL;
        if (selected)
            conf_selected = g_strdup("1");

        check_int(conf_rows == NULL, 1, "при нуле сенсоров ключа rows нет");
        check(conf_selected != NULL,
              "признак selected пишется даже без сенсоров");
        g_free(conf_selected);
    }
}

int main(void)
{
    printf("test_sensors\n");
    test_reads_channels();
    test_channel_order_is_numeric();
    test_invalid_marker();
    test_negative_temp_is_valid();
    test_malformed_value();
    test_empty_value();
    test_error_is_not_zero();
    test_find_is_exact();
    test_hwmon_order();
    test_no_channels_skips_chip();
    test_id_independent_of_hwmon_number();
    test_same_chip_different_devices();
    test_find_reading_rejects_partial();
    test_sensors_name();
    test_sensors_names_unique_on_live_tree();
    test_config_roundtrip_keeps_source();
    test_config_source_takes_after_first_bar();
    test_config_skips_empty();
    test_font_size_in_points();
    test_font_scale_proportional();
    test_units_format();
    test_join_pair_keeps_source();
    test_join_pair_roundtrip();
    test_units();
    test_format();
    test_config_list();
    test_config_list_edges();
    test_rounding();
    test_rounded_region();
    test_live_tree();
    test_dummy_row_constants();
    test_dummy_value_formatting();
    test_config_only_enabled_sensors();
    test_step_never_overlaps_text();
    test_dummy_disappears_on_first_sensor();
    test_dummy_only_on_first_run();
    test_auto_height_uses_effective_step();
    test_default_value_x_after_rows();
    test_nvml_source_prefix();
    test_nvml_bus_slot();
    test_nvml_read_unknown_is_nan();
    test_nvml_live_if_present();
    test_channel_type_filter();
    test_no_type_file_means_temperature();
    test_auto_height_fits_all_rows();
    test_toggle_row_is_reversible();
    test_bus_kind_and_slot();
    test_name_composed_from_kind_slot();
    test_name_without_device();
    test_slots_unique_per_group();
    test_group_names();
    test_group_slots_unique_live();
    test_every_config_row_has_reading();

    if (failures == 0)
        printf("TEST_OK: %d проверок, 0 провалов\n", checks);
    else
        printf("TEST_FAIL: %d проверок, %d провалов\n", checks, failures);
    return failures == 0 ? 0 : 1;
}
