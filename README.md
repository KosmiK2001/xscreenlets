# Xscreenlets

Xscreenlets — замена устаревших screenlets (python2+PyGTK) для Gentoo-машины.
Демон + плагины в виде .so через gmodule. Плагины никогда не выгружаются.

## Требования

- GCC с поддержкой C11
- GTK+ 3.0
- librsvg-2.0
- gmodule-2.0
- glib-2.0
- gio-2.0
- libsoup-3.0
- libxml-2.0

Сборка производится с флагами `-O2 -Wall -Wextra` без предупреждений.

## Сборка

Сборка производится с флагами `-O2 -g3 -Wall -Wextra`; при добавлении новых
модулей сначала проверяйте их компиляцию отдельно. В текущем дереве старые
предупреждения уже существующих модулей видны в полном `make clean && make`.

```bash
make
```

Создаются артефакты в каталоге `build/`:
- `build/xscreenletsd` — демон;
- `build/clock.so` — плагин часов;
- `build/calendar.so` — плагин календаря;
- `build/launcher.so` — плагин запуска приложений;
- `build/frame_launcher.so` — плагин рамки с гостями;
- `build/clearrss.so` — C/GTK3-порт ClearRss;
- `build/cpu_monitor.so` — монитор нагрузки, частоты и температуры по сокетам CPU;
- `build/memory_monitor.so` — монитор RAM и swap с независимыми длинными историями;
- `build/xclock` — standalone версия часов.

`make clean && make` завершает сборку без ошибок. Предупреждения старых
модулей не относятся к новому `clearrss.c`; его объект собирается без новых
диагностик.

Сборка без предупреждений:
```bash
make clean && make
```

## Установка

```bash
make install
```

По умолчанию устанавливает в `~/bin/` и `~/lib/xscreenlets/plugins/`:
- `~/bin/xscreenletsd`;
- все плагины `.so`, включая `clearrss.so`;
- иконку `~/lib/xscreenlets/icons/clearrss.svg`;
- темы `default` и `Simple` в `~/lib/xscreenlets/themes/clearrss/`.

Поддерживаются переменные `PREFIX` и `DESTDIR`:
```bash
make install PREFIX=/usr/local
make install DESTDIR=/tmp/xscreenlets PREFIX=/usr
```

## Запуск

### Демон
```bash
~/bin/xscreenletsd
```
Или после установки:
```bash
xscreenletsd
```

Или явно с каталогом плагинов:

```bash
DISPLAY=:0 ~/bin/xscreenletsd --plugdir ~/lib/xscreenlets/plugins --debug
```

Демон читает конфигурацию текущей схемы из
`~/.config/xscreenlets/.plugins/`, а автозапуск — из symlink'ов в
`~/.config/xscreenlets/plugins_on/`. Плагин `clearrss` сам не создаёт и не
изменяет symlink'и автозапуска.

### ClearRss

C/GTK3-порт оригинального `/usr/share/screenlets/ClearRss/ClearRssScreenlet.py`:

- RSS и Atom через `libsoup-3.0` и `libxml-2.0`;
- асинхронная сеть с ограниченным чтением ответа и worker-разбор XML без блокировки GTK;
- Properties для `feed_name`, `feed_url`, `update_interval`, шрифта, цветов,
  `window_width`, `window_height`, `news_count` и `auto_news_count`;
- `auto_news_count=true` вычисляет число видимых новостей по высоте окна,
  размеру шрифта, ширине и длине заголовка/описания;
- `news_count` задаёт ручной максимум и сохраняется при переключении
  обратно из автоматического режима;
- Refresh, Previous item и Next item;
- нижние кнопки прокрутки и прокрутка колесом;
- `View this News` открывает выбранную запись через `xdg-open` без shell-инъекции;
- темы `default` и `Simple` с локальными SVG-ассетами.

Имя типа — `clearrss`. Конфиг инстанса создаётся демоном и хранится как
`~/.config/xscreenlets/.plugins/clearrss-UUID8-метка.conf`; секция конфига
равна имени инстанса. Для гостя `frame_launcher` добавляется только ключ
`started_by=plugin`; плагин не редактирует `plugins_on` и список гостей.

Подробный план переноса: `PLAN-CLEARRSS.md`.

### CPU monitor

Тип плагина — `cpu_monitor`. Каждый инстанс отвечает за один
`physical_package_id` из sysfs. Физические ядра группируются по
`thread_siblings_list`; первый логический поток показывается сверху, второй
снизу, между ними отображаются частота физического ядра и его температура.
Оба блока потока показывают историю нагрузки цветными вертикальными
столбцами: `system`, `user`, `nice`, `iowait`; один столбец добавляется на
каждый тик обновления, старые столбцы сдвигаются влево.

- нагрузка читается напрямую из `/proc/stat`;
- частота — максимум `scaling_cur_freq` потоков ядра;
- температура — `coretemp` по меткам `Package id N` и `Core M`;
- по умолчанию сетка имеет 4 колонки и 2 строки, размер всего окна — 200×152 px;
- `Window width` и `Window height` в Properties задают общий размер окна, а блоки
  ядер и число столбцов истории автоматически занимают всю доступную область
  с 1 px внутренним отступом;
- `socket_id` в Properties выбирает сокет (`0`, `1`, ...);
- два живых инстанса не могут одновременно занять один сокет.

### Memory monitor

Тип плагина — `memory_monitor`. Данные читаются напрямую из `/proc/meminfo`
раз в `update_ms` (по умолчанию 1000 мс), а `draw()` только отображает
кэшированную поверхность и не выполняет I/O. RAM-занятость вычисляется как
`MemTotal - MemAvailable`, swap — как `SwapTotal - SwapFree`; при нулевом
`SwapTotal` показываются `0%`, `0 KiB` и `0 KiB` с безопасным нулём в истории.

Виджет содержит длинные вертикальные истории. RAM-график является стеком из
четырёх независимо сэмплируемых, непересекающихся компонентов `/proc/meminfo`:
`Пользователь/приложения = MemTotal - MemFree - Buffers - Cached`,
`Разделяемая = Shmem`, `Буфера = Buffers`, `Кеш = Cached - Shmem`; доли делятся
на `MemTotal`. Процент RAM по-прежнему показывает используемую память
`MemTotal - MemAvailable`. У каждого сегмента свой RGBA-ключ в Properties:
`ram_color`, `shared_color`, `buffers_color`, `cache_color`. Swap сохраняет
отдельную историю. Каждый новый столбец появляется справа, а предыдущие точки
истории уходят влево. Ширина
обоих графиков автоматически вычисляется из ширины окна, реально измеренного
блока текста, боковых отступов и зазора; в Properties ширина графика отдельно
не задаётся. Высота обоих графиков также вычисляется из `window_height`, поэтому
изменение высоты окна масштабирует RAM и Swap синхронно. Окно по умолчанию
`320×344`; формула высоты графика — `(window_height - 64) / 2`, то есть `140` px
при `344` и `128` px при `320`. Окно можно уменьшать до `100×100` px. При
высоте меньше `200` px включается отдельный row-layout: каждый раздел занимает
две строки (`Ram:/Swap: + процент`, затем граф слева, а `Now` и `Overall` на
отдельных строках справа с коротким stippled HR между ними). Проценты RAM и
Swap всегда выровнены по правому краю applet (`PANGO_ALIGN_RIGHT`). В compact
режиме график занимает всю оставшуюся ширину непосредственно до колонки
`Now/Overall`, а общий `striped_hr` между разделами — всю внутреннюю ширину от
левого до правого 4 px поля. Внутренние HR остаются ограниченными фактической
шириной `Now/Overall`. Высота row-графика равна
`(window_height - 45) / 2`; HR располагается на 3 px ниже рамки RAM, поэтому
элементы не соприкасаются. Правый край подписей сохраняет 4 px внутреннего поля.
Отдельная RGBA-настройка `Graph background` в Properties управляет внутренним
фоном обоих графиков; её ключ `graph_background_color` независим от фона/прозрачности
всего applet. Цвет применяется сразу и сохраняется в config.
`make test` проверяет MemAvailable, SwapTotal/SwapFree, нулевой swap,
responsive-расчёт ширины, высоты и компактной геометрии, а также ограничение
внутреннего HR шириной фактического Pango-текста `Now/Overall` на
детерминированных входных данных.

## Process list

Тип плагина — `process_list`. Он читает числовые данные напрямую из
`/proc/<pid>/stat` и `/proc/<pid>/status`, сортирует
процессы по убыванию CPU и рисует до 8 строк `NAME PID CPU MEM I/O`. CPU —
дельта `utime + stime`, MEM — фактический RSS из `VmRSS`; I/O — суммарная
скорость дискового чтения и записи
(`read_bytes + write_bytes`) за интервал обновления. Заголовок, шрифты и
обновление раз в секунду следуют эталону `.conkyrc_proc`; окно по умолчанию
320×164 px: колонка Name выравнивается влево, PID/CPU/MEM/I/O — вправо,
а PID рассчитан на многомиллионные значения. CPU имеет два взаимоисключающих режима: `Per core` (`100%` — один
логический поток) или `Conky (${top cpu})` (`100%` — все онлайн-потоки);
значения отображаются с явным знаком `%`, колонка рассчитана на `100% × n_cpu`.
MEM показывает фактический RSS процесса в `B`, `K`, `M`, `G`, `T`, `P` или `E`.
Активный столбец сортировки обведён зелёной рамкой `1 px`; текст в
заголовках и строках имеет внутренний отступ `2 px` слева и справа. Клик по
`NAME`, `PID`, `CPU`, `MEM` или
`I/O` сортирует полный текущий снимок процессов: первый клик задаёт обычное
направление для текстовых/числовых столбцов, второй — обратное, третий
возвращает выбранные в Properties `Default sort` и `Default direction`
(по умолчанию `CPU`, descending).
Background — полноценный GtkColorButton RGBA (`use_alpha=TRUE`); в конфиг
сохраняется нормализованная строка `r,g,b,a`, а выбранная alpha применяется
к `cairo_set_source_rgba` при каждом перерисовывании. `draw()` не читает `/proc`.

```bash
~/bin/xclock
```
Или после установки:
```bash
xclock
```

Standalone версия создаёт собственный экземпляр плагина часов и работает без демона (полезно для отладки). Она собирается в `build/xclock`, но целью `install` не устанавливается.

## Конфигурация

Конфигурационный файл: `~/.config/xscreenlets/xscreenlets.conf` (формат GKeyFile).

### Секция `[global]`

Секция `[global]` задаёт поведение окон всех плагинов на X11/Compiz:

- `type` — тип окна, один из `desktop`, `normal`, `dock` (регистронезависимо; по умолчанию `dock`).
- `keep_below` — булево значение, устанавливает `_NET_WM_STATE_BELOW` (по умолчанию `true`).
- `sticky` — булево значение: окно на всех виртуальных рабочих столах
  (`_NET_WM_STATE_STICKY`; по умолчанию `true`). Перекрывается ключом `sticky`
  в конфиге плагина.

Пример:
```
[global]
type = normal
keep_below = true
```

**Рабочий вариант на X11/Compiz:** `type=normal`, `keep_below=true` — окно отображается, попадает в `_NET_CLIENT_LIST`, прозрачные края и input shape работают, секундная стрелка тикает. `type=desktop` с `keep_below=true` также поддерживается и отрисовывает кадр; для видимого окна на Compiz предпочтителен `normal`.

### Перемещение окон

Окна виджетов двигаются **оконным менеджером**, а не кодом демона:
- **Alt+Left_click** в Compiz/MATE перетаскивает окно, как у старых python2-screenlets.
- Самодельный drag (`drag_on`/`drag_dx`/`drag_dy`) в `common.c` **удалён** — `on_button`/`on_motion` проксируют только в плагин (`ops->button`/`ops->motion`); если плагин не обработал событие, возвращается `FALSE`.
- Позиция окна отслеживается через обработчик `configure-event`: при каждом перемещении WM вызывается `gtk_window_get_position`, `state->x`/`state->y` обновляются, а конфиг `[имя плагина] x=,y=` сохраняется с дебаунсом не чаще 1 раза в 2 секунды (поле `last_conf_save_us`).
- При `xs_core_shutdown_plugin` / `xs_core_reload` позиция сохраняется финально перед уничтожением окна.
- После перезапуска демона окно открывается на последней сохранённой позиции.

### Меню виджета (правый клик) — как в оригинальном python-screenlets

Структура повторяет `add_default_menuitems` оригинала (ClockScreenlet, STANDARD):

```
Get Clock Skins            ← пункт плагина Clock (xdg-open gnome-look.org)
──────────────
Theme >                    ← подменю тем (галка у текущей)
Size >                     ← 16 значений, как в оригинале: 20…1000 %
Window >
  Lock        (чекбокс; при заблокированной позиции configure-event игнорируется)
  Sticky      (чекбокс; окно на всех рабочих столах)
  Widget      (чекбокс; свойство _COMPIZ_WIDGET — compiz widget-плагин)
  Keep above  (чекбокс; взаимоисключимо с Keep below)
  Keep below  (чекбокс)
──────────────
Properties...              ← диалог настроек
Info...                    ← GtkAboutDialog
──────────────
Quit
```

- **Size** — `scale:V` (диапазон оригинала 0.2–10.0) → конфиг + **живой ресайз
  окна** (`host->resize`, как update_shape в оригинале): окно не пересоздаётся,
  открытый Properties-диалог и стек окон WM не затрагиваются.
- **Window** — команды `win:lock|sticky|widget|above|below:0|1`; применяются
  живо (stick / keep-above / keep-below / свойство `_COMPIZ_WIDGET`) и
  сохраняются в конфиг плагина (`lock`, `sticky`, `widget`, `keep_above`,
  `keep_below`), при recreate восстанавливаются.
- **Properties...** — диалог в стиле оригинального OptionsDialog (490×450,
  keep-above, только Close), с вкладками:
  - **About** — иконка (`/usr/share/screenlets/<Name>/icon.svg|png`, fallback
    gtk-properties), имя, версия, описание и автор из метаданных плагина
    (`XsPluginDesc.desc/author/version`, ABI — в конце структуры).
  - **Options** — вложенный блокнот: страница «Window» (Scale spin 0.2–10,
    Opacity шкала 0.1–1.0, 5 чекбоксов флагов окна — всё мгновенно) +
    страницы-группы плагина через хук `ops->properties(p, GtkNotebook *)`.
    У Clock — три группы как в оригинале, все опции функциональны:
    - **Clock**: Time Zone (Entry), Time-Offset (Float -12..12, шаг 0.5),
      Hour-Format (ComboBox 12/24), Show seconds-hand (чекбокс).
    - **Alarm**: Activate Alarm, Alarm-Time (3 SpinButton h/m/s),
      Alarm stops after (Int 0..5000), Run a command + Alarm command
      (мигание alarm_length/2 сек, однократно в сутки, spawn команды).
    - **Face**: Face-Text (Pango-разметка на циферблате, центрируется;
      несколько строк центрируются независимо), Text-Font (FontButton),
      Text-Color (ColorButton RGBA; по умолчанию чёрный непрозрачный),
      X/Y-Position (Int 0..100), Show today's date + Date Format (strftime;
      по умолчанию `%Y.%m.%d`).
    Все строки — в формате оригинала: метка 180 px слева + виджет,
    tooltip = описание опции; изменения применяются в реальном времени.
    Старые ключи конфига h24/show_seconds мигрируются автоматически.
  - **Themes** — скроллируемый список тем (хук `ops->fill_themes`, 4 колонки);
    строка = name (ultrabold large) « v<version>», info (small), «by author» —
    всё из theme.conf [Theme]; нет данных → «(no info available)». Текущая
    тема (из конфига) выделена при открытии вкладки; переключение — только
    реальным кликом по другой строке. Клик по
    теме меняет её **на месте** (без recreate, диалог не закрывается).
- **Info...** — GtkAboutDialog с именем плагина.
- **Reload** в меню апплета больше нет (как в оригинале) — он остался в трее.
- Пункты оригинала «Add one more / Delete this / Quit this» требуют
  мультиинстансов — пока не реализованы (ядро держит одно окно на плагин).
- Прозрачность настраивается через **Properties** (шкала Opacity).

**Реализация**:
- Меню создаётся в `xs_core_popup_menu()` в `src/core/common.c` при каждом правом клике; после закрытия (`selection-done`) уничтожается через idle — меню не накапливаются.
- Порядок: пункты плагина (`ops->menu`) → Size → Window → Properties/Info → Quit; плагинские пункты обрабатываются в `p->ops->menu_cmd()`.
- Команда `theme:` делает recreate плагина; `scale:` — живой ресайз
  (`host->resize`); `op:` остался для совместимости (recreate).
- Команда `win:<flag>:<0|1>` применяет флаг окна живо и сохраняет в конфиг.
- **Инвариант**: каждый GtkWidget добавляется ровно в один menu shell. Если один и тот же виджет добавить в подменю и в корень меню, GTK выдаёт «Can't set a parent on widget which has a parent», а `gtk_widget_show_all()` уходит в бесконечную взаимную рекурсию (67k кадров) и SIGSEGV — это был реальный краш правого клика.

### Секция `[clock]` плагина часов:
- `theme` — имя каталога с темой (по умолчанию: `cairo-clock`)
- `scale` — масштаб окна (по умолчанию: `1.0`, диапазон: `0.2–10.0`)
- `h24` — 24-часовой формат (по умолчанию: `true`)
- `x`, `y` — начальная позиция окна (по умолчанию: `80`, `80`)
- `opacity` — прозрачность окна (по умолчанию: `1.0`, диапазон: `0.1–1.0`)
- `show_seconds` — показывать секундную стрелку (по умолчанию: `true`)

Пример:
```
[clock]
theme = glass
scale = 1.4
opacity = 0.75
h24 = false
show_seconds = false
x = 100
y = 100
```

Темы ищутся в следующем порядке:
1. `~/.config/xscreenlets/themes/clock/<тема>/`
2. `/usr/share/screenlets/Clock/themes/<тема>/`

Доступные темы: cairo-clock, glass, ryx-glass, station, tango, time4linux, time4linux-inverse (каталоги `/usr/share/screenlets/Clock/themes`), плюс пользовательские в `~/.config/xscreenlets/themes/clock/<name>/`.

---

## Настройка

Конфигурация разделена на два файла:

### 1. `~/.config/xscreenlets/xscreenletsd.conf` — демон
```ini
[global]
type = normal        # тип окна: normal | desktop | dock (по умолчанию dock)
keep_below = true    # _NET_WM_STATE_BELOW
```

### 2. `~/.config/xscreenlets/plugins/<имя>.conf` — плагины
Файл на каждый плагин (`clock.conf`, `calendar.conf`, ...), секция = имя плагина.
Для `[clock]`:
- `theme` — имя каталога темы (cairo-clock, glass, ryx-glass, station, tango,
  time4linux, time4linux-inverse; свои темы — ~/.config/xscreenlets/themes/clock/<name>/)
- `scale` — масштаб окна (0.2–10.0); размер окна = (int)(240*scale)
- `opacity` — прозрачность (0.1–1.0); реализована **рендером (cairo paint_with_alpha)**:
  окно остаётся непрозрачным для WM — Alt+drag работает при любой прозрачности
- `h24` — 24-часовой формат (true/false)
- `show_seconds` — секундная стрелка (true/false)
- `x`, `y` — позиция (обновляется автоматически при Alt+drag с дебаунсом 2 с)

### Применение изменений
После правки любого конфига: `killall -HUP xscreenletsd` или **Reload** в трее.
Миграция старого `xscreenlets.conf` выполнена один раз; старый файл — `xscreenlets.conf.migrated`.

Пример конфига:
```
[global]
type = normal
keep_below = true

[clock]
theme = cairo-clock
scale = 1.0
opacity = 1.0
h24 = true
show_seconds = true
x = 80
y = 38
```

## API плагина

Плагин — это разделяемая библиотека (`.so`), экспортирующая функцию `xs_plugin_desc()` через gmodule.

Основные структуры (см. `include/xs_api.h`):
- `XsPlugin` — экземпляр виджета
- `XsPluginOps` — операции плагина (`init`, `draw`, `tick`, `button`, `motion`, `shutdown`)
- `XsHostApi` — API ядра, доступный плагину (`make_window`, `invalidate`, `set_tick`, конфиг-хелперы, работа с темами, лог)

Плагин НЕ должен выгружаться (.so остаётся загруженным до конца процесса).
Все коллбеки выполняются в главном цикле GTK (потоки можно использовать только для блокирующего I/O, а UI-обновления — через `g_idle_add`).

### Обязанности плагина
1. В `init` прочитать конфиг из переданного `GKeyFile` (секция совпадает с `desc.name`).
2. Создать окно через `host->make_window()`.
3. Установить интервал таймера через `host->set_tick()`.
4. В `draw` отрисовать содержимое на переданном `cairo_t` (фон уже очищен ядром).
5. В `tick` вернуть новый интервал в миллисекундах (0 — оставить текущий).
6. Обрабатывать события мыши через `button`/`motion` (возвращать `TRUE`, если событие обработано).
7. В `shutdown` уничтожить окна и освободить приватные данные.

### Доступные функции ядра (через `xs_host_api()`)
- `GtkWidget *make_window(XsPlugin *p, int x, int y, int w, int h)` — создать окно виджету
- `void invalidate(XsPlugin *p)` — запросить пересчёт input shape
- `void set_tick(XsPlugin *p, guint ms)` — установить интервал таймера
- Конфиг-хелперы: `conf_int`, `conf_dbl`, `conf_str`, `conf_set_int`, `conf_set_str`
- Тема: `theme_load` (загрузить каталог SVG), `theme_draw` (нарисовать элемент темы)
- `void log(const char *fmt, ...)` — лог с префиксом `[xscreenletsd]`
- `void resize(XsPlugin *p, int w, int h)` — живой ресайз окна (без recreate)
- `void recreate(XsPlugin *p)` — пересоздать плагин (theme/op)

## Плагин calendar (замена ClearCalendar v0.4)

Второй плагин, загружается демоном автоматически из `~/lib/xscreenlets/plugins/calendar.so`.

### Внешний вид
- Базовый размер 204×210 (масштабируется через scale 0.2–10, живой ресайз).
- Сетка месяца: заголовок «Месяц Год» (справа), строка дней недели (первые
  3 буквы локализованных имён, ru_RU), сетка 7 колонок × 6 строк, подсветка
  today красным, дни с событиями — зелёным.
- **Листание месяцев: колесо мыши** над окном (вверх — следующий, вниз —
  предыдущий), средняя кнопка — возврат к текущему месяцу.
- Темы: `date-bg.svg` из каталога темы (Dark/Simple/Noback в
  `/usr/share/screenlets/ClearCalendar/themes`, свои в
  `~/.config/xscreenlets/themes/calendar/`); без темы — полупрозрачный
  rounded-rect.

### События (iCalendar)
- Источник — локальный .ics (ключ `icalpath`; дефолт —
  `/usr/share/screenlets/ClearCalendar/calendar.ics`). URL (http) не
  поддерживается — пишется в лог.
- Парсятся VEVENT: SUMMARY, DTSTART (дата = первые 8 цифр), RRULE только
  FREQ=YEARLY (годовое повторение — «@»-пометка). EXDATE/сложные RRULE — нет.
- Меню плагина: **View events** (список событий видимого месяца),
  **Update events** (перечитать ics), **Toggle events** (показ/скрытие),
  **Back to today**, Theme-подменю.

### Конфиг `~/.config/xscreenlets/plugins/calendar.conf` ([calendar])
`first_weekday` (имя дня в левой колонке; дефолт — понедельник),
`icalpath`, `showevents`, `theme`, `scale`, `x`, `y`,
`font_color`/`today_color`/`event_color`/`today_event_color`/
`background_color` (формат «r,g,b,a»).

## Ограничения

- GTK+ 3.0 только (без поддержки GTK+ 4 или Wayland)
- Плагины никогда не выгружаются (`dlclose` не используется)
- Рисование выполняется только в главном цикле GTK
- Нет многопоточного доступа к UI из плагинов
- Ввод формы окна формируется из непрозрачных частей кадра (альфа > 8)

## Отладка

Для запуска демона с подробными сообщениями используйте `--debug`:

```bash
DISPLAY=:0 ./build/xscreenletsd --plugdir ~/lib/xscreenlets/plugins --debug
```

В режиме отладки выводятся параметры окна (`type` и `keep_below`), счётчик отрисовок, таймер тиков и результаты загрузки тем.

Сборка без предупреждений:
```bash
make clean && make
```

### Бенчмарк CPU/RSS

Замер выполнен на `CLK_TCK=100`, окно 60 секунд. CPU рассчитан по `/proc/<pid>/stat` полям 14–15 (`utime + stime`); RSS — по второму полю `/proc/<pid>/statm` (страницы × 4096 байт).

| Процесс | CPU% (60с) | RSS, МБ |
| --- | ---: | ---: |
| python2 screenlets-daemon (PID 8651) | 0.0000 | 45.922 |
| python2 Clock (PID 9175) | 0.1083 | 59.359 |
| python2 ClearCalendar (PID 9176) | 0.1917 | 63.625 |
| python2 Launcher (PID 9177) | 0.0000 | 62.961 |
| **Сумма python2-четвёрки** | **0.3000** | **231.867** |
| **xscreenletsd** (после оптимизации shape) | **0.21** | **41.36** |

Проверка экспорта символа плагина:
```bash
nm -D build/clock.so | grep xs_plugin_desc
```

Проверка зависимостей:
```bash
ldd build/xscreenletsd build/clock.so build/xclock
```

## Лицензия

MIT

## Для ассистентов / саб-агентов (обязательные правила)

- **Единственный рабочий каталог проекта**: `/home/kosmik2001_dir/Документы/GLM-5.3-Kimi/xscreenlets/`
  (перенесён сюда 2026-09-13; старый `/home/kosmik2001_dir/xscreenlets` больше не существует).
  Начинать каждую задачу с `cd` в этот каталог; пути к файлам — абсолютные.
- **НЕ трогать** чужие проекты: всё в `~/Документы/GLM-5.3-Kimi/`, кроме `xscreenlets/`
  (android/, openvpn_WEB/, shaping-view-gtk3/, tc_shaper/ и пр.) — читать как
  референсы можно, писать нельзя.
- Установка результатов — только копирование: `cp build/clock.so ~/lib/xscreenlets/plugins/`,
  `cp build/xscreenletsd ~/bin/`. Файлы `~/lib/xscreenlets/` и `~/bin/` иначе не менять.
- Структура проекта: `include/` (API-контракт), `src/core/` (демон), `src/widgets/` (плагины),
  сборка — `make` (артефакты в `build/`), README.md обновлять при смене интерфейса/конфигов.
- GUI-тесты: `DISPLAY=:0 ./build/xscreenletsd --plugdir ~/lib/xscreenlets/plugins`.
  Убивать только собственные экземпляры демона. Python2-процессы владельца
  (`screenlets-daemon.py`, `*Screenlet.py`) НЕ трогать.
- Конфиги владельца (`~/.config/xscreenlets/xscreenletsd.conf`,
  `~/.config/xscreenlets/plugins/*.conf`) менять только для тестов и возвращать обратно.
- Кросс-проверка: любые изменения ядра прогонять с плагином clock (меню, HUP-reload,
  сохранение позиции) — это наши интеграционные тесты.

### Мультиинстанс

Один плагин (.so) может иметь несколько инстансов. Имя инстанса = секция
конфига в `~/.config/xscreenlets/plugins/<имя>.conf`; тип плагина отделён
от имени инстанса. Список инстансов хранится в секции `[instances]`
главного конфига (`имя=тип`); при отсутствии секции создаётся один
инстанс на загруженный тип (обратная совместимость).

- Правый клик на апплете: **Add one more <тип>** — новый инстанс
  (`launcher-2`, `launcher-3`, …), наследует theme/scale/opacity
  от прототипа; **Delete this <тип>** — удалить инстанс.
- Изменения списка сразу сохраняются в `[instances]`.
- Tray: отдельный чекбокс на каждый инстанс.
