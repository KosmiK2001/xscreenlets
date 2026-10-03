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

# --- quirk-фикстуры ---
# По одной на каждое правило: qN/BAT0 — единственный источник в своём
# каталоге, иначе list_read вернул бы несколько элементов.

# q1: Charging при отрицательном токе (axp20x так пишут при разряде)
mkdir -p "$F/q1/BAT0"
printf 'Battery\n'      > "$F/q1/BAT0/type"
printf 'Charging\n'     > "$F/q1/BAT0/status"
printf '50\n'           > "$F/q1/BAT0/capacity"
printf -- '-500000\n'    > "$F/q1/BAT0/current_now"

# q2: 95 % но Discharging — батарея полна, статус врёт
mkdir -p "$F/q2/BAT0"
printf 'Battery\n'      > "$F/q2/BAT0/type"
printf 'Discharging\n'  > "$F/q2/BAT0/status"
printf '95\n'           > "$F/q2/BAT0/capacity"

# q3: capacity=0 + capacity_level=Low
mkdir -p "$F/q3/BAT0"
printf 'Battery\n'      > "$F/q3/BAT0/type"
printf 'Discharging\n'  > "$F/q3/BAT0/status"
printf '0\n'            > "$F/q3/BAT0/capacity"
printf 'Low\n'          > "$F/q3/BAT0/capacity_level"

# q4: capacity=0 + capacity_level=Unknown — данных нет
mkdir -p "$F/q4/BAT0"
printf 'Battery\n'      > "$F/q4/BAT0/type"
printf 'Discharging\n'  > "$F/q4/BAT0/status"
printf '0\n'            > "$F/q4/BAT0/capacity"
printf 'Unknown\n'      > "$F/q4/BAT0/capacity_level"

# q5: Not charging при 40 % — батарея садится
mkdir -p "$F/q5/BAT0"
printf 'Battery\n'      > "$F/q5/BAT0/type"
printf 'Not charging\n' > "$F/q5/BAT0/status"
printf '40\n'           > "$F/q5/BAT0/capacity"

# q6: Not charging при 95 % — уже полна
mkdir -p "$F/q6/BAT0"
printf 'Battery\n'      > "$F/q6/BAT0/type"
printf 'Not charging\n' > "$F/q6/BAT0/status"
printf '95\n'           > "$F/q6/BAT0/capacity"

# q7: батарейка извлечена
mkdir -p "$F/q7/BAT0"
printf 'Battery\n'      > "$F/q7/BAT0/type"
printf 'Unknown\n'      > "$F/q7/BAT0/status"
printf 'no\n'           > "$F/q7/BAT0/present"

# q8: Unknown при 100 % и нулевом токе — заряжена
mkdir -p "$F/q8/BAT0"
printf 'Battery\n'      > "$F/q8/BAT0/type"
printf 'Unknown\n'      > "$F/q8/BAT0/status"
printf '100\n'          > "$F/q8/BAT0/capacity"
printf '0\n'            > "$F/q8/BAT0/current_now"

# q9: 90 % при разряде. Регресс: порог 90 % раньше объявлял батарею
# полной, и applet писал "Full" вместо времени на отключённой от сетки
# батарее. Здесь status=Discharging и ток > 0 — идёт разряд, значит
# время до разряда полезнее, чем слово Full.
mkdir -p "$F/q9/BAT0"
printf 'Battery\n'      > "$F/q9/BAT0/type"
printf 'Discharging\n'  > "$F/q9/BAT0/status"
printf '90\n'           > "$F/q9/BAT0/capacity"
printf '1500000\n'      > "$F/q9/BAT0/current_now"

# q10: 90 % на зарядке при нулевом токе — зарядка упёрлась в порог BIOS,
# разряда нет, Full оправдан (регресс на прежнее поведение q8).
mkdir -p "$F/q10/BAT0"
printf 'Battery\n'      > "$F/q10/BAT0/type"
printf 'Full\n'         > "$F/q10/BAT0/status"
printf '90\n'           > "$F/q10/BAT0/capacity"
printf '0\n'            > "$F/q10/BAT0/current_now"

gcc -O0 -g3 -Wall -Wextra -I"$B/src/widgets" $(pkg-config --cflags glib-2.0) \
    "$B/tests/test_acpi_battery.c" "$B/src/widgets/acpi_battery_core.c" \
    -o /home/kosmik2001_dir/.hermes/cache/scratch/test_ab \
    $(pkg-config --libs glib-2.0) || exit 1

/home/kosmik2001_dir/.hermes/cache/scratch/test_ab "$F" "$F"
