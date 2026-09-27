/* sensors_nvml.c — чтение температур NVIDIA через NVML, без subprocess.
 *
 * Всё живёт в одном контексте: nvmlInit() один раз при первом
 * обращении, nvmlShutdown() — на выходе плагина. Повторный init/shutdown
 * на каждом кадре был бы дорог и на рёбрах NVML даёт ошибки, поэтому
 * состояние явное.
 *
 * Температуры NVML — ЦЕЛЫЕ градусы, без долей и без тысячных, в отличие
 * от sysfs (там миллиградусы). Это различие легко пропустить, и оно даёт
 * «0.0°C» вместо 45°C при ошибочном делении на 1000. Здесь деления нет
 * вовсе: значение приходит как есть.
 *
 * Каналы, которые карта не поддерживает (у GTX 970 это
 * NVML_TEMPERATURE_MEMORY — rc=2 NOT_SUPPORTED), в список не попадают.
 * Это тот же принцип, что с фильтром tempN_type у вентиляторов nct7904:
 * канала нет — строки нет, а не «0.0°C».
 */
#include "sensors_nvml.h"

#include <dlfcn.h>
#include <string.h>
#include <math.h>

/* Коды NVML, нужные нам. Объявляем локально: заголовка nvidia-ml.h в
 * системе нет, и тащить его ради трёх констант — лишняя зависимость,
 * которая не соберётся на машине без NVIDIA-драйвера. */
#define NVML_SUCCESS                0
#define NVML_TEMPERATURE_GPU         0
#define NVML_TEMPERATURE_MEM         1

/* Имена символов NVML: публичный API суффиксирован _v2 (и _v1 у старых
 * функций). Пробуем обе схемы — иначе на части драйверов плагин молча
 * не найдёт функций. */
typedef int  (*fn_init_v2)(void);
typedef int  (*fn_shutdown)(void);
typedef int  (*fn_count_v2)(unsigned int *);
typedef int  (*fn_handle_v2)(unsigned int, void **);
typedef int  (*fn_name)(void *, char *, unsigned int);
typedef int  (*fn_uuid)(void *, char *, unsigned int);
typedef int  (*fn_temp)(void *, int, unsigned int *);

typedef struct {
    void   *lib;
    int     ready;        /* nvmlInit прошёл */
    int     tried;        /* уже пытались; без этого каждый кадр
                           * заново dlopen-ит отсутствующую библиотеку */
    fn_init_v2    init;
    fn_shutdown   shutdown;
    fn_count_v2   count;
    fn_handle_v2  handle;
    fn_name       name;
    fn_uuid       uuid;
    fn_temp       temp;
} NvmlCtx;

static NvmlCtx nvml_ctx;

static void *nvml_sym(const char *name)
{
    return dlsym(nvml_ctx.lib, name);
}

/* Загрузить библиотеку и связать символы. Один раз за жизнь процесса:
 * попытка помечается tried даже при неудаче, чтобы плагин на машине без
 * NVIDIA не платил dlopen за каждый кадр. */
static gboolean nvml_open(void)
{
    if (nvml_ctx.tried)
        return nvml_ctx.ready;
    nvml_ctx.tried = TRUE;

    /* Нет -l при линковке плагина, символы берём сами. */
    const char *const candidates[] = {
        "libnvidia-ml.so.1",
        "libnvidia-ml.so",
        NULL
    };

    for (guint i = 0; candidates[i] && !nvml_ctx.lib; i++) {
        nvml_ctx.lib = dlopen(candidates[i], RTLD_LAZY);
        if (nvml_ctx.lib)
            break;
    }
    if (!nvml_ctx.lib)
        return FALSE;   /* машина без NVIDIA — это не ошибка */

    /* У каждой функции пробуем _v2, потом v1: набор суффиксов различается
     * между функциями и версиями драйвера. */
    nvml_ctx.init  = (fn_init_v2)  (nvml_sym("nvmlInit_v2")
                                  ?: nvml_sym("nvmlInit"));
    nvml_ctx.count = (fn_count_v2) (nvml_sym("nvmlDeviceGetCount_v2")
                                  ?: nvml_sym("nvmlDeviceGetCount"));
    nvml_ctx.handle = (fn_handle_v2)(nvml_sym("nvmlDeviceGetHandleByIndex_v2")
                                   ?: nvml_sym("nvmlDeviceGetHandleByIndex"));
    nvml_ctx.name  = (fn_name)    (nvml_sym("nvmlDeviceGetName"));
    nvml_ctx.uuid  = (fn_uuid)    (nvml_sym("nvmlDeviceGetUUID"));
    nvml_ctx.temp  = (fn_temp)    (nvml_sym("nvmlDeviceGetTemperature"));
    nvml_ctx.shutdown = (fn_shutdown)(nvml_sym("nvmlShutdown_v2")
                                   ?: nvml_sym("nvmlShutdown"));

    if (!nvml_ctx.init || !nvml_ctx.count || !nvml_ctx.handle
        || !nvml_ctx.name || !nvml_ctx.uuid || !nvml_ctx.temp) {
        /* Библиотека есть, но это не NVML: закрываем, чтобы не держать
         * непродуктивный хендл до конца жизни плагина. */
        dlclose(nvml_ctx.lib);
        nvml_ctx.lib = NULL;
        return FALSE;
    }

    if (nvml_ctx.init() != NVML_SUCCESS) {
        /* Драйвер есть, но карта недоступна (нет карты, загрузка модуля
         * не завершена). Считаем NVML недоступным и не пробуем снова. */
        dlclose(nvml_ctx.lib);
        nvml_ctx.lib = NULL;
        return FALSE;
    }
    nvml_ctx.ready = TRUE;
    return TRUE;
}

/* Освободить NVML. Вызывается на выходе плагина: без этого handle
 * остаётся открытым, и при пересоздании апплета (properties -> init
 * -> shutdown -> init) число инициализаций накапливалось бы. */
void nvml_shutdown(void)
{
    if (nvml_ctx.ready && nvml_ctx.shutdown)
        nvml_ctx.shutdown();
    nvml_ctx.ready = FALSE;
    nvml_ctx.tried = FALSE;   /* следующий init попробует заново */
    if (nvml_ctx.lib) {
        dlclose(nvml_ctx.lib);
        nvml_ctx.lib = NULL;
    }
    memset(&nvml_ctx.init, 0, sizeof nvml_ctx.init);
    nvml_ctx.count = NULL;
    nvml_ctx.handle = NULL;
    nvml_ctx.name = NULL;
    nvml_ctx.uuid = NULL;
    nvml_ctx.temp = NULL;
    nvml_ctx.shutdown = NULL;
}

gboolean nvml_source_is_nvidia(const char *row_id)
{
    return row_id && g_str_has_prefix(row_id, "nvidia/");
}

/* Найти устройство по UUID: перечисление дешёвое (одно на кадр, доли
 * миллисекунды) и не требует кэша, который пришлось бы инвалидировать
 * при появлении/смене карты. */
static gboolean nvml_device_by_uuid(const char *uuid, void **out_dev)
{
    unsigned int n = 0;

    if (!nvml_open() || nvml_ctx.count(&n) != NVML_SUCCESS)
        return FALSE;
    for (unsigned int i = 0; i < n; i++) {
        void *dev = NULL;
        char buf[96] = {0};

        if (nvml_ctx.handle(i, &dev) != NVML_SUCCESS || !dev)
            continue;
        if (nvml_ctx.uuid(dev, buf, sizeof buf) != NVML_SUCCESS)
            continue;
        if (g_strcmp0(buf, uuid) == 0) {
            *out_dev = dev;
            return TRUE;
        }
    }
    return FALSE;
}

SensorList *nvml_sensor_list_read(void)
{
    unsigned int n = 0;
    SensorList *list;
    GPtrArray *chips;

    if (!nvml_open())
        return NULL;   /* нет NVIDIA — пустого списка не будет, но это
                         * и не ошибка: вызывающий обязан это учесть */
    if (nvml_ctx.count(&n) != NVML_SUCCESS || n == 0)
        return NULL;

    list = g_new0(SensorList, 1);
    list->chips = g_ptr_array_new_with_free_func(
        (GDestroyNotify) sensor_chip_free);
    chips = list->chips;

    for (unsigned int i = 0; i < n; i++) {
        void *dev = NULL;
        char name[96] = {0};
        char uuid[96] = {0};
        SensorChip *chip;
        GPtrArray *readings;

        if (nvml_ctx.handle(i, &dev) != NVML_SUCCESS || !dev)
            continue;
        if (nvml_ctx.name(dev, name, sizeof name) != NVML_SUCCESS)
            continue;
        if (nvml_ctx.uuid(dev, uuid, sizeof uuid) != NVML_SUCCESS)
            continue;

        chip = g_new0(SensorChip, 1);
        chip->chip   = g_strdup("nvidia");
        chip->device = g_strdup(uuid);
        /* dev_path заполняем идентификатором, а не sysfs-путём: у NVML
         * нет пути в sysfs, и группировка по нему не должна ломаться. */
        chip->dev_path = g_strdup_printf("nvml/%s", uuid);
        readings = g_ptr_array_new_with_free_func(
            (GDestroyNotify) sensor_reading_free);
        chip->readings = readings;

        /* Канал 0 — температура ядра. Есть всегда.
         * Канал 1 — температура памяти; у GTX 970 (кеш в чипе) она
         * не поддерживается, и код будет NVML_ERROR_NOT_SUPPORTED.
         * Неподдерживаемые каналы пропускаем целиком. */
        for (int sensor = NVML_TEMPERATURE_GPU; sensor <= NVML_TEMPERATURE_MEM;
             sensor++) {
            unsigned int value = 0;
            SensorReading *r;
            int rc;

            rc = nvml_ctx.temp(dev, sensor, &value);
            if (rc != NVML_SUCCESS)
                continue;   /* карта этого не умеет — строки не будет */

            r = g_new0(SensorReading, 1);
            r->chip   = g_strdup("nvidia");
            r->label  = g_strdup(sensor == NVML_TEMPERATURE_GPU
                                 ? "gpu" : "memory");
            r->device = g_strdup(uuid);
            r->channel = sensor;
            r->celsius = (gdouble) value;   /* уже целые градусы */
            r->valid = TRUE;
            r->read_error = FALSE;
            g_ptr_array_add(readings, r);
        }

        if (readings->len == 0) {
            /* Карта есть, но ни одного читаемого канала — в таблицу
             * настроек она не должна попасть пустой строкой. */
            sensor_chip_free(chip);
            continue;
        }
        g_ptr_array_add(chips, chip);
    }

    if (chips->len == 0) {
        sensor_list_free(list);
        return NULL;
    }
    return list;
}

gboolean nvml_read_value(const char *row_id, gdouble *out_celsius)
{
    char *uuid = NULL;
    const char *rest;
    const char *label;
    int sensor = -1;
    void *dev = NULL;
    unsigned int value = 0;

    if (out_celsius)
        *out_celsius = NAN;
    if (!nvml_source_is_nvidia(row_id))
        return FALSE;

    rest = strchr(row_id + strlen("nvidia/"), '/');
    if (!rest)
        return FALSE;
    uuid = g_strndup(row_id + strlen("nvidia/"), rest - (row_id + strlen("nvidia/")));
    label = rest + 1;

    if (g_strcmp0(label, "gpu") == 0)
        sensor = NVML_TEMPERATURE_GPU;
    else if (g_strcmp0(label, "memory") == 0)
        sensor = NVML_TEMPERATURE_MEM;
    if (sensor < 0) {
        g_free(uuid);
        return FALSE;
    }

    if (!nvml_device_by_uuid(uuid, &dev)
        || nvml_ctx.temp(dev, sensor, &value) != NVML_SUCCESS) {
        g_free(uuid);
        return FALSE;   /* значение неизвестно, а не 0 */
    }
    g_free(uuid);
    if (out_celsius)
        *out_celsius = (gdouble) value;
    return TRUE;
}
