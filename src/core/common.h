#ifndef XS_COMMON_H
#define XS_COMMON_H

#include <gtk/gtk.h>
#include <cairo.h>
#include <librsvg/rsvg.h>
#include <stdarg.h>
#include "xs_api.h"

/* Оформление темы (SVG) */
typedef struct {
	char       *dir;
	GHashTable *svgs;          /* элемент -> RsvgHandle* */
} XsTheme;

/* Загруженный модуль плагина (заполняет main.c; нужен common.c для
 * запуска гостя типа без живых инстансов) */
typedef struct {
    GModule *mod;
    XsPluginDesc *desc;
} XsLoadedPlugin;

/* main.c регистрирует массив модулей после load_plugin_modules() */
void xs_core_set_loaded_modules_ref(GPtrArray **ref);

/* Состояние окна плагина */
typedef struct _XsWinState {
	GtkWidget *win;            /* корневое окно */
	GtkWidget *area;           /* drawing area внутри win */
	int x, y, w, h;            /* геометрия */
	XsPlugin *plug;            /* обратная ссылка на плагин */
	guint tick_id;             /* ID таймера g_timeout */
	guint tick_ms;             /* текущий интервал таймера (мс) */
	guint shape_idle_id;       /* ID idle для обновления input shape */
	guint shape_retry_id;      /* ID retry таймера для input shape */
	gint64 last_shape_us;      /* время последнего обновления формы (мкс) */
	gint64 last_conf_save_us;  /* время последнего сохранения позиции в конфиг (мкс) */
	guint tick_count;          /* число срабатываний таймера */
	gint64 last_tick_log_us;   /* время последней сводки tick */
	XsTheme *theme;            /* текущая тема плагина */
	cairo_surface_t *frame;    /* последняя отрисованная поверхность (для shape) */
	gboolean shape_pending;    /* ожидает обновление формы */
	gboolean frame_dirty;      /* frame изменился с последнего capture_frame */
	double opacity;            /* прозрачность через рендер (1.0 = непрозрачно) */
	gboolean locked;           /* позиция зафиксирована (Lock) */
	gboolean sticky;           /* окно на всех рабочих столах */
	gboolean widget;           /* свойство _COMPIZ_WIDGET (compiz widget-плагин) */
	gboolean keep_above;       /* всегда сверху */
	gboolean keep_below;       /* всегда снизу */
	gboolean freed;            /* ресурсы уже освобождены */
} XsWinState;

/* Отложенная команда меню (выполняется в idle после закрытия меню) */
typedef struct {
    XsPlugin *plug;
    char     *cmd;
} XsCmd;

/* Диспетчер core-команд меню (используется и для idle-исполнения) */
void xs_core_dispatch_cmd(XsPlugin *p, const char *cmd);
/* Хелперы меню для плагинов: activate-коллбек пунктов с data "xs-cmd" и разделитель */
void xs_core_menu_activate(GtkMenuItem *item, gpointer data);
GtkWidget *xs_core_add_separator(GtkWidget *menu);

gboolean xs_core_theme_has(XsPlugin *p, const char *element);

/* Хелперы Properties-диалога (стиль get_widget_for_option оригинала):
 * GtkBox-строка «метка 180px + виджет», tooltip = desc; сигналы подключает
 * плагин. Все возвращают сам виджет ввода. */GtkWidget *xs_prop_add_bool(GtkBox *box, const char *label, const char *desc,
                            gboolean value);
GtkWidget *xs_prop_add_string(GtkBox *box, const char *label, const char *desc,
                              const char *value);
GtkWidget *xs_prop_add_choices(GtkBox *box, const char *label, const char *desc,
                               const char *const *choices, const char *value);
GtkWidget *xs_prop_add_int(GtkBox *box, const char *label, const char *desc,
                           double value, double min, double max, double step);
GtkWidget *xs_prop_add_float(GtkBox *box, const char *label, const char *desc,
                             double value, double min, double max,
                             double step, int digits);
GtkWidget *xs_prop_add_color(GtkBox *box, const char *label, const char *desc,
                             double r, double g, double b, double a);
GtkWidget *xs_prop_add_font(GtkBox *box, const char *label, const char *desc,
                            const char *value);
GtkWidget *xs_prop_add_time(GtkBox *box, const char *label, const char *desc,
                            int h, int m, int s);
/* Заголовок группы опций: описание + разделитель (как show_options_for_object) */
void xs_prop_add_group_header(GtkBox *box, const char *info);
/* Добавить готовый виджет в строку «метка 180px + виджет» (для нестандартных) */
GtkWidget *xs_prop_add_row(GtkBox *box, const char *label, const char *desc,
                           GtkWidget *input);

/* Ядро: инициализация и shutdown */
void xs_core_init(const char *conf_path);
void xs_core_shutdown_all(void);
GKeyFile *xs_core_conf(void);
const char *xs_core_conf_path(void);
void xs_core_conf_flush(void);
/* Плагин-конфиги: ~/.config/xscreenlets/plugins/<name>.conf (секция = имя плагина) */
GKeyFile *xs_core_plugin_conf(const char *name);
void xs_core_plugin_conf_flush(const char *name);
/* Каталог включённых конфигов (plugins_on); NULL до xs_core_init(). */
const char *xs_core_onoff_dir(void);
/* Каталог реальных конфигов (~/.config/xscreenlets/.plugins) */
const char *xs_core_plugins_dir(void);
const char *xs_core_plugdir(void);

/* Каталог системных тем: -DXS_THEME_DIR при сборке или --themedir.
 * NULL, если не задан. Задаётся демоном через xs_core_set_themedir();
 * переменная живёт в common.c, поскольку common.o линкуется и в xclock,
 * где main.c нет. */
void xs_core_set_themedir(const char *dir);
const char *xs_core_themedir(void);

/* Найти каталог ТЕМЫ апплета с приоритетом пользователь -> система.
 *
 * Порядок поиска:
 *   1. $XDG_CONFIG_HOME/xscreenlets/themes/<plugin>/<theme>   (пользователь)
 *   2. $HOME/.xscreenlets/themes/<plugin>/<theme>             (legacy, старые
 *      установки screenlets держали темы здесь)
 *   3. <XS_THEME_DIR>/<plugin>/<theme>                       (система)
 *
 * Пользовательские темы идут первыми намеренно: установленный пакет
 * не должен перекрывать настройку конкретного пользователя.
 *
 * Возвращает новый путь (владелец вызывающего, g_free) либо NULL,
 * если нигде нет. Каталог существование НЕ проверяет - вызывающий
 * передаёт результат в theme_load(), который и вернёт ошибку. */
char *xs_core_find_theme(const char *plugin, const char *theme);
/* найти живой инстанс по имени (NULL если не запущен) */
char **xs_core_list_running_daemon_instances(int *count);
XsPlugin *xs_core_find_instance(const char *name);
/* выкинуть кэш конфига инстанса (после ручной правки файла) */
void xs_core_drop_conf_cache(const char *name);
/* окно Applet management (applet_manager.c) */
void xs_applet_manager_show(void);
/* Диалог мёртвого symlink'а (определён в common.c): 1=удалить,
 * 0=выбран другой конфиг (имя в *new_name), -1=отмена. */
int xs_dead_link_dialog(GtkWindow *parent, const char *linkname,
                        char **new_name);

/* Регистрация плагинов */
void xs_core_register_plugin(XsPlugin *p);
void xs_core_unregister_plugin(XsPlugin *p);
void xs_core_shutdown_plugin(XsPlugin *p);
void xs_core_reload(void);
void xs_core_show_plugin(XsPlugin *p, gboolean visible);
gsize xs_core_plugin_count(void);
XsPlugin *xs_core_plugin_at(gsize index);

/* Мультиинстанс: тип плагина инстанса ("clock" для name "clock-2") */
const char *xs_core_plugin_type(XsPlugin *p);
char **xs_core_list_plugin_types(void);
/* Найти свободное имя инстанса: "name", "name-2", "name-3", ... */
char *xs_core_next_instance_name(const char *type);
/* Создать новый инстанс типа type (загрузка .so не требуется — модуль уже
 * в памяти); наследует theme/scale/opacity из секции type, если у нового
 * инстанса ещё нет своих. Возвращает 0 при успехе. */
int xs_core_add_instance(const char *type);
/* Удалить инстанс: shutdown + очистка (секция конфига сохраняется как есть). */
void xs_core_delete_instance_full(XsPlugin *p, gboolean delete_conf);
void xs_core_remove_guest_entry(const char *name);
#define xs_core_delete_instance(p) xs_core_delete_instance_full(p, TRUE)
/* Записать текущий набор инстансов в [instances] главного конфига
 * (реализация в main.c). */
void xs_core_save_instances(void);

/* API для плагинов */
XsHostApi *xs_host_api(void);

/* Лог */
void xs_log_impl(const char *fmt, ...) G_GNUC_PRINTF(1, 2);
void xs_log_implv(const char *fmt, va_list ap);
void xs_core_set_debug(gboolean debug);
gboolean xs_core_is_debug(void);

/* Внутренние */
void xs_core_cleanup_plugin_window(XsPlugin *p);
void xs_core_free_plugin(XsPlugin *p);
gboolean xs_core_recreate_plugin(XsPlugin *p);

#endif /* XS_COMMON_H */
