# План disk_monitor

## Решение пользователя

- Один applet отслеживает один диск.
- На первом запуске автоматически выбирается первый доступный whole-disk из
  `/dev/disk/by-id/*`; в Properties можно выбрать другой диск.
- Конфигурация хранит стабильный `by-id` путь, а не `/dev/sd*`.
- `Read` и `Write` показываются текстом в индивидуально настраиваемых X/Y.
- Длинный график показывает историю чтения и записи; значения адаптивно
  форматируются как `B/s`, `KiB/s`, `MiB/s`, `GiB/s`, `TiB/s`.
- Температура показывается текстом в индивидуально настраиваемом X/Y и
  рисуется линией внутри того же графика.
- График должен повторять подачу gkrellm: две независимые серии в общей
  системе координат и общий масштаб bytes/s.

## Источники данных

- I/O: `/proc/diskstats`, 512-byte sectors, delta по monotonic time.
- Температура: связанный sysfs `hwmon/drivetemp` или `hwmon/nvme`.
- Fallback: hddtemp daemon на localhost:7634, протокол pipe-delimited,
  запрос не чаще одного раза в 60 секунд.
- `smartctl` автоматически не запускается.
- `tick()` выполняет I/O; `draw()` только показывает cached Cairo surface.

## Найденный визуальный эталон gkrellm

Официальный checkout:
`/home/kosmik2001_dir/.hermes/cache/scratch/gkrellm-reference`.

- `src/disk.c:607-649`: отдельные `read_cd` и `write_cd`, общий график.
- `src/disk.c:395-467`: `$r` и `$w` для текущих значений.
- `src/disk.c:769-806`: temperature decal поверх panel.
- `src/sysdeps/sensors-common.c:265-365`: hddtemp daemon читается раз в минуту.

## Обнаруженные блокеры первой реализации

1. `gint[]` temperature history ошибочно обрабатывался через API для
   `guint64[]`; нужен отдельный typed ring helper.
2. Смена диска не очищала histories и previous counters.
3. NVMe aliases (`name`, `name_1`) не дедуплицировались по resolved device.
4. hddtemp вызывался каждый тик и неверно разбирал pipe-протокол.
5. Первый renderer делил graph на две горизонтальные панели вместо единой
   общей системы координат gkrellm.
6. Температурный текст по умолчанию находился над graph, а не внутри него.
7. `sectors * 512` и `delta * 1000000` могли переполниться.

## Критерии готовности

- RED/GREEN tests для discovery, diskstats, rate overflow, formatting,
  temperature protocol/history, device reset и geometry.
- `make test` и `make clean && make -j2` без новых warnings/errors.
- Targeted `-Werror`, `nm -D ... xs_plugin_desc`, `ldd`, `git diff --check`.
- Coherent install core + all plugins + icons.
- Один свежий daemon, live config, runtime log, `dmesg`, screenshot.
- Не включать `src/core/common.c` и `config-backups/` в disk_monitor commit.
