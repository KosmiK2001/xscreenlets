CC = gcc
# Ловец падений (встроенный SIGSEGV-backtrace в main.c) включается
# -DXS_ENABLE_CRASH_LATCHER. По умолчанию ВЫКЛЮЧЕН: в ebuild это будет
# soft-debug USE-флаг (напр. "debug"), потому что ловец на каждый
# аварийный сигнал пишет стек в файл и завершает процесс — в обычной
# сборке пользователю это не нужно. Включать при расследовании падений.
CFLAGS = -O2 -g3 -Wall -Wextra -I./include -I./src/core -I./src/widgets -std=gnu11 $(shell pkg-config --cflags gtk+-3.0 librsvg-2.0 gmodule-2.0) $(shell pkg-config --cflags libsoup-3.0 libxml-2.0 json-glib-1.0 gio-2.0)

# Дополнительные флаги сборки задаются снаружи:
#   make EXTRA_CFLAGS="-DXS_MEM_DEBUG -O0 -g3" all
# Дописываются В КОНЕЦ, поэтому перекрывают базовые -O2/-g3.
# Зачем нужно: CFLAGS переопределён через =, и передать флаги в командной
# строке (make CFLAGS=...) нельзя - пришлось бы заново перечислять все
# -I и pkg-config, что легко рассинхронизировать с остальным Makefile.
EXTRA_CFLAGS =

# Системные каталоги. ebuild передаёт их через EXTRA_CFLAGS
# (-DXS_PLUGIN_DIR=... -DXS_THEME_DIR=...); здесь объявлены значения по
# умолчанию, чтобы make all работал без внешних флагов, и как точка
# подстановки для LOCALEDIR/TEXTDOMAIN ниже.
PLUGIN_DIR ?=
THEME_DIR ?=

# Переводы. Каталог задаётся снаружи тем же способом, что и XS_PLUGIN_DIR
# в ebuild: пустое значение = искать в $HOME (запуск из build/ без
# установки), непустое = системный каталог из пакета.
LOCALEDIR ?=
TEXTDOMAIN ?= xscreenlets
# ВНИМАНИЕ: это += НЕ работает, когда ebuild передаёт EXTRA_CFLAGS=...
# на командной строке make. Переменная из командной строки имеет
# приоритет над присваиваниями в makefile, и все += молча отбрасываются:
#
#   EXTRA_CFLAGS = base ; EXTRA_CFLAGS += FROMMAKEFILE
#   make            -> [base FROMMAKEFILE]
#   make EXTRA_CFLAGS=-DFROMCMDLINE  -> [-DFROMCMDLINE]   # потеряно
#
# Именно так был потерян -DXS_LOCALEDIR, а с ним весь gettext: демон
# собирался, ошибок не было, интерфейс оставался английским. Поэтому
# LOCALEDIR передаётся отдельной переменной XS_I18N_CFLAGS, которую
# командная строка не перекрывает.
# Флаги подключаются через CFLAGS, а не EXTRA_CFLAGS: именно CFLAGS
# ebuild на командной строке не передаёт, поэтому даже полное
# перекрытие EXTRA_CFLAGS не съест i18n-флаги.
XS_I18N_CFLAGS = -DXS_LOCALEDIR='"$(LOCALEDIR)"' -DXS_TEXTDOMAIN='"$(TEXTDOMAIN)"' -DHAVE_GETTEXT
CFLAGS += $(XS_I18N_CFLAGS)

# gettext() есть в glibc, поэтому -lintl не нужен; HAVE_GETTEXT включает
# макрос _() в i18n.h. Без него интерфейс остаётся английским, и проект
# продолжает собираться.
EXTRA_CFLAGS += -DXS_PLUGIN_DIR='"$(PLUGIN_DIR)"' -DXS_THEME_DIR='"$(THEME_DIR)"' 

LDFLAGS_DAEMON = $(shell pkg-config --libs gtk+-3.0 librsvg-2.0 gmodule-2.0) -lX11
LDFLAGS_PLUGIN = $(shell pkg-config --libs gtk+-3.0 librsvg-2.0 glib-2.0) -lm
LDFLAGS_RSS_PLUGIN = $(shell pkg-config --libs gtk+-3.0 librsvg-2.0 glib-2.0 libsoup-3.0 libxml-2.0) -lm
LDFLAGS_WEATHER_PLUGIN = $(shell pkg-config --libs gtk+-3.0 librsvg-2.0 glib-2.0 libsoup-3.0 json-glib-1.0 gio-2.0) -lm
LDFLAGS_STANDALONE = $(shell pkg-config --libs gtk+-3.0 librsvg-2.0 glib-2.0) -lm -lX11

PREFIX ?= $(HOME)
DESTDIR ?=
INSTALL ?= install

BUILD_DIR = build
TARGET_DAEMON = $(BUILD_DIR)/xscreenletsd
TARGET_PLUGIN = $(BUILD_DIR)/clock.so
TARGET_CAL_PLUGIN = $(BUILD_DIR)/calendar.so
TARGET_LAU_PLUGIN = $(BUILD_DIR)/launcher.so
TARGET_FL_PLUGIN = $(BUILD_DIR)/frame_launcher.so
TARGET_RSS_PLUGIN = $(BUILD_DIR)/clearrss.so
TARGET_WEATHER_PLUGIN = $(BUILD_DIR)/clearweather.so
TARGET_CPU_PLUGIN = $(BUILD_DIR)/cpu_monitor.so
TARGET_MEMORY_PLUGIN = $(BUILD_DIR)/memory_monitor.so
TARGET_DISK_PLUGIN = $(BUILD_DIR)/disk_monitor.so
TARGET_NETWORK_PLUGIN = $(BUILD_DIR)/network_monitor.so
TARGET_SENSORS_PLUGIN = $(BUILD_DIR)/sensors.so
TARGET_CONLOG_PLUGIN = $(BUILD_DIR)/conlog.so
TARGET_PROCESS_PLUGIN = $(BUILD_DIR)/process_list.so
TARGET_ACPI_BATTERY_PLUGIN = $(BUILD_DIR)/acpi_battery.so
TARGET_STANDALONE = $(BUILD_DIR)/xclock

SRC_COMMON = src/core/common.c
SRC_TRAY = src/core/tray.c
SRC_MAIN = src/core/main.c
SRC_CLOCK = src/widgets/clock.c
SRC_CALENDAR = src/widgets/calendar.c
SRC_LAUNCHER = src/widgets/launcher.c
SRC_FL = src/widgets/frame_launcher.c
SRC_RSS = src/widgets/clearrss.c

OBJS_COMMON = $(BUILD_DIR)/common.o $(BUILD_DIR)/applet_manager.o $(BUILD_DIR)/i18n.o
OBJS_TRAY = $(BUILD_DIR)/tray.o
OBJS_MAIN = $(BUILD_DIR)/main.o
OBJS_CLOCK = $(BUILD_DIR)/clock.o
OBJS_STANDALONE_CLOCK = $(BUILD_DIR)/standalone_clock.o
OBJS_CALENDAR = $(BUILD_DIR)/calendar.o
OBJS_LAUNCHER = $(BUILD_DIR)/launcher.o
OBJS_FL = $(BUILD_DIR)/frame_launcher.o
OBJS_RSS = $(BUILD_DIR)/clearrss.o
OBJS_COMMON_DBG = $(BUILD_DIR)/common_dbg.o
OBJS_TRAY_DBG = $(BUILD_DIR)/tray_dbg.o
OBJS_CLOCK_DBG = $(BUILD_DIR)/clock_dbg.o
OBJS_STANDALONE_DBG = $(BUILD_DIR)/standalone_dbg.o

all: $(TARGET_DAEMON) $(TARGET_PLUGIN) $(TARGET_CAL_PLUGIN) $(TARGET_LAU_PLUGIN) $(TARGET_FL_PLUGIN) $(TARGET_RSS_PLUGIN) $(TARGET_WEATHER_PLUGIN) $(TARGET_CPU_PLUGIN) $(TARGET_MEMORY_PLUGIN) $(TARGET_DISK_PLUGIN) $(TARGET_NETWORK_PLUGIN) $(TARGET_PROCESS_PLUGIN) $(TARGET_STANDALONE) $(TARGET_SENSORS_PLUGIN) $(TARGET_ACPI_BATTERY_PLUGIN)

$(BUILD_DIR):
	mkdir -p $(BUILD_DIR)

$(BUILD_DIR)/acpi_battery_core.o: src/widgets/acpi_battery_core.c src/widgets/acpi_battery_core.h | $(BUILD_DIR)
	$(CC) $(CFLAGS) $(EXTRA_CFLAGS) -fPIC -c $< -o $@

$(BUILD_DIR)/acpi_battery.o: src/widgets/acpi_battery.c src/widgets/acpi_battery_core.h include/xs_api.h src/core/common.h | $(BUILD_DIR)
	$(CC) $(CFLAGS) $(EXTRA_CFLAGS) -fPIC -c $< -o $@

$(TARGET_ACPI_BATTERY_PLUGIN): build/acpi_battery.o build/acpi_battery_core.o | $(BUILD_DIR)
	$(CC) $(CFLAGS) $(EXTRA_CFLAGS) -shared -fPIC -o $@ build/acpi_battery.o build/acpi_battery_core.o $(LDFLAGS_PLUGIN)

$(TARGET_DAEMON): $(OBJS_COMMON) $(OBJS_TRAY) $(OBJS_MAIN) | $(BUILD_DIR)
	$(CC) $(CFLAGS) $(EXTRA_CFLAGS) -o $@ $(OBJS_COMMON) $(OBJS_TRAY) $(OBJS_MAIN) $(LDFLAGS_DAEMON)

$(TARGET_PLUGIN): $(OBJS_CLOCK) | $(BUILD_DIR)
	$(CC) $(CFLAGS) $(EXTRA_CFLAGS) -shared -fPIC -o $@ $(OBJS_CLOCK) $(LDFLAGS_PLUGIN)

$(TARGET_CAL_PLUGIN): $(OBJS_CALENDAR) | $(BUILD_DIR)
	$(CC) $(CFLAGS) $(EXTRA_CFLAGS) -shared -fPIC -o $@ $(OBJS_CALENDAR) $(LDFLAGS_PLUGIN)

$(TARGET_LAU_PLUGIN): $(OBJS_LAUNCHER) | $(BUILD_DIR)
	$(CC) $(CFLAGS) $(EXTRA_CFLAGS) -shared -fPIC -o $@ $(OBJS_LAUNCHER) $(LDFLAGS_PLUGIN) $(shell pkg-config --libs gdk-pixbuf-2.0)

$(TARGET_FL_PLUGIN): $(OBJS_FL) | $(BUILD_DIR)
	$(CC) $(CFLAGS) $(EXTRA_CFLAGS) -shared -fPIC -o $@ $(OBJS_FL) $(LDFLAGS_PLUGIN)

$(TARGET_RSS_PLUGIN): $(OBJS_RSS) | $(BUILD_DIR)
	$(CC) $(CFLAGS) $(EXTRA_CFLAGS) -shared -fPIC -o $@ $(OBJS_RSS) $(LDFLAGS_RSS_PLUGIN)

$(TARGET_CPU_PLUGIN): build/cpu_monitor.o | $(BUILD_DIR)
	$(CC) $(CFLAGS) $(EXTRA_CFLAGS) -shared -fPIC -o $@ build/cpu_monitor.o $(LDFLAGS_PLUGIN)

$(BUILD_DIR)/memory_monitor_core.o: src/widgets/memory_monitor_core.c src/widgets/memory_monitor_core.h | $(BUILD_DIR)
	$(CC) $(CFLAGS) $(EXTRA_CFLAGS) -fPIC -c $< -o $@

$(BUILD_DIR)/memory_monitor.o: src/widgets/memory_monitor.c src/widgets/memory_monitor_core.h include/xs_api.h src/core/common.h | $(BUILD_DIR)
	$(CC) $(CFLAGS) $(EXTRA_CFLAGS) -fPIC -c $< -o $@

$(TARGET_MEMORY_PLUGIN): build/memory_monitor.o build/memory_monitor_core.o | $(BUILD_DIR)
	$(CC) $(CFLAGS) $(EXTRA_CFLAGS) -shared -fPIC -o $@ build/memory_monitor.o build/memory_monitor_core.o $(LDFLAGS_PLUGIN)

$(BUILD_DIR)/disk_monitor_core.o: src/widgets/disk_monitor_core.c src/widgets/disk_monitor_core.h | $(BUILD_DIR)
	$(CC) $(CFLAGS) $(EXTRA_CFLAGS) -fPIC -c $< -o $@

$(BUILD_DIR)/disk_monitor.o: src/widgets/disk_monitor.c src/widgets/disk_monitor_core.h include/xs_api.h src/core/common.h | $(BUILD_DIR)
	$(CC) $(CFLAGS) $(EXTRA_CFLAGS) -fPIC -c $< -o $@

$(TARGET_DISK_PLUGIN): build/disk_monitor.o build/disk_monitor_core.o | $(BUILD_DIR)
	$(CC) $(CFLAGS) $(EXTRA_CFLAGS) -shared -fPIC -o $@ build/disk_monitor.o build/disk_monitor_core.o $(LDFLAGS_PLUGIN)

$(BUILD_DIR)/network_monitor_core.o: src/widgets/network_monitor_core.c src/widgets/network_monitor_core.h | $(BUILD_DIR)
	$(CC) $(CFLAGS) $(EXTRA_CFLAGS) -fPIC -c $< -o $@

$(BUILD_DIR)/network_monitor.o: src/widgets/network_monitor.c src/widgets/network_monitor_core.h include/xs_api.h src/core/common.h | $(BUILD_DIR)
	$(CC) $(CFLAGS) $(EXTRA_CFLAGS) -fPIC -c $< -o $@

$(TARGET_NETWORK_PLUGIN): build/network_monitor.o build/network_monitor_core.o | $(BUILD_DIR)
	$(CC) $(CFLAGS) $(EXTRA_CFLAGS) -shared -fPIC -o $@ build/network_monitor.o build/network_monitor_core.o $(LDFLAGS_PLUGIN)

$(BUILD_DIR)/sensors_core.o: src/widgets/sensors_core.c src/widgets/sensors_core.h | $(BUILD_DIR)
	$(CC) $(CFLAGS) $(EXTRA_CFLAGS) -fPIC -c $< -o $@
$(BUILD_DIR)/sensors_nvml.o: src/widgets/sensors_nvml.c src/widgets/sensors_nvml.h src/widgets/sensors_core.h | $(BUILD_DIR)
$(BUILD_DIR)/sensors.o: src/widgets/sensors.c src/widgets/sensors_core.h src/widgets/sensors_nvml.h include/xs_api.h src/core/common.h | $(BUILD_DIR)
	$(CC) $(CFLAGS) $(EXTRA_CFLAGS) -fPIC -c $< -o $@
$(TARGET_SENSORS_PLUGIN): build/sensors.o build/sensors_core.o build/sensors_nvml.o | $(BUILD_DIR)
	$(CC) $(CFLAGS) $(EXTRA_CFLAGS) -shared -fPIC -o $@ build/sensors.o build/sensors_core.o build/sensors_nvml.o $(LDFLAGS_PLUGIN) -ldl

$(BUILD_DIR)/conlog_core.o: src/widgets/conlog_core.c src/widgets/conlog_core.h | $(BUILD_DIR)
	$(CC) $(CFLAGS) $(EXTRA_CFLAGS) -fPIC -c $< -o $@
$(BUILD_DIR)/conlog.o: src/widgets/conlog.c src/widgets/conlog_core.h include/xs_api.h src/core/common.h | $(BUILD_DIR)
	$(CC) $(CFLAGS) $(EXTRA_CFLAGS) -fPIC -c $< -o $@
$(TARGET_CONLOG_PLUGIN): build/conlog.o build/conlog_core.o | $(BUILD_DIR)
	$(CC) $(CFLAGS) $(EXTRA_CFLAGS) -shared -fPIC -o $@ build/conlog.o build/conlog_core.o $(LDFLAGS_PLUGIN)

# Урезанная версия conlog: только вывод строк, без conlog_core.
# Что убрано и как возвращать — src/widgets/CONLOG-FUNCTIONALITY.md.
# conlog.c и conlog_core.c остаются на месте нетронутыми.
$(BUILD_DIR)/clearweather.o: src/widgets/clearweather.c include/xs_api.h src/core/common.h | $(BUILD_DIR)
	$(CC) $(CFLAGS) $(EXTRA_CFLAGS) -fPIC -c $< -o $@
$(TARGET_WEATHER_PLUGIN): build/clearweather.o | $(BUILD_DIR)
	$(CC) $(CFLAGS) $(EXTRA_CFLAGS) -shared -fPIC -o $@ build/clearweather.o $(LDFLAGS_WEATHER_PLUGIN)

$(BUILD_DIR)/conlog_min.o: src/widgets/conlog_min.c include/xs_api.h src/core/common.h | $(BUILD_DIR)
	$(CC) $(CFLAGS) $(EXTRA_CFLAGS) -fPIC -c $< -o $@
$(BUILD_DIR)/conlog_min.so: $(BUILD_DIR) build/conlog_min.o
	$(CC) $(CFLAGS) $(EXTRA_CFLAGS) -shared -fPIC -o $@ build/conlog_min.o $(LDFLAGS_PLUGIN)

$(TARGET_PROCESS_PLUGIN): build/process_list.o build/process_list_core.o | $(BUILD_DIR)
	$(CC) $(CFLAGS) $(EXTRA_CFLAGS) -shared -fPIC -o $@ build/process_list.o build/process_list_core.o $(LDFLAGS_PLUGIN)
$(TARGET_STANDALONE): $(OBJS_COMMON) $(OBJS_CLOCK) $(OBJS_STANDALONE_CLOCK) $(OBJS_TRAY) | $(BUILD_DIR)
	$(CC) $(CFLAGS) $(EXTRA_CFLAGS) -o $@ $(OBJS_COMMON) $(OBJS_CLOCK) $(OBJS_STANDALONE_CLOCK) $(OBJS_TRAY) $(LDFLAGS_STANDALONE)

# Pattern rules for object files (пересборка при изменении заголовков)
# -fPIC обязателен: этим правилом собираются common.o и applet_manager.o,
# которые идут и в .so, и в исполняемые файлы. Без флага сборка
# опиралась на то, что x86-64 gcc по умолчанию генерирует PIC-код - на
# другой архитектуре или при -fno-pic линковка .so падала бы.
$(BUILD_DIR)/%.o: src/core/%.c include/xs_api.h src/core/common.h | $(BUILD_DIR)
	$(CC) $(CFLAGS) $(EXTRA_CFLAGS) -fPIC -c $< -o $@

$(BUILD_DIR)/%.o: src/widgets/%.c include/xs_api.h src/core/common.h | $(BUILD_DIR)
	$(CC) $(CFLAGS) $(EXTRA_CFLAGS) -fPIC -c $< -o $@

$(BUILD_DIR)/test_memory_monitor: tests/test_memory_monitor.c src/widgets/memory_monitor_core.c src/widgets/memory_monitor_core.h
	$(CC) $(CFLAGS) $(EXTRA_CFLAGS) -o $@ tests/test_memory_monitor.c src/widgets/memory_monitor_core.c $(LDFLAGS_PLUGIN)

$(BUILD_DIR)/test_disk_monitor: tests/test_disk_monitor.c src/widgets/disk_monitor_core.c src/widgets/disk_monitor_core.h
	$(CC) $(CFLAGS) $(EXTRA_CFLAGS) -o $@ tests/test_disk_monitor.c src/widgets/disk_monitor_core.c $(LDFLAGS_PLUGIN)

$(BUILD_DIR)/test_network_monitor: tests/test_network_monitor.c src/widgets/network_monitor_core.c src/widgets/network_monitor_core.h
	$(CC) $(CFLAGS) $(EXTRA_CFLAGS) -o $@ tests/test_network_monitor.c src/widgets/network_monitor_core.c $(LDFLAGS_PLUGIN)
$(BUILD_DIR)/test_sensors: tests/test_sensors.c src/widgets/sensors_core.c src/widgets/sensors_core.h src/widgets/sensors_nvml.c src/widgets/sensors_nvml.h
	$(CC) $(CFLAGS) $(EXTRA_CFLAGS) -o $@ tests/test_sensors.c src/widgets/sensors_core.c src/widgets/sensors_nvml.c $(LDFLAGS_PLUGIN) -ldl
$(BUILD_DIR)/test_conlog: tests/test_conlog.c src/widgets/conlog_core.c src/widgets/conlog_core.h
	$(CC) $(CFLAGS) $(EXTRA_CFLAGS) -o $@ tests/test_conlog.c src/widgets/conlog_core.c $(LDFLAGS_PLUGIN)

$(BUILD_DIR)/test_process_list: tests/test_process_list.c src/widgets/process_list_core.c src/widgets/process_list_core.h
	$(CC) $(CFLAGS) $(EXTRA_CFLAGS) -o $@ tests/test_process_list.c src/widgets/process_list_core.c $(LDFLAGS_PLUGIN)

test: $(BUILD_DIR)/test_memory_monitor $(BUILD_DIR)/test_disk_monitor $(BUILD_DIR)/test_network_monitor $(BUILD_DIR)/test_sensors $(BUILD_DIR)/test_conlog $(BUILD_DIR)/test_process_list
	$(BUILD_DIR)/test_memory_monitor
	$(BUILD_DIR)/test_disk_monitor
	$(BUILD_DIR)/test_network_monitor
	$(BUILD_DIR)/test_sensors
	$(BUILD_DIR)/test_conlog
	$(BUILD_DIR)/test_process_list

clean:
	rm -rf $(BUILD_DIR)

# ВНИМАНИЕ, ЛОВУШКА: install ставит ТЯЖЁЛЫЙ build/conlog.so как
# plugins/conlog.so и молча откатит урезанный applet (с его 90% CPU на
# большом потоке). Урезанный вариант ставится вручную:
#   cp build/conlog_min.so ~/lib/xscreenlets/plugins/conlog.so
# Подробности — src/widgets/CONLOG-FUNCTIONALITY.md

# ==== Переводы интерфейса ====
# Каталог .mo-файлов собирается из po/*.po. Список языков берётся из
# po/LINGUAS, чтобы добавление языка не требовало правки Makefile.
LINGUAS = $(patsubst po/%.po,%,$(wildcard po/*.po))

POTFILES := $(shell sed -e '/^#/d' -e '/^$$/d' po/POTFILES.in)


# Помеченные для перевода функции. Кроме "_" есть хелперы, которые
# принимают подпись и сами вызывают gtk_label_new(_(label)): подписи
# передаются литералами в десятках вызовов, и в разметке они остаются
# обычными строками. Без --keyword их xgettext не видит, и перевод молча
# не работает - ровно тот дефект, который пришлось искать вручную.
#
#   nm_grid_add_label  подписи полей сетевого монитора
#   nm_section         заголовки секций сетевого монитора
#   cl_row             подписи полей conlog
#   sen_row            подписи полей sensors
#
# Номера аргументов обязательны. Без суффикса ":N" xgettext берёт ТОЛЬКО
# первый строковый аргумент функции, а у этих хелперов первый - GtkWidget
# или ключ конфига:
#
#   cl_row(grid, row, label, w)          label - третий
#   sen_row(g, label, widget)            label - второй
#   nm_grid_add_label(g, label)          label - второй
#   nm_section(page, title)              title - второй
#
# Проверено: без номеров все четыре дают 0 извлечённых строк, с номерами
# 16, 17, 2 и 5 соответственно.
#
# Плюс nm_* : первый аргумент - ключ конфига, но он не переводится
# (не находит msgstr и остаётся как есть), так что в POT попадает и он.
# clock_props_group сюда НЕ входит: у него два строковых аргумента
# (title и info), а при двух ключах одного имени xgettext берёт
# ПОСЛЕДНИЙ - второй потерял бы первый молча. Проверено:
#
#   --keyword=f:2 --keyword=f:3  -> извлекается только 3-й
#
# В clock.c всего три вызова, поэтому оба аргумента переведены
# прямо на стороне вызовов - дешевле и без двусмысленности.
XGETTEXT_KEYWORDS = _ cl_row:3 sen_row:2 nm_grid_add_label:2 nm_section:2 \
                  gtk_combo_box_text_append_text

# Шаблон: msgid из исходников. Перегенерировать после правки строк:
#   make po-update
#
# Зависимость только от POTFILES, поэтому правка Makefile (например
# списка --keyword) НЕ пересобирает .pot: цель считается up-to-date и
# po-update молча копирует старый шаблон. Из-за этого не extraction, а
# устаревший timestamp. Правится принудительным удалением build/po.
$(BUILD_DIR)/po/xscreenlets.pot: $(POTFILES) Makefile
	xgettext --from-code=UTF-8 --language=C \
		$(foreach k,$(XGETTEXT_KEYWORDS),--keyword=$(k)) \
		--add-comments=Translators --package-name=xscreenlets \
		--copyright-holder=KosmiK2001 --output=$@ $(POTFILES)

po-update: $(BUILD_DIR)/po/xscreenlets.pot
	cp $< po/xscreenlets.pot
	@echo "обновлён po/xscreenlets.pot; проверьте po/*.po через msgmerge"

# .mo собираются в build/locale/<lang>/LC_MESSAGES/<domain>.mo -
# такую раскладку ожидает gettext.
LOCALES_OUT = $(foreach l,$(LINGUAS),\
	$(BUILD_DIR)/locale/$(l)/LC_MESSAGES/xscreenlets.mo)

$(BUILD_DIR)/locale/%/LC_MESSAGES/xscreenlets.mo: po/%.po
	@mkdir -p $(dir $@)
	msgfmt --check --output-file=$@ $<

locale: $(LOCALES_OUT)

# Тест переводов: открывает окно с надписями Properties, чтобы перевод
# был виден глазами. Проверять через lsof бессмысленно - gettext читает
# .mo лениво, при первом вызове _(), то есть только после создания
# окна. Сама проверка: LANGUAGE=ru XSCREENLETS_LOCALEDIR=build/locale
# make test-i18n-window
$(BUILD_DIR)/test_i18n_window: tests/test_i18n_window.c src/core/i18n.c \
		include/xs_api.h src/core/common.h | $(BUILD_DIR)
	$(CC) $(CFLAGS) $(EXTRA_CFLAGS) -o $@ tests/test_i18n_window.c \
		src/core/i18n.c $(LDFLAGS_PLUGIN)

# Проверка пунктов меню: трей (tray.c) и контекстное меню апплета
# (common.c). Отдельный тест, потому что это два разных файла, и жалобы
# на них приходили отдельно: свойства перевелись, а меню осталось
# английским.
$(BUILD_DIR)/test_i18n_menu: tests/test_i18n_menu.c | $(BUILD_DIR)
	$(CC) $(CFLAGS) $(EXTRA_CFLAGS) -o $@ tests/test_i18n_menu.c

test-i18n-menu: $(BUILD_DIR)/test_i18n_menu locale
	@$(BUILD_DIR)/test_i18n_menu $(BUILD_DIR)/locale

test-i18n-window: $(BUILD_DIR)/test_i18n_window locale
	@echo "Открылось окно с переводами. Закройте его, когда посмотрите."
	@XSCREENLETS_LOCALEDIR=$(BUILD_DIR)/locale $(BUILD_DIR)/test_i18n_window


install: all locale
	$(INSTALL) -d $(DESTDIR)$(PREFIX)/bin
	$(INSTALL) -m 0755 $(TARGET_DAEMON) $(DESTDIR)$(PREFIX)/bin/xscreenletsd
	$(INSTALL) -d $(DESTDIR)$(PREFIX)/lib/xscreenlets/plugins
	$(INSTALL) -m 0755 $(TARGET_PLUGIN) $(DESTDIR)$(PREFIX)/lib/xscreenlets/plugins/clock.so
	$(INSTALL) -m 0755 $(TARGET_CAL_PLUGIN) $(DESTDIR)$(PREFIX)/lib/xscreenlets/plugins/calendar.so
	$(INSTALL) -m 0755 $(TARGET_LAU_PLUGIN) $(DESTDIR)$(PREFIX)/lib/xscreenlets/plugins/launcher.so
	$(INSTALL) -m 0755 $(TARGET_FL_PLUGIN) $(DESTDIR)$(PREFIX)/lib/xscreenlets/plugins/frame_launcher.so
	$(INSTALL) -m 0755 $(TARGET_RSS_PLUGIN) $(DESTDIR)$(PREFIX)/lib/xscreenlets/plugins/clearrss.so
	$(INSTALL) -m 0755 $(TARGET_CPU_PLUGIN) $(DESTDIR)$(PREFIX)/lib/xscreenlets/plugins/cpu_monitor.so
	$(INSTALL) -m 0755 $(TARGET_MEMORY_PLUGIN) $(DESTDIR)$(PREFIX)/lib/xscreenlets/plugins/memory_monitor.so
	$(INSTALL) -m 0755 $(TARGET_DISK_PLUGIN) $(DESTDIR)$(PREFIX)/lib/xscreenlets/plugins/disk_monitor.so
	$(INSTALL) -m 0755 $(TARGET_NETWORK_PLUGIN) $(DESTDIR)$(PREFIX)/lib/xscreenlets/plugins/network_monitor.so
	$(INSTALL) -m 0755 $(TARGET_SENSORS_PLUGIN) $(DESTDIR)$(PREFIX)/lib/xscreenlets/plugins/sensors.so
	@# conlog НЕ входит в цель all (исходники есть, но он собирается
	@# отдельно), поэтому build/conlog.so обычно отсутствует - и install
	@# на нём падал, не доходя до остального. Ставим только если файл
	@# реально есть. Минус перед install делает отсутствие не ошибкой.
	@# ВНИМАНИЕ, ЛОВУШКА (см. CONLOG-FUNCTIONALITY.md): тяжёлый conlog.so
	@# откатывает урезанный апплет, который иначе ест ~90% CPU.
	-[ -f $(TARGET_CONLOG_PLUGIN) ] && \
		$(INSTALL) -m 0755 $(TARGET_CONLOG_PLUGIN) $(DESTDIR)$(PREFIX)/lib/xscreenlets/plugins/conlog.so || true
	$(INSTALL) -m 0755 $(TARGET_PROCESS_PLUGIN) $(DESTDIR)$(PREFIX)/lib/xscreenlets/plugins/process_list.so
	$(INSTALL) -d $(DESTDIR)$(PREFIX)/lib/xscreenlets/icons
	$(INSTALL) -m 0644 icons/clearrss.svg $(DESTDIR)$(PREFIX)/lib/xscreenlets/icons/clearrss.svg
	$(INSTALL) -m 0644 icons/cpu_monitor.svg $(DESTDIR)$(PREFIX)/lib/xscreenlets/icons/cpu_monitor.svg
	$(INSTALL) -m 0644 icons/memory_monitor.svg $(DESTDIR)$(PREFIX)/lib/xscreenlets/icons/memory_monitor.svg
	$(INSTALL) -m 0644 icons/disk_monitor.svg $(DESTDIR)$(PREFIX)/lib/xscreenlets/icons/disk_monitor.svg
	$(INSTALL) -m 0644 icons/process_list.svg $(DESTDIR)$(PREFIX)/lib/xscreenlets/icons/process_list.svg
	@# Темы ставим ЦИКЛОМ по каталогам, а не списком файлов. Перечисление
	@# перечислением уже приводило к потере: тени кнопок (shadow.svg,
	@# shadow_mid.svg, button_bg.svg) и Simple/shadow.svg были закоммичены,
	@# но в install попадал только background.svg - и на свежей машине
	@# апплет выходил без теней. Цикл добавляет новые файлы сам.
	@for t in themes/clearrss/*/; do \
		name=$$(basename $$t); \
		$(INSTALL) -d $(DESTDIR)$(PREFIX)/lib/xscreenlets/themes/clearrss/$$name; \
		for f in $$t*.svg; do \
			[ -f "$$f" ] || continue; \
			$(INSTALL) -m 0644 "$$f" $(DESTDIR)$(PREFIX)/lib/xscreenlets/themes/clearrss/$$name/; \
		done; \
	done
	@# Переводы: циклом по LINGUAS, как темы выше. Каталог повторяет
	@# путь, который ищет gettext: <localedir>/<lang>/LC_MESSAGES/<domain>.mo
	@# Плоская раскладка (lang.mo рядом) не работает и молча даёт
	@# английский интерфейс - это самая частая ошибка при установке.
	@for mo in $(LOCALES_OUT); do \
		lang=$$(basename $$(dirname $$(dirname $$mo))); \
		d=$(DESTDIR)$(PREFIX)/share/locale/$$lang/LC_MESSAGES; \
		$(INSTALL) -d $$d; \
		$(INSTALL) -m 0644 $$mo $$d/xscreenlets.mo; \
	done

run: all
	@echo "To run the daemon: ./$(TARGET_DAEMON)"
	@echo "To run the standalone clock: ./$(TARGET_STANDALONE)"

# debug — промежуточная сборка: -O0 + макросы + фреймы (как у shaping-view-gtk3)
BIN_DBG = $(BUILD_DIR)/xscreenletsd-debug
XCLK_DBG = $(BUILD_DIR)/xclock-debug
debug: CFLAGS += -O0 -DDEBUG -fno-omit-frame-pointer
debug: all $(BIN_DBG) $(XCLK_DBG)

$(BIN_DBG): $(OBJS_COMMON) $(OBJS_TRAY) $(OBJS_MAIN) | $(BUILD_DIR)
	$(CC) $(CFLAGS) $(EXTRA_CFLAGS) -o $@ $(OBJS_COMMON) $(OBJS_TRAY) $(OBJS_MAIN) $(LDFLAGS_DAEMON)

$(XCLK_DBG): $(OBJS_COMMON_DBG) $(OBJS_CLOCK_DBG) $(OBJS_STANDALONE_DBG) $(OBJS_TRAY_DBG) | $(BUILD_DIR)
	$(CC) $(CFLAGS) $(EXTRA_CFLAGS) -o $@ $(OBJS_COMMON_DBG) $(OBJS_CLOCK_DBG) $(OBJS_STANDALONE_DBG) $(OBJS_TRAY_DBG) $(LDFLAGS_STANDALONE)

$(BUILD_DIR)/common_dbg.o: src/core/common.c | $(BUILD_DIR)
	$(CC) $(CFLAGS) $(EXTRA_CFLAGS) -c $< -o $@
$(BUILD_DIR)/clock_dbg.o: src/widgets/clock.c | $(BUILD_DIR)
	$(CC) $(CFLAGS) $(EXTRA_CFLAGS) -c $< -o $@
$(BUILD_DIR)/standalone_dbg.o: src/widgets/standalone_clock.c | $(BUILD_DIR)
	$(CC) $(CFLAGS) $(EXTRA_CFLAGS) -c $< -o $@
$(BUILD_DIR)/tray_dbg.o: src/core/tray.c | $(BUILD_DIR)
	$(CC) $(CFLAGS) $(EXTRA_CFLAGS) -c $< -o $@

# sanitize — с AddressSanitizer/UBSan (те же флаги у отладочной цели)
BIN_SAN = $(BUILD_DIR)/xscreenletsd-san
sanitize: CFLAGS += -O1 -fsanitize=address,undefined -fno-omit-frame-pointer
sanitize: all $(BIN_SAN)
$(BIN_SAN): $(OBJS_COMMON) $(OBJS_TRAY) $(OBJS_MAIN) | $(BUILD_DIR)
	$(CC) $(CFLAGS) $(EXTRA_CFLAGS) -o $@ $(OBJS_COMMON) $(OBJS_TRAY) $(OBJS_MAIN) $(LDFLAGS_DAEMON) -fsanitize=address,undefined

.PHONY: all clean test run install debug sanitize
