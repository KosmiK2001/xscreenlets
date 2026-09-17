CC = gcc
CFLAGS = -O2 -g3 -Wall -Wextra -I./include -I./src/core -std=gnu11 $(shell pkg-config --cflags gtk+-3.0 librsvg-2.0 gmodule-2.0)
LDFLAGS_DAEMON = $(shell pkg-config --libs gtk+-3.0 librsvg-2.0 gmodule-2.0)
LDFLAGS_PLUGIN = $(shell pkg-config --libs gtk+-3.0 librsvg-2.0 glib-2.0) -lm
LDFLAGS_STANDALONE = $(shell pkg-config --libs gtk+-3.0 librsvg-2.0 glib-2.0) -lm

PREFIX ?= $(HOME)
DESTDIR ?=
INSTALL ?= install

BUILD_DIR = build
TARGET_DAEMON = $(BUILD_DIR)/xscreenletsd
TARGET_PLUGIN = $(BUILD_DIR)/clock.so
TARGET_CAL_PLUGIN = $(BUILD_DIR)/calendar.so
TARGET_LAU_PLUGIN = $(BUILD_DIR)/launcher.so
TARGET_STANDALONE = $(BUILD_DIR)/xclock

SRC_COMMON = src/core/common.c
SRC_TRAY = src/core/tray.c
SRC_MAIN = src/core/main.c
SRC_CLOCK = src/widgets/clock.c
SRC_CALENDAR = src/widgets/calendar.c
SRC_LAUNCHER = src/widgets/launcher.c

OBJS_COMMON = $(BUILD_DIR)/common.o
OBJS_TRAY = $(BUILD_DIR)/tray.o
OBJS_MAIN = $(BUILD_DIR)/main.o
OBJS_CLOCK = $(BUILD_DIR)/clock.o
OBJS_STANDALONE_CLOCK = $(BUILD_DIR)/standalone_clock.o
OBJS_CALENDAR = $(BUILD_DIR)/calendar.o
OBJS_LAUNCHER = $(BUILD_DIR)/launcher.o
OBJS_COMMON_DBG = $(BUILD_DIR)/common_dbg.o
OBJS_TRAY_DBG = $(BUILD_DIR)/tray_dbg.o
OBJS_CLOCK_DBG = $(BUILD_DIR)/clock_dbg.o
OBJS_STANDALONE_DBG = $(BUILD_DIR)/standalone_dbg.o

all: $(TARGET_DAEMON) $(TARGET_PLUGIN) $(TARGET_CAL_PLUGIN) $(TARGET_LAU_PLUGIN) $(TARGET_STANDALONE)

$(BUILD_DIR):
	mkdir -p $(BUILD_DIR)

$(TARGET_DAEMON): $(BUILD_DIR) $(OBJS_COMMON) $(OBJS_TRAY) $(OBJS_MAIN)
	$(CC) $(CFLAGS) -o $@ $(OBJS_COMMON) $(OBJS_TRAY) $(OBJS_MAIN) $(LDFLAGS_DAEMON)

$(TARGET_PLUGIN): $(BUILD_DIR) $(OBJS_CLOCK)
	$(CC) $(CFLAGS) -shared -fPIC -o $@ $(OBJS_CLOCK) $(LDFLAGS_PLUGIN)

$(TARGET_CAL_PLUGIN): $(BUILD_DIR) $(OBJS_CALENDAR)
	$(CC) $(CFLAGS) -shared -fPIC -o $@ $(OBJS_CALENDAR) $(LDFLAGS_PLUGIN)

$(TARGET_LAU_PLUGIN): $(BUILD_DIR) $(OBJS_LAUNCHER)
	$(CC) $(CFLAGS) -shared -fPIC -o $@ $(OBJS_LAUNCHER) $(LDFLAGS_PLUGIN) $(shell pkg-config --libs gdk-pixbuf-2.0)

$(TARGET_STANDALONE): $(BUILD_DIR) $(OBJS_COMMON) $(OBJS_CLOCK) $(OBJS_STANDALONE_CLOCK) $(OBJS_TRAY)
	$(CC) $(CFLAGS) -o $@ $(OBJS_COMMON) $(OBJS_CLOCK) $(OBJS_STANDALONE_CLOCK) $(OBJS_TRAY) $(LDFLAGS_STANDALONE)

# Pattern rules for object files
$(BUILD_DIR)/%.o: src/core/%.c
	$(CC) $(CFLAGS) -c $< -o $@

$(BUILD_DIR)/%.o: src/widgets/%.c
	$(CC) $(CFLAGS) -fPIC -c $< -o $@

clean:
	rm -rf $(BUILD_DIR)

install: all
	$(INSTALL) -d $(DESTDIR)$(PREFIX)/bin
	$(INSTALL) -m 0755 $(TARGET_DAEMON) $(DESTDIR)$(PREFIX)/bin/xscreenletsd
	$(INSTALL) -d $(DESTDIR)$(PREFIX)/lib/xscreenlets/plugins
	$(INSTALL) -m 0755 $(TARGET_PLUGIN) $(DESTDIR)$(PREFIX)/lib/xscreenlets/plugins/clock.so
	$(INSTALL) -m 0755 $(TARGET_CAL_PLUGIN) $(DESTDIR)$(PREFIX)/lib/xscreenlets/plugins/calendar.so
	$(INSTALL) -m 0755 $(TARGET_LAU_PLUGIN) $(DESTDIR)$(PREFIX)/lib/xscreenlets/plugins/launcher.so

run: all
	@echo "To run the daemon: ./$(TARGET_DAEMON)"
	@echo "To run the standalone clock: ./$(TARGET_STANDALONE)"


# debug — промежуточная сборка: -O0 + макросы + фреймы (как у shaping-view-gtk3)
BIN_DBG = $(BUILD_DIR)/xscreenletsd-debug
XCLK_DBG = $(BUILD_DIR)/xclock-debug
debug: CFLAGS += -O0 -DDEBUG -fno-omit-frame-pointer
debug: all $(BIN_DBG) $(XCLK_DBG)

$(BIN_DBG): $(BUILD_DIR) $(OBJS_COMMON) $(OBJS_TRAY) $(OBJS_MAIN)
	$(CC) $(CFLAGS) -o $@ $(OBJS_COMMON) $(OBJS_TRAY) $(OBJS_MAIN) $(LDFLAGS_DAEMON)

$(XCLK_DBG): $(BUILD_DIR) $(OBJS_COMMON_DBG) $(OBJS_CLOCK_DBG) $(OBJS_STANDALONE_DBG) $(OBJS_TRAY_DBG)
	$(CC) $(CFLAGS) -o $@ $(OBJS_COMMON_DBG) $(OBJS_CLOCK_DBG) $(OBJS_STANDALONE_DBG) $(OBJS_TRAY_DBG) $(LDFLAGS_STANDALONE)

$(BUILD_DIR)/common_dbg.o: src/core/common.c | $(BUILD_DIR)
	$(CC) $(CFLAGS) -c $< -o $@
$(BUILD_DIR)/clock_dbg.o: src/widgets/clock.c | $(BUILD_DIR)
	$(CC) $(CFLAGS) -c $< -o $@
$(BUILD_DIR)/standalone_dbg.o: src/widgets/standalone_clock.c | $(BUILD_DIR)
	$(CC) $(CFLAGS) -c $< -o $@
$(BUILD_DIR)/tray_dbg.o: src/core/tray.c | $(BUILD_DIR)
	$(CC) $(CFLAGS) -c $< -o $@

# sanitize — с AddressSanitizer/UBSan (те же флаги у отладочной цели)
BIN_SAN = $(BUILD_DIR)/xscreenletsd-san
sanitize: CFLAGS += -O1 -fsanitize=address,undefined -fno-omit-frame-pointer
sanitize: all $(BIN_SAN)
$(BIN_SAN): $(BUILD_DIR) $(OBJS_COMMON) $(OBJS_TRAY) $(OBJS_MAIN)
	$(CC) $(CFLAGS) -o $@ $(OBJS_COMMON) $(OBJS_TRAY) $(OBJS_MAIN) $(LDFLAGS_DAEMON) -fsanitize=address,undefined

.PHONY: all clean run install debug sanitize