/* sensors_nvml.h — источник NVIDIA через NVML.
 *
 * Почему не hwmon: у NVIDIA нет узла /sys/class/hwmon. Драйвер nvidia
 * экспортирует только NVML (libnvidia-ml), и на этой машине
 * ни один узел в /sys/class/hwmon не ссылается на nvidia.
 * nvidia-smi(1) — это обёртка над тем же NVML, но это процесс на каждый
 * кадр и парсинг его текста; здесь символы берутся напрямую.
 *
 * Почему dlopen, а не линковка с -lnvidia-ml: заголовков nvidia-ml.h в
 * системе нет, а плагин должен грузиться и на машине без NVIDIA вообще.
 * dlopen даёт ровно это: отсутствие библиотеки — не ошибка загрузки,
 * а пустой список сенсоров, и плагин продолжает работать.
 *
 * Идентификатор строки — UUID карты («GPU-3aaa4916-…»), а не индекс
 * nvmlDevice: индекс — это порядок перечисления и он меняется при
 * перестановке карт в слотах. UUID переживает и это, и перезагрузку,
 * и смену iBus, в отличие от PCI-адреса.
 */
#ifndef SENSORS_NVML_H
#define SENSORS_NVML_H

#include "sensors_core.h"

/* Сенсоры NVIDIA. Возвращает NULL, если NVML нет или не инициализируется
 * — это нормальное состояние, а не ошибка. Список, если он есть,
 * свободен через sensor_list_free(). */
SensorList *nvml_sensor_list_read(void);

/* Сказан ли этот канал через NVML. В конфиге строка выглядит как
 * «nvidia/GPU-3aaa4916-…/gpu», и по префиксу «nvidia/» её надо уметь
 * опознать без обращения к диску: NVML в обход sysfs. */
gboolean nvml_source_is_nvidia(const char *row_id);

/* Значение канала. Аналог sensor_read, но для источника NVML:
 * возвращает FALSE и ставит *out_celsius в NAN, если канала нет. */
gboolean nvml_read_value(const char *row_id, gdouble *out_celsius);

#endif /* SENSORS_NVML_H */
