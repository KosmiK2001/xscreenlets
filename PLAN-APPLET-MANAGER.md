# Applet Management — план (чекпоинт)

## Задача (от пользователя)
Сделать демон-трей ближе к оригиналу (python screenlets):
- УБРАТЬ из меню трея пункты «Launch Applet» и «Running Instances»
  (функционал в коде СОХРАНИТЬ).
- Добавить один пункт меню: **Applet management**.
- По клику — окно с ТРЕМЯ вкладками:

1. **Существующие апплеты (значки)** — как оригинальный «Менеджер апплетов»:
   значки всех загруженных типов (.so из plugdir; оригинальные иконки
   python-версии: /usr/share/screenlets/<Type>/icon.svg|png, fallback
   /usr/share/icons/screenlets.svg). Кнопки/действия: Запустить,
   Установить (создать инстанс), Сбросить настройки апплета (удалить
   конфиг инстанса? — в оригинале reset default settings).
2. **Запущенные апплеты (дерево вложенности)** — TreeView с деревом:
   корень = инстансы, запущенные демоном (main_daemon), дети = гости
   (guests_N), вложенность полная (frame внутри frame и т.д.).
3. **Конфиги** — список всех файлов .plugins/*.conf: имя, тип,
   started_by (main_daemon/plugin), автозапуск (есть ли symlink в
   plugins_on — переключатель, ТОЛЬКО для корневых), кнопки: удалить
   конфиг, редактировать вручную (gtk plasma? — внешний редактор
   $EDITOR или gtk_dialog с GtkTextView).

## Текущая структура меню трея (tray.c rebuild):
Launch Applet | Running Instances | Restart Applets | Stop all Applets | About | Quit
→ станет: **Applet management** | Restart Applets | Stop all Applets | About | Quit

## Ключевые API, которые уже есть (common.h):
- xs_core_list_plugin_types() — все загруженные типы
- xs_core_list_running_daemon_instances(&count)
- xs_core_plugin_conf(name) / conf_flush
- xs_core_add_instance(type) / xs_core_delete_instance_full(p, del_conf)
- g_plugin_conf_dir = ~/.config/xscreenlets/.plugins
- g_plugin_onoff_dir = ~/.config/xscreenlets/plugins_on (symlink'и)
- guests_N в конфиге frame_launcher-*.conf — список гостей
- started_by: main_daemon | plugin (гость)
- «Автозапуск корневого» = наличие symlink plugins_on/<name>.conf

## Иконки типов:
- launcher: /usr/share/screenlets/Launcher/icon.svg
- clock:    /usr/share/screenlets/Clock/icon.svg (или clock.svg)
- calendar: /usr/share/screenlets/Calendar/icon.svg
- frame_launcher: свой svg в проекте (themes) или screenlets.svg

## Статус
- [x] План создан
- [ ] Модуль src/core/applet_manager.c + окно с 3 вкладками
- [ ] Пункт меню Applet management в tray.c; старые пункты убрать
- [ ] Вкладка 1: сетка значков + Запустить/Сброс настроек
- [ ] Вкладка 2: дерево запущенных (вложенность)
- [ ] Вкладка 3: конфиги + автозапуск + удаление + ручная правка
- [ ] Сборка, установка, проверка с юзером
