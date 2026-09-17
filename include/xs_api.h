/* xs_api.h — контракт плагина Xscreenlets, v1.
 * Плагин = .so, экспортирует xs_plugin_desc() (gmodule). Выгрузка не предусмотрена:
 * демон живёт с плагином до конца процесса. Все коллбэки выполняются в GTK main loop
 * (потоки — только для блокирующего I/O внутри плагина, UI — через g_idle_add). */
#ifndef XS_API_H
#define XS_API_H

#include <gtk/gtk.h>
#include <gmodule.h>
#include <gdk/gdkx.h> /* XReparentWindow для хостинга гостей */
#include <X11/Xlib.h>

#define XS_API_VERSION 1

typedef struct _XsPlugin    XsPlugin;
typedef struct _XsPluginOps XsPluginOps;
typedef struct _XsHostApi   XsHostApi;

/* Экземпляр виджета, созданный плагином. */
struct _XsPlugin {
	const char *name;          /* имя ИНСТАНСА ("clock", "launcher-2"); = секция конфига */
	const char *type;          /* ТИП плагина ("clock") — путь тем/иконок; NULL = name */
	XsHostApi   *host;
	void       *priv;          /* внутренние данные плагина */
	GtkWidget  *win;           /* корневое окно виджета (создаёт common) */
	const XsPluginOps *ops;
	/* метаданные для About (заполняет ядро из XsPluginDesc при регистрации) */
	const char *desc;
	const char *author;
	const char *version;
};

/* Функции плагина. Окно (priv->win) создаёт КОР плагин через xs_common_*,
 * демон не знает деталей отрисовки. */
struct _XsPluginOps {
	/* создать окно/состояние; конфиг kf секции [name]; вернуть 0 при успехе */
	int  (*init)  (XsPlugin *p, GKeyFile *kf);
	/* перерисовка: cairo на окне priv->win (drawing area) */
	void (*draw)  (XsPlugin *p, cairo_t *cr, int w, int h);
	/* плановый таймер, ms; вернуть новый интервал (0 = не менять) */
	guint (*tick) (XsPlugin *p);
	/* события мыши на окне: return TRUE = обработано (drag уже учтён кором) */
	gboolean (*button)(XsPlugin *p, GdkEventButton *ev);
	gboolean (*motion) (XsPlugin *p, GdkEventMotion *ev);
	/* останов: уничтожить окна, освободить priv (без выгрузки .so) */
	void (*shutdown)(XsPlugin *p);
	/* добавить пункты плагина в контекстное меню (ПЕРЕД core-пунктами);
	 * p->win уже создан, используйте ops->menu(p, menu);
	 * если нет своих пунктов — NULL */
	void (*menu)(XsPlugin *p, GtkMenu *m);
	/* обработка команд из плагина-меню (начинаются с "p:") */
	void (*menu_cmd)(XsPlugin *p, const char *cmd);
	/* добавить свои страницы-группы в Options-вкладку диалога Properties
	 * (GtkNotebook); NULL — без своих страниц */
	void (*properties)(XsPlugin *p, GtkNotebook *nb);
	/* наполнить список тем для вкладки Themes диалога Properties;
	 * колонки: 0=dir-имя, 1=info, 2=author, 3=version (из theme.conf [Theme]);
	 * NULL — вкладка Themes не показывается */
	void (*fill_themes)(XsPlugin *p, GtkListStore *store);
	/* событие колеса мыши на окне: return TRUE = обработано */
	gboolean (*scroll)(XsPlugin *p, GdkEventScroll *ev);
};

typedef struct {
	const char        *name;        /* "clock" */
	unsigned int       api_version; /* == XS_API_VERSION */
	const XsPluginOps *ops;         /* plugin entry points */
	const char        *desc;        /* описание для About-страницы (или NULL) */
	const char        *author;      /* автор для About-страницы (или NULL) */
	const char        *version;     /* версия для About-страницы (или NULL) */
} XsPluginDesc;

/* точка входа gmodule */
typedef XsPluginDesc *(*XsPluginDescFn)(void);

#define XS_PLUGIN_EXPORT(desc) \
	XsPluginDesc *xs_plugin_desc(void) { return desc; }

/* --- host API (core), доступный плагинам через xs_host_* --- */
XsHostApi *xs_host_api(void);

struct _XsHostApi {
	/* Окна: rgba, shape по альфе, drag; геометрия сохраняется в конфиг при drag-end */
	GtkWidget *(*make_window)(XsPlugin *p, int x, int y, int w, int h);
	/* заставить демон пересчитать input shape после изменения кадра */
	void  (*invalidate)(XsPlugin *p);
	/* таймер плагина (один на виджет) */
	void  (*set_tick)(XsPlugin *p, guint ms);
	/* конфиг-хелперы */
	int   (*conf_int)   (GKeyFile *kf, const char *sec, const char *key, int def);
	double (*conf_dbl)  (GKeyFile *kf, const char *sec, const char *key, double def);
	char *(*conf_str)   (GKeyFile *kf, const char *sec, const char *key, const char *def);
	void  (*conf_set_int)(GKeyFile *kf, const char *sec, const char *key, int v);
	void  (*conf_set_str)(GKeyFile *kf, const char *sec, const char *key, const char *v);
	void  (*conf_set_dbl)(GKeyFile *kf, const char *sec, const char *key, double v);
	/* тема: каталог SVG (теперь и PNG) */
	gboolean (*theme_load)(XsPlugin *p, const char *dir);
	void     (*theme_draw)(XsPlugin *p, cairo_t *cr, const char *el,
	                       double x, double y, double width);
	/* лог демона */
	void (*log)(const char *fmt, ...) G_GNUC_PRINTF(1, 2);
	/* уничтожить окно плагина без прямого GTK в плагине */
	void (*cleanup_window)(XsPlugin *p);
	/* set window opacity (0.0–1.0); add new fields ONLY at the end (ABI) */
	void (*set_opacity)(XsPlugin *p, double opacity);
	/* пересоздать плагин (call from plugin menu actions) */
	void (*recreate)(XsPlugin *p);
	/* изменить размер окна плагина живьём (без recreate);
	 * input shape пересчитается автоматически */
	void (*resize)(XsPlugin *p, int w, int h);
	/* как theme_draw, но с отдельной высотой viewport (неквадратные темы) */
	void (*theme_draw_full)(XsPlugin *p, cairo_t *cr, const char *el,
	                            double x, double y, double width,
	                            double height);
	/* рендер элемента темы в его НАТУРАЛЬНОМ размере (svg без ресайза,
	 * png 1:1) — как theme.render в оригинале */
	void (*theme_draw_native)(XsPlugin *p, cairo_t *cr, const char *el,
	                              double x, double y);
	/* --- frame_launcher / хостинг гостей (добавлено в конец, ABI) --- */
	/* запустить гостя из СУЩЕСТВУЮЩЕГО конфига .plugins/<guest_name>.conf;
	 * циклы проверяет демон; возвращает созданный XsPlugin (окно ещё
	 * НЕ встроено: репарентит хост) или NULL */
	XsPlugin *(*start_guest)(XsPlugin *host, const char *guest_name);
	/* остановить гостя (полное удаление инстанса) */
	void (*stop_guest)(XsPlugin *host, const char *guest_name);
};

#endif /* XS_API_H */
