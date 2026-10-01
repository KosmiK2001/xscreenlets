#!/bin/bash
# Тест acpi_battery_core на фикстуре sysfs.
# Фикстура повторяет раскладку /sys/class/power_supply: каждый источник —
# каталог с файлами type/status/capacity/energy_*/power_now.
set -u
B=/home/kosmik2001_dir/Документы/GLM-5.3-Kimi/xscreenlets
F=/home/kosmik2001_dir/.hermes/cache/scratch/abat-fixture

rm -rf "$F"
mkdir -p "$F/BAT0" "$F/hidpp_battery_0" "$F/ac"

# ноутбук: разряжается, 42%, 12.6 из 30 Wh при 10 W
printf 'Battery\n'      > "$F/BAT0/type"
printf 'Discharging\n'  > "$F/BAT0/status"
printf '42\n'           > "$F/BAT0/capacity"
printf '12600000\n'     > "$F/BAT0/energy_now"
printf '30000000\n'     > "$F/BAT0/energy_full"
printf '10000000\n'     > "$F/BAT0/power_now"
printf 'yes\n'          > "$F/BAT0/present"

# мышь Logitech: тип Mouse, заряжается, 60%
printf 'Mouse\n'        > "$F/hidpp_battery_0/type"
printf 'Charging\n'     > "$F/hidpp_battery_0/status"
printf '60\n'           > "$F/hidpp_battery_0/capacity"

# не батарея: тип UPS — должен быть отброшен
printf 'UPS\n'          > "$F/ac/type"
printf 'Unknown\n'      > "$F/ac/status"
printf '50\n'           > "$F/ac/capacity"

gcc -O0 -g3 -Wall -Wextra -I"$B/src/widgets" $(pkg-config --cflags glib-2.0) \
    "$B/tests/test_acpi_battery.c" "$B/src/widgets/acpi_battery_core.c" \
    -o /home/kosmik2001_dir/.hermes/cache/scratch/test_ab \
    $(pkg-config --libs glib-2.0) || exit 1

/home/kosmik2001_dir/.hermes/cache/scratch/test_ab "$F"