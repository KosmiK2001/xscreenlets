#include <unistd.h> /* symlink, unlink, rmdir */
#include <stdio.h>  /* remove */
#include <errno.h>  /* errno, ENOENT */

#include "common.h"
#include "tray.h"
#include "i18n.h"

#include <cairo.h>
#include <glib.h>
#include <gtk/gtk.h>
#include <librsvg/rsvg.h>
#include <stdarg.h>
#include <stdio.h>
#include <string.h>

static GKeyFile *g_conf;
static char *g_conf_path;
static GPtrArray *g_plugins;
static gboolean g_debug = FALSE;
/* Плагин-конфиги: name -> GKeyFile* (лениво, файл plugins/<name>.conf) */
static GHashTable *g_plugin_confs;
static char *g_plugin_conf_dir;
/* Каталог symlink'ов включённых конфигов (plugins_on; только для демона) */
static char *g_plugin_onoff_dir;

static void xs_short_uuid(char buf[9]);
static void xs_core_migrate_legacy_configs(const char *legacy_dir);

static void xs_theme_free(XsTheme *theme)
{
    if (!theme)
        return;
    g_free(theme->dir);
    g_hash_table_destroy(theme->svgs);
    g_free(theme);
}

void xs_log_implv(const char *fmt, va_list ap)
{
    g_printerr("[xscreenletsd] ");
    vfprintf(stderr, fmt, ap);
    g_printerr("\n");
}

void xs_log_impl(const char *fmt, ...)
{
    va_list ap;

    va_start(ap, fmt);
    xs_log_implv(fmt, ap);
    va_end(ap);
}

static char *xs_config_parent(const char *path)
{
    char *parent;

    if (!path)
        return NULL;
    parent = g_path_get_dirname(path);
    if (parent && g_mkdir_with_parents(parent, 0700) != 0)
        xs_log_impl("cannot create config directory %s", parent);
    return parent;
}

void xs_core_set_debug(gboolean debug)
{
    g_debug = debug;
}

gboolean xs_core_is_debug(void)
{
    return g_debug;
}

void xs_core_init(const char *conf_path)
{
    GError *error = NULL;
    char *parent;

    if (g_conf)
        xs_core_shutdown_all();

    g_free(g_conf_path);
    g_conf_path = g_strdup(conf_path ? conf_path : "");
    g_conf = g_key_file_new();
    parent = xs_config_parent(g_conf_path);
    g_free(parent);

    if (g_conf_path[0] != '\0' &&
        !g_key_file_load_from_file(g_conf, g_conf_path, G_KEY_FILE_NONE,
                                   &error)) {
        xs_log_impl("config %s: new", g_conf_path);
        if (error) {
            xs_log_impl("%s", error->message);
            g_error_free(error);
        }
    }
    if (!g_plugin_conf_dir) {
        g_plugin_conf_dir = g_build_filename(g_path_get_dirname(g_conf_path),
                                             "plugins", NULL);
    }
    /* Схема мультиинстанс-конфигов:
     *  .plugins/   — реальные конфиги всех инстансов (плагины пишут сюда)
     *  plugins/    — совместимость: миграция старых конфигов, потом удаляется
     *  plugins_on/ — symlink'и включённых конфигов (только для демона)
     * Демон запускает только те конфиги, чьи symlink'и лежат в plugins_on. */
    {
        char *dotdir = g_build_filename(g_path_get_dirname(g_conf_path),
                                        ".plugins", NULL);
        char *onoffdir = g_build_filename(g_path_get_dirname(g_conf_path),
                                          "plugins_on", NULL);
        char *legacy = g_build_filename(g_path_get_dirname(g_conf_path),
                                        "plugins", NULL);

        g_free(g_plugin_conf_dir);
        g_plugin_conf_dir = dotdir;   /* реальные конфиги */
        g_plugin_onoff_dir = onoffdir;
        g_mkdir_with_parents(dotdir, 0700);
        g_mkdir_with_parents(onoffdir, 0700);
        /* Миграция: если plugins/*.conf ещё есть (настоящие файлы) —
         * переносим в .plugins, а сам plugins/ оставляем только если это
         * НЕ каталог symlink'ов. */
        xs_core_migrate_legacy_configs(legacy);
        g_free(legacy);
    }
    g_plugins = g_ptr_array_new();
}

/* Миграция старой схемы: plugins/*.conf (реальные файлы) → переименовать
 * в тип-UUID-user_label, секцию внутри привести к имени файла, добавить
 * user_label, перенести в .plugins, создать symlink в plugins_on.
 * Вызывается один раз при старте демона. */
static void xs_core_migrate_legacy_configs(const char *legacy_dir)
{
    GDir *d;
    const char *fn;

    if (!legacy_dir)
        return;
    d = g_dir_open(legacy_dir, 0, NULL);
    if (!d)
        return;
    while ((fn = g_dir_read_name(d)) != NULL) {
        char *path;
        char *base;
        char *newbase, *newpath;
        GKeyFile *kf;
        char *sec = NULL, uuid[9], *lab = NULL, *data;
        gsize len;
        char *dotpath, *linkpath, *target;
        gchar **keys = NULL;
        gsize nkeys = 0;
        GKeyFile *nk;

        if (!g_str_has_suffix(fn, ".conf"))
            continue;
        path = g_build_filename(legacy_dir, fn, NULL);
        if (g_file_test(path, G_FILE_TEST_IS_SYMLINK) ||
            !g_file_test(path, G_FILE_TEST_IS_REGULAR)) {
            g_free(path);
            continue; /* symlink (не наш случай) — не трогаем */
        }
        base = g_strndup(fn, strlen(fn) - 5); /* без .conf */
        kf = g_key_file_new();
        g_key_file_load_from_file(kf, path, G_KEY_FILE_NONE, NULL);
        lab = g_key_file_get_string(kf, base, "user_label", NULL);
        if (!lab || !lab[0]) {
            g_free(lab);
            lab = g_strdup("blank_label");
        }
        xs_short_uuid(uuid);
        newbase = g_strdup_printf("%s-%s-%s", base, uuid, lab);
        nk = g_key_file_new();
        if (g_key_file_has_group(kf, base))
            keys = g_key_file_get_keys(kf, base, &nkeys, NULL);
        if (keys) {
            for (gsize i = 0; i < nkeys; i++) {
                char *v = g_key_file_get_string(kf, base, keys[i], NULL);

                if (v) {
                    g_key_file_set_string(nk, newbase, keys[i], v);
                    g_free(v);
                }
            }
            g_strfreev(keys);
        }
        g_key_file_set_string(nk, newbase, "user_label", lab);
        data = g_key_file_to_data(nk, &len, NULL);
        g_key_file_free(nk);
        g_key_file_free(kf);
        newpath = g_build_filename(g_plugin_conf_dir, newbase, NULL);
        g_file_set_contents(newpath, data, -1, NULL);
        g_free(data);
        remove(path);
        /* symlink в plugins_on */
        dotpath = g_path_get_dirname(g_plugin_conf_dir); /* .../xscreenlets */
        linkpath = g_build_filename(dotpath, "plugins_on", newbase, NULL);
        target = g_build_filename(dotpath, ".plugins", newbase, NULL);
        unlink(linkpath);
        if (symlink(target, linkpath) != 0)
            xs_log_impl("migrate: symlink %s failed", linkpath);
        xs_log_impl("migrated %s -> %s", fn, newbase);
        g_free(target);
        g_free(linkpath);
        g_free(dotpath);
        g_free(newpath);
        g_free(newbase);
        g_free(lab);
        g_free(sec);
        g_free(base);
        g_free(path);
    }
    g_dir_close(d);
    /* старый plugins/ больше не нужен: удаляем, если пуст */
    if (rmdir(legacy_dir) == 0)
        xs_log_impl("removed empty legacy dir %s", legacy_dir);
}

/* --- плагин-конфиги --- */

static void xs_plugin_conf_free_value(GKeyFile *kf)
{
    if (kf)
        g_key_file_free(kf);
}

static char *xs_plugin_conf_path(const char *name)
{
    char *file = g_strdup_printf("%s.conf", name);
    char *path = g_build_filename(g_plugin_conf_dir ? g_plugin_conf_dir : ".",
                                  file, NULL);
    g_free(file);
    return path;
}

/* Каталог включённых конфигов (symlink'и; только для демона). */
const char *xs_core_onoff_dir(void)
{
    return g_plugin_onoff_dir;
}

/* Каталог реальных конфигов (использует applet_manager). */
const char *xs_core_plugins_dir(void)
{
    return g_plugin_conf_dir;
}

/* Слабая реализация: у standalone (xclock) нет main.c с plugdir. */
__attribute__((weak)) const char *xs_core_plugdir(void)
{
    return NULL;
}

/* Короткий UUID (8 hex-символов) для имени инстанса. */
static void xs_short_uuid(char buf[9])
{
    guint32 v[2];

    for (int i = 0; i < 2; i++) {
        v[i] = g_random_int();
    }
    g_snprintf(buf, 9, "%08x", (v[0] ^ v[1]) & 0xffffffffu);
}

/* Обновить symlink в plugins_on (после переименования конфига или
 * включения инстанса). old_link = NULL → просто создать новый. */
static void xs_onoff_relink(const char *old_name, const char *new_name)
{
    char *dir, *oldlink, *newlink;

    if (!g_plugin_onoff_dir || !new_name || !new_name[0])
        return;
    char *new_conf = g_strdup_printf("%s.conf", new_name);
    char *old_conf = (old_name && old_name[0])
                         ? g_strdup_printf("%s.conf", old_name) : NULL;

    dir = g_path_get_dirname(g_plugin_onoff_dir);
    newlink = g_build_filename(g_plugin_onoff_dir, new_conf, NULL);
    unlink(newlink);
    {
        char *target = g_build_filename(dir, ".plugins",
                                        new_conf, NULL);

        if (symlink(target, newlink) != 0)
            xs_log_impl("symlink %s -> %s failed", newlink, target);
        g_free(target);
    }
    if (old_conf) {
        oldlink = g_build_filename(g_plugin_onoff_dir, old_conf, NULL);
        unlink(oldlink);
        g_free(oldlink);
    }
    g_free(new_conf);
    g_free(old_conf);
    g_free(newlink);
    g_free(dir);
}

GKeyFile *xs_core_plugin_conf(const char *name)
{
    GKeyFile *kf;

    if (!g_plugin_confs)
        g_plugin_confs = g_hash_table_new_full(g_str_hash, g_str_equal,
                                               g_free,
                                               (GDestroyNotify)xs_plugin_conf_free_value);
    kf = g_hash_table_lookup(g_plugin_confs, name);
    if (kf)
        return kf;
    kf = g_key_file_new();
    char *path = xs_plugin_conf_path(name);
    if (!g_key_file_load_from_file(kf, path, G_KEY_FILE_NONE, NULL))
        xs_log_impl("plugin config %s: new", path);
    else
        xs_log_impl("plugin config: %s", path);
    g_hash_table_insert(g_plugin_confs, g_strdup(name), kf);
    g_free(path);
    return kf;
}

void xs_core_plugin_conf_flush(const char *name)
{
    GKeyFile *kf;
    char *path;
    char *data;
    gsize length;

    if (!g_plugin_confs)
        return;
    kf = g_hash_table_lookup(g_plugin_confs, name);
    if (!kf)
        return;
    path = xs_plugin_conf_path(name);
    data = g_key_file_to_data(kf, &length, NULL);
    if (!g_file_set_contents(path, data, -1, NULL))
        xs_log_impl("cannot write plugin config %s", path);
    g_free(data);
    g_free(path);
}

/* Перечитать все плагин-конфиги с диска (SIGHUP). */
static void xs_plugin_confs_reload(void)
{
    GHashTableIter it;
    gpointer key, val;

    if (!g_plugin_confs)
        return;
    g_hash_table_iter_init(&it, g_plugin_confs);
    while (g_hash_table_iter_next(&it, &key, &val)) {
        char *path = xs_plugin_conf_path((const char *)key);
        g_key_file_load_from_file((GKeyFile *)val, path,
                                  G_KEY_FILE_KEEP_COMMENTS, NULL);
        g_free(path);
    }
}

void xs_core_conf_flush(void)
{
    char *data;
    gsize length;

    if (!g_conf || !g_conf_path || g_conf_path[0] == '\0')
        return;
    data = g_key_file_to_data(g_conf, &length, NULL);
    if (!data) {
        xs_log_impl("cannot serialize config %s", g_conf_path);
        return;
    }
    if (!g_file_set_contents(g_conf_path, data, length, NULL))
        xs_log_impl("cannot write config %s", g_conf_path);
    g_free(data);
}

/* Forward declarations */
static cairo_surface_t *capture_frame(cairo_t *cr, int w, int h);
static void reset_shape_schedule(XsWinState *state);
static gboolean xs_core_tick_cb(XsPlugin *p);
static void set_tick(XsPlugin *p, guint ms);
static gboolean theme_load(XsPlugin *p, const char *dir);
static void theme_draw(XsPlugin *p, cairo_t *cr, const char *el,
                       double x, double y, double width);
static void theme_draw_full(XsPlugin *p, cairo_t *cr, const char *el,
                            double x, double y, double width, double height);
static void theme_draw_native(XsPlugin *p, cairo_t *cr, const char *el,
                              double x, double y);
/* --- хостинг гостей (frame_launcher) --- */
XsPlugin *xs_core_start_guest_instance(XsPlugin *host,
                                       const char *guest_name);
GtkWidget *host_content_widget(XsPlugin *p);
void xs_core_guest_set_started_by(XsPlugin *g, const char *who);
/* создать инстанс с ЗАДАННЫМ именем (не генерить UUID), для гостей */
XsPlugin *xs_core_add_instance_for_host(const char *type,
                                        const char *iname);
/* --- хостинг гостей (frame_launcher): запуск/останов инстанса из
 * конфига по запросу плагина-хоста, с защитой от циклов --- */
static XsPlugin *start_guest(XsPlugin *host, const char *guest_name);
static void stop_guest(XsPlugin *host, const char *guest_name);
/* снимок куска фона рамки под гостем (в data окна "xs-host-backdrop") */
static void host_update_guest_backdrop(XsPlugin *host, XsPlugin *g);
/* контекст отложенного снимка */
typedef struct {
    XsPlugin *host;
    XsPlugin *guest;
} GuestBgCtx;
static gpointer guest_bg_ctx_new(XsPlugin *host, XsPlugin *guest);
static gboolean host_update_guest_backdrop_idle(gpointer data);
/* повторное применение позиции гостя (фикс «съеденного» первого move) */
typedef struct {
    XsPlugin *host;
    XsPlugin *guest;
} GuestReapplyCtx;
static gboolean guest_reapply_pos_idle(gpointer data);
static gpointer guest_reapply_ctx_new(XsPlugin *host, XsPlugin *guest);
/* создать новый инстанс типа и включить гостем хоста */
static XsPlugin *start_guest_new(XsPlugin *host, const char *type);
/* списки для диалогов frame_launcher */
char **xs_core_list_plugin_types(void);
int xs_core_type_count(void);
char **xs_core_list_running_daemon_instances(int *count);
static void conf_set_int(GKeyFile *kf, const char *s, const char *k, int v);
static double conf_dbl(GKeyFile *kf, const char *s, const char *k, double d);
static char *conf_str(GKeyFile *kf, const char *s, const char *k, const char *d);
static void conf_set_str(GKeyFile *kf, const char *s, const char *k, const char *v);
static gboolean on_button(GtkWidget *w, GdkEventButton *ev, gpointer d);
static gboolean on_motion(GtkWidget *w, GdkEventMotion *ev, gpointer d);
static gboolean on_draw_frame(GtkWidget *area, cairo_t *cr, gpointer d);
static gboolean on_configure_event(GtkWidget *w, GdkEventConfigure *ev, gpointer d);
static void on_window_destroy(GtkWidget *w, gpointer d);
static GtkWidget *make_window(XsPlugin *p, int x, int y, int w, int h);
static void xs_core_save_plugin_position(XsPlugin *p);
static gboolean xs_core_input_shape_idle(XsPlugin *p);

/* Forward declarations */
static guint g_props_label_rename_id = 0;
/* Заморозка x/y на время правки метки/переименования конфига */
static gboolean g_props_label_freeze = FALSE;
static gboolean xs_prop_label_rename_cb(gpointer data);
static gboolean xs_core_menu_dispatch_idle(gpointer data)
{
    XsCmd *c = data;

    if (c->plug && c->cmd) {
        if (g_str_has_prefix(c->cmd, "p:")) {
            if (c->plug->ops && c->plug->ops->menu_cmd)
                c->plug->ops->menu_cmd(c->plug, c->cmd + 2);
            xs_core_plugin_conf_flush(c->plug->name);
        } else {
            xs_core_dispatch_cmd(c->plug, c->cmd);
        }
    }
    g_free(c->cmd);
    g_free(c);
    return G_SOURCE_REMOVE;
}

void xs_core_menu_activate(GtkMenuItem *item, gpointer data)
{
    XsPlugin *p = data;
    const char *cmd = g_object_get_data(G_OBJECT(item), "xs-cmd");
    XsCmd *c;

    if (!cmd || !p)
        return;
    c = g_new0(XsCmd, 1);
    c->plug = p;
    c->cmd = g_strdup(cmd);
    g_idle_add_full(G_PRIORITY_HIGH_IDLE, xs_core_menu_dispatch_idle, c, NULL);
}

static gboolean xs_core_menu_destroy_idle(gpointer data)
{
    gtk_widget_destroy(GTK_WIDGET(data));
    return G_SOURCE_REMOVE;
}

/* Меню одноразовое: после закрытия уничтожается (иначе утечка на каждый ПКМ). */
static void xs_core_menu_selection_done(GtkMenuShell *shell, gpointer data)
{
    (void)shell;
    g_idle_add(xs_core_menu_destroy_idle, data);
}

GtkWidget *xs_core_add_separator(GtkWidget *menu)
{
    GtkWidget *sep = gtk_separator_menu_item_new();

    gtk_menu_shell_append(GTK_MENU_SHELL(menu), sep);
    return sep;
}

static gboolean xs_core_win_flag_get(XsWinState *state, const char *what)
{
    if (!state)
        return FALSE;
    if (strcmp(what, "lock") == 0)
        return state->locked;
    if (strcmp(what, "sticky") == 0)
        return state->sticky;
    if (strcmp(what, "widget") == 0)
        return state->widget;
    if (strcmp(what, "above") == 0)
        return state->keep_above;
    if (strcmp(what, "below") == 0)
        return state->keep_below;
    return FALSE;
}

static void xs_core_set_win_flag(XsWinState *state, const char *what,
                                 gboolean active)
{
    if (strcmp(what, "lock") == 0) {
        state->locked = active;
    } else if (strcmp(what, "sticky") == 0) {
        state->sticky = active;
    } else if (strcmp(what, "widget") == 0) {
        state->widget = active;
    } else if (strcmp(what, "above") == 0) {
        state->keep_above = active;
        if (active)
            state->keep_below = FALSE;
    } else if (strcmp(what, "below") == 0) {
        state->keep_below = active;
        if (active)
            state->keep_above = FALSE;
    }
}

/* Живое применение флагов окна (без recreate). Widget — как в оригинале:
 * свойство _COMPIZ_WIDGET на X-окне (для compiz widget-плагина). */
static void xs_core_apply_window_flags(XsWinState *state)
{
    GdkWindow *gdkwin;

    if (!state || !state->win)
        return;
    if (state->keep_above && state->keep_below)
        state->keep_below = FALSE;
    if (state->sticky)
        gtk_window_stick(GTK_WINDOW(state->win));
    else
        gtk_window_unstick(GTK_WINDOW(state->win));
    gtk_window_set_keep_above(GTK_WINDOW(state->win), state->keep_above);
    gtk_window_set_keep_below(GTK_WINDOW(state->win), state->keep_below);
    gdkwin = gtk_widget_get_window(state->win);
    if (gdkwin) {
        if (state->widget) {
            long val = 1;
            gdk_property_change(gdkwin,
                                gdk_atom_intern("_COMPIZ_WIDGET", FALSE),
                                gdk_atom_intern("WINDOW", FALSE), 32,
                                GDK_PROP_MODE_REPLACE, (const guchar *)&val, 1);
        } else {
            gdk_property_delete(gdkwin,
                                gdk_atom_intern("_COMPIZ_WIDGET", FALSE));
        }
    }
}

static void xs_core_save_window_flags(XsPlugin *p)
{
    XsWinState *state;
    GKeyFile *kf;

    if (!p || !p->name || !p->win)
        return;
    state = g_object_get_data(G_OBJECT(p->win), "xs-state");
    if (!state)
        return;
    kf = xs_core_plugin_conf(p->name);
    g_key_file_set_boolean(kf, p->name, "lock", state->locked);
    g_key_file_set_boolean(kf, p->name, "sticky", state->sticky);
    g_key_file_set_boolean(kf, p->name, "widget", state->widget);
    g_key_file_set_boolean(kf, p->name, "keep_above", state->keep_above);
    g_key_file_set_boolean(kf, p->name, "keep_below", state->keep_below);
    xs_core_plugin_conf_flush(p->name);
}

static void xs_core_window_toggled(GtkCheckMenuItem *mi, gpointer data)
{
    XsPlugin *p = data;
    XsWinState *state;
    const char *what;

    if (!p)
        return;
    state = p->win ? g_object_get_data(G_OBJECT(p->win), "xs-state") : NULL;
    what = g_object_get_data(G_OBJECT(mi), "xs-what");
    if (!state || state->freed || !what)
        return;
    xs_core_set_win_flag(state, what, gtk_check_menu_item_get_active(mi));
    xs_core_apply_window_flags(state);
    xs_core_save_window_flags(p);
}

/* --- Properties-диалог: вкладки About/Options/Themes, как OptionsDialog --- */

static void xs_core_prop_win_toggled(GtkToggleButton *btn, gpointer data)
{
    XsPlugin *p = data;
    XsWinState *state;
    const char *what;

    if (!p || !p->win)
        return;
    state = g_object_get_data(G_OBJECT(p->win), "xs-state");
    what = g_object_get_data(G_OBJECT(btn), "xs-what");
    if (!state || state->freed || !what)
        return;
    xs_core_set_win_flag(state, what, gtk_toggle_button_get_active(btn));
    xs_core_apply_window_flags(state);
    xs_core_save_window_flags(p);
}

/* Отложенное переименование конфига после правки User label (debounce:
 * только когда ввод прекратился на 800 мс). Переименовывает файл в
 * .plugins (тип-UUID-новая_метка), секцию, symlink в plugins_on и имя
 * инстанса. Данные (окно) не пересоздаются — имя меняем на живом
 * инстансе и во всех кэшах. */

/* Проверить, занято ли имя конфига (кроме самого инстанса old_name). */
static gboolean xs_conf_name_taken(const char *name, const char *old_name)
{
    char *path;
    gboolean taken;

    if (old_name && strcmp(name, old_name) == 0)
        return FALSE;
    path = xs_plugin_conf_path(name);
    taken = g_file_test(path, G_FILE_TEST_EXISTS);
    g_free(path);
    return taken;
}

static XsPlugin *find_plugin_by_name(const char *name);
XsPlugin *xs_core_find_instance(const char *name)
{
    if (!name || !name[0] || !g_plugins)
        return NULL;
    return find_plugin_by_name(name);
}

void xs_core_drop_conf_cache(const char *name)
{
    if (g_plugin_confs && name && name[0])
        g_hash_table_remove(g_plugin_confs, name);
}
static void xs_tray_add_plugin_wrapper(XsPlugin *p);

static gboolean xs_prop_label_rename_cb(gpointer data)
{
    XsPlugin *p = data;
    XsWinState *state;
    GKeyFile *kf;
    char *lab;
    char *uuid = NULL;
    char *oldname_for_guests = NULL;
    char *dash;
    char *newname;
    char *oldpath, *newpath;
    char *link, *target, *oldlink;

    g_props_label_rename_id = 0;
    g_props_label_freeze = TRUE; /* конфиг/позиция заморожены до конца */
    if (!p || !p->name || !p->win)
        return G_SOURCE_REMOVE;
    state = g_object_get_data(G_OBJECT(p->win), "xs-state");
    if (!state || state->freed)
        return G_SOURCE_REMOVE;
    kf = xs_core_plugin_conf(p->name);
    lab = conf_str(kf, p->name, "user_label", "blank_label");
    if (!lab || !lab[0]) {
        g_free(lab);
        lab = g_strdup("blank_label");
    }
    /* запрет опасных символов в имени файла */
    for (char *s = lab; *s; s++) {
        if (*s == '/' || *s == '\\')
            *s = '_';
    }
    /* новый name: текущий-тип до первого UUID → тип-UUID-метка.
     * Имя = <тип>-<UUID8>-<label>; тип и UUID берём из текущего имени. */
    dash = strchr(p->name, '-');
    if (!dash) {
        g_free(lab);
        return G_SOURCE_REMOVE;
    }
    {
        gsize tl = dash - p->name;
        const char *rest = dash + 1; /* UUID8-label */

        if (strlen(rest) < 9 || rest[8] != '-') {
            g_free(lab);
            return G_SOURCE_REMOVE;
        }
        newname = g_strdup_printf("%.*s-%.8s-%s", (int)tl, p->name,
                                  rest, lab);
    }
    if (strcmp(newname, p->name) == 0) {
        g_free(newname);
        g_free(lab);
        return G_SOURCE_REMOVE;
    }
    if (xs_conf_name_taken(newname, p->name)) {
        xs_log_impl("rename: '%s' уже существует, метка сохранена в конфиге",
                    newname);
        g_free(newname);
        g_free(lab);
        return G_SOURCE_REMOVE;
    }
    /* 1) файл конфига */
    oldpath = xs_plugin_conf_path(p->name);
    newpath = xs_plugin_conf_path(newname);
    if (rename(oldpath, newpath) != 0) {
        xs_log_impl("rename %s -> %s failed", oldpath, newpath);
        g_free(oldpath);
        g_free(newpath);
        g_free(newname);
        g_free(lab);
        return G_SOURCE_REMOVE;
    }
    /* 2) ключи в кэше конфигов: перенаправить запись кэша на новое имя */
    if (g_plugin_confs) {
        gpointer key, val;

        if (g_hash_table_steal_extended(g_plugin_confs, p->name,
                                        &key, &val)) {
            g_hash_table_insert(g_plugin_confs, g_strdup(newname), val);
            g_free(key);
        }
    }
    /* 3) переписать секцию в конфиге */
    kf = xs_core_plugin_conf(newname);
    {
        gchar **keys = NULL;
        gsize n = 0;

        if (g_key_file_has_group(kf, p->name))
            keys = g_key_file_get_keys(kf, p->name, &n, NULL);
        if (keys) {
            GKeyFile *nk = g_key_file_new();

            for (gsize i = 0; i < n; i++) {
                char *v = g_key_file_get_string(kf, p->name, keys[i],
                                                NULL);

                if (v) {
                    g_key_file_set_string(nk, newname, keys[i], v);
                    g_free(v);
                }
            }
            g_strfreev(keys);
            {
                char *data;
                gsize len;

                data = g_key_file_to_data(nk, &len, NULL);
                g_file_set_contents(newpath, data, -1, NULL);
                g_free(data);
            }
            g_key_file_free(nk);
        }
    }
    /* Кэш после записи файла содержит старую секцию — перечитать
     * файл В ТОТ ЖЕ объект (steal+insert создавал ВТОРОЙ объект:
     * priv->kf плагинов продолжал указывать на старый, их правки
     * (icon/scale) писались в осиротевший кэш и терялись при
     * следующем flush по имени). */
    kf = g_hash_table_lookup(g_plugin_confs, newname);
    if (kf)
        g_key_file_load_from_file(kf, newpath, G_KEY_FILE_NONE,
                                  NULL);
    /* 4) symlink в plugins_on: только для main_daemon-инстансов.
     * Гость рамки (xs-guest-host) symlink не имеет/не создаёт:
     * запускает его хозяин, а не демон. При этом мусорные ссылки
     * (старые форматы) подчищаются в любом случае. */
    {
        gboolean is_guest = g_object_get_data(G_OBJECT(p->win),
                                              "xs-guest-host") != NULL;

        if (g_plugin_onoff_dir) {
            char *newname_conf = g_strdup_printf("%s.conf", newname);
            char *oldname_conf = g_strdup_printf("%s.conf", p->name);

            oldlink = g_build_filename(g_plugin_onoff_dir,
                                       oldname_conf, NULL);
            link = g_build_filename(g_plugin_onoff_dir,
                                    newname_conf, NULL);
            target = g_build_filename(
                g_path_get_dirname(g_plugin_onoff_dir), ".plugins",
                newname_conf, NULL);
            unlink(link);
            if (!is_guest && symlink(target, link) != 0)
                xs_log_impl("rename: symlink %s failed", link);
            unlink(oldlink);
            /* мусорные варианты без .conf из прошлых версий */
            {
                char *junk = g_build_filename(g_plugin_onoff_dir,
                                              newname, NULL);

                unlink(junk);
                g_free(junk);
            }
            g_free(oldname_conf);
            g_free(newname_conf);
            g_free(oldlink);
            g_free(link);
            g_free(target);
        }
    }
    /* 5) имя инстанса на живом объекте */
    /* Кэш-запись под старым именем больше не нужна (ключ уже перенаправлен
     * выше), но на всякий случай снимаем оставшийся дубликат. */
    if (g_plugin_confs &&
        g_hash_table_contains(g_plugin_confs, p->name) &&
        strcmp(p->name, newname) != 0) {
        g_hash_table_remove(g_plugin_confs, p->name);
    }
    oldname_for_guests = g_strdup(p->name);
    g_free((char *)p->name);
    p->name = newname;
    if (p->type)
        ; /* тип не меняется */
    gtk_window_set_title(GTK_WINDOW(p->win), newname);
    /* 7) если гость в рамке — обновить guests_N в конфиге хозяина */
    {
        const char *host_name = g_object_get_data(G_OBJECT(p->win),
                                                  "xs-guest-host");

        if (host_name && host_name[0]) {
            XsPlugin *host = find_plugin_by_name(host_name);

            if (host && host->name) {
                GKeyFile *hkf = xs_core_plugin_conf(host->name);
                int gi;

                for (gi = 1; gi <= 64; gi++) {
                    char *gkey = g_strdup_printf("guests_%d", gi);
                    char *gv = g_key_file_get_string(
                        hkf, host->name, gkey, NULL);

                    if (gv && strcmp(gv, oldname_for_guests) == 0) {
                        g_key_file_set_string(hkf, host->name, gkey,
                                              newname);
                        g_free(gv);
                        xs_core_plugin_conf_flush(host->name);
                        xs_log_impl(
                            "guest rename: %s guests_%d -> %s",
                            host->name, gi, newname);
                        g_free(gkey);
                        break;
                    }
                    g_free(gv);
                    g_free(gkey);
                    if (!gv)
                        break;
                }
                xs_tray_rebuild();
            }
        }
    }
    xs_log_impl("renamed instance -> %s (label '%s')", newname, lab);
    g_free(lab);
    g_free(oldname_for_guests);
    g_free(oldpath);
    g_free(newpath);
    g_free(uuid);
    /* 6) меню трея: пересобрать с новым именем */
    xs_tray_rebuild();
    /* Заморозка снята: после пересборки меню и всех записей. Позицию
     * окна обновить актуальной (state->x/y уже актуальны). */
    g_props_label_freeze = FALSE;
    return G_SOURCE_REMOVE;
}

static void xs_core_prop_label_changed(GtkEditable *e, gpointer data)
{
    XsPlugin *p = data;
    GKeyFile *kf;
    char *txt;

    if (!p || !p->name)
        return;
    kf = xs_core_plugin_conf(p->name);
    txt = gtk_editable_get_chars(e, 0, -1);
    g_key_file_set_string(kf, p->name, "user_label",
                          (txt && txt[0]) ? txt : "blank_label");
    g_free(txt);
    xs_core_plugin_conf_flush(p->name);
    /* debounce: переименование через 800 мс после последней правки */
    if (g_props_label_rename_id)
        g_source_remove(g_props_label_rename_id);
    g_props_label_rename_id = g_timeout_add(800, xs_prop_label_rename_cb,
                                            p);
    g_props_label_freeze = TRUE;
}

/* Текущее открытое окно Properties (для восстановления keep-above после
 * recreate). Диалог один на процесс — как в оригинале se.run(). */
static GtkWindow *g_props_dialog = NULL;
/* Spin-кнопки X/Y открытого Properties-диалога: обновляются при перемещении
 * окна апплета (debounce: только когда перемещение прекратилось на 500 мс). */
static GtkSpinButton *g_props_spin_x = NULL;
static GtkSpinButton *g_props_spin_y = NULL;
/* Какому плагину принадлежат открытые спиннеры X/Y */
static XsPlugin *g_props_spin_plugin = NULL;
static guint g_props_pos_update_id = 0;

static void xs_core_props_destroyed(GtkWidget *w, gpointer data)
{
    (void)w;
    (void)data;
    g_props_dialog = NULL;
    g_props_spin_x = NULL;
    g_props_spin_y = NULL;
    g_props_spin_plugin = NULL;
    if (g_props_pos_update_id) {
        g_source_remove(g_props_pos_update_id);
        g_props_pos_update_id = 0;
    }
}

static void xs_core_prop_scale_changed(GtkSpinButton *spin, gpointer data)
{
    XsPlugin *p = data;
    GKeyFile *kf;
    double scale;

    if (!p || !p->name)
        return;
    kf = xs_core_plugin_conf(p->name);
    scale = gtk_spin_button_get_value(spin);
    g_key_file_set_double(kf, p->name, "scale", scale);
    xs_core_plugin_conf_flush(p->name);
    /* Размер окна не задаётся отсюда (240*x — канон часов): каждый плагин
     * сам подгоняет окно под свой канон на ближайшем tick. Для мгновенной
     * реакции часов — отдельная ветка ниже. */
    if (p->ops && p->ops->menu_cmd)
        p->ops->menu_cmd(p, "scale-applied");
}

static void xs_core_prop_opacity_changed(GtkRange *range, gpointer data)
{
    XsPlugin *p = data;
    GKeyFile *kf;

    if (!p || !p->name)
        return;
    xs_host_api()->set_opacity(p, gtk_range_get_value(range));
    kf = xs_core_plugin_conf(p->name);
    g_key_file_set_double(kf, p->name, "opacity", gtk_range_get_value(range));
    xs_core_plugin_conf_flush(p->name);
}

/* Ручное редактирование X/Y окна в Properties/Window: живьём перемещает
 * окно (gtk_window_move) и сохраняет в конфиг плагина — как configure-event.
 * Lock по-прежнему запрещает автоматически принимать новые координаты из
 * configure/drag, но не должен блокировать явно заданные пользователем X/Y. */
static void xs_core_prop_pos_changed(GtkSpinButton *spin, gpointer data)
{
    XsPlugin *p = data;
    XsWinState *state;
    GKeyFile *kf;
    const char *key = g_object_get_data(G_OBJECT(spin), "xs-pos");
    int v;

    if (!p || !p->name || !p->win || !key)
        return;
    state = g_object_get_data(G_OBJECT(p->win), "xs-state");
    if (!state || state->freed)
        return;
    v = (int)gtk_spin_button_get_value(spin);
    kf = xs_core_plugin_conf(p->name);
    g_key_file_set_integer(kf, p->name, key, v);
    xs_core_plugin_conf_flush(p->name);
    if (strcmp(key, "x") == 0)
        state->x = v;
    else
        state->y = v;
    {
        /* гость рамки: v — координаты ВНУТРИ content-зоны →
         * КЛАМП в зону (не вылезать за рамку) и пересчёт в
         * координаты родителя (cx/cy) */
        const char *host_name = g_object_get_data(G_OBJECT(p->win),
                                                  "xs-guest-host");
        XsPlugin *host = host_name ? find_plugin_by_name(host_name)
                                   : NULL;
        gint *xy = (host && host->win)
                       ? g_object_get_data(G_OBJECT(host->win),
                                           "xs-content-xy")
                       : NULL;

        if (xy) {
            XsWinState *hs = g_object_get_data(
                G_OBJECT(host->win), "xs-state");
            int cx = xy[0], cy = xy[1];
            int gw = 0, gh = 0, max_x, max_y;

            gtk_window_get_size(GTK_WINDOW(p->win), &gw, &gh);
            max_x = hs ? hs->w - cx * 2 - gw : 0;
            max_y = hs ? hs->h - cy * 2 - gh : 0;
            if (max_x < 0) max_x = 0;
            if (max_y < 0) max_y = 0;
            if (state->x > max_x) {
                state->x = max_x;
                g_key_file_set_integer(kf, p->name, "x", max_x);
            }
            if (state->y > max_y) {
                state->y = max_y;
                g_key_file_set_integer(kf, p->name, "y", max_y);
            }
            if (state->x < 0) {
                state->x = 0;
                g_key_file_set_integer(kf, p->name, "x", 0);
            }
            if (state->y < 0) {
                state->y = 0;
                g_key_file_set_integer(kf, p->name, "y", 0);
            }
            xs_log_impl("guest pos: %s spinner(%d,%d) -> (%d,%d) max=(%d,%d)",
                        p->name, strcmp(key, "x") == 0 ? v : state->x,
                        strcmp(key, "x") == 0 ? state->y : v,
                        state->x, state->y, max_x, max_y);
            xs_core_plugin_conf_flush(p->name);
            /* синхронизируем спиннер с клампнутым значением */
            gtk_spin_button_set_value(spin, strcmp(key, "x") == 0
                                                ? state->x : state->y);
            gtk_window_move(GTK_WINDOW(p->win),
                            cx + state->x, cy + state->y);
            /* фон под гостем изменился — обновить снимок (двойной:
             * сразу и после отрисовки кадра) */
            {
                GuestBgCtx *c1 = guest_bg_ctx_new(host, p);
                GuestBgCtx *c2 = guest_bg_ctx_new(host, p);

                g_timeout_add(120, host_update_guest_backdrop_idle, c1);
                g_timeout_add(500, host_update_guest_backdrop_idle, c2);
            }
        } else {
            gtk_window_move(GTK_WINDOW(p->win),
                            strcmp(key, "x") == 0 ? v : state->x,
                            strcmp(key, "x") == 0 ? state->y : v);
        }
    }
}

/* --- синхронизация spin-кнопок X/Y Properties с окном апплета при его
 * перемещении (Alt+drag и т.п.). Debounce: обновление только когда
 * configure-event не приходит 500 мс (перемещение прекратилось), чтобы
 * не мешать вводу и не дёргать окно настроек во время drag. --- */

static gboolean xs_core_props_pos_update_idle(gpointer data)
{
    XsPlugin *p = data;
    XsWinState *state;

    g_props_pos_update_id = 0;
    if (!p || !p->win || !g_props_dialog)
        return G_SOURCE_REMOVE;
    state = g_object_get_data(G_OBJECT(p->win), "xs-state");
    if (!state || state->freed)
        return G_SOURCE_REMOVE;
    /* ВАЖНО: спиннеры принадлежат диалогу КОНКРЕТНОГО плагина.
     * Обновляем только если диалог открыт именно для p, иначе
     * set_value чужого спиннера дёргает value-changed → pos_changed
     * с data другого плагина → окно прыгает на чужие координаты. */
    if (g_props_spin_plugin != p)
        return G_SOURCE_REMOVE;
    if (g_props_spin_x) {
        /* блокируем сигнал: set_value эмитит value-changed →
         * pos_changed → gtk_window_move (петля) */
        g_signal_handlers_block_by_func(
            g_props_spin_x, xs_core_prop_pos_changed, p);
        if ((int)gtk_spin_button_get_value(g_props_spin_x) != state->x)
            gtk_spin_button_set_value(g_props_spin_x, state->x);
        g_signal_handlers_unblock_by_func(
            g_props_spin_x, xs_core_prop_pos_changed, p);
    }
    if (g_props_spin_y) {
        g_signal_handlers_block_by_func(
            g_props_spin_y, xs_core_prop_pos_changed, p);
        if ((int)gtk_spin_button_get_value(g_props_spin_y) != state->y)
            gtk_spin_button_set_value(g_props_spin_y, state->y);
        g_signal_handlers_unblock_by_func(
            g_props_spin_y, xs_core_prop_pos_changed, p);
    }
    return G_SOURCE_REMOVE;
}

/* Вызывается из on_configure_event после каждого изменения позиции. */
static void xs_core_props_pos_schedule(XsPlugin *p)
{
    if (!g_props_dialog || (!g_props_spin_x && !g_props_spin_y))
        return;
    if (g_props_pos_update_id)
        g_source_remove(g_props_pos_update_id);
    /* 500 мс тишины после последнего configure-event = перемещение
     * прекратилось; тогда одним обновлением подводим координаты. */
    g_props_pos_update_id = g_timeout_add(500, xs_core_props_pos_update_idle,
                                          p);
}

/* ==== Хелперы Properties-диалога (стиль get_widget_for_option) ==== */

static GtkWidget *xs_prop_row(GtkBox *box, const char *label, const char *desc,
                              GtkWidget *input)
{
    /* Подпись и описание переводятся здесь, а не на стороне вызовов:
     * xs_prop_add_string/bool/int/float/choices вызываются десятки раз,
     * и оборачивать каждый литерал в _() отдельно - источник пропусков.
     * Ключ конфигурации идёт через g_object_set_data("xs-key") и сюда
     * не попадает, так что перевод подписи ему не мешает.
     *
     * В xgettext это выражено одним ключом xs_prop_add_*:2,3 - два
     * номера аргумента через запятую. Два отдельных ключа :2 и :3
     * не годятся: xgettext берёт ПОСЛЕДНИЙ, первый теряется. */
    GtkWidget *hbox = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 5);
    GtkWidget *lbl = gtk_label_new(_(label));

    gtk_widget_set_halign(lbl, GTK_ALIGN_START);
    gtk_widget_set_valign(lbl, GTK_ALIGN_START);
    gtk_widget_set_size_request(lbl, 180, 28);
    gtk_box_pack_start(GTK_BOX(hbox), lbl, FALSE, TRUE, 0);
    if (input) {
        if (desc) {
            /* Описание остаётся английским: в ключах xgettext его нет,
             * перевод тут был бы мёртвым кодом. Для перевода описаний
             * нужен отдельный ключ :3 - см. комментарий в Makefile. */
            gtk_widget_set_tooltip_text(input, desc);
        }
        gtk_box_pack_start(GTK_BOX(hbox), input, FALSE, TRUE, 0);
    }
    gtk_box_pack_start(box, hbox, FALSE, TRUE, 0);
    return input;
}

GtkWidget *xs_prop_add_bool(GtkBox *box, const char *label, const char *desc,
                            gboolean value)
{
    GtkWidget *w = gtk_check_button_new();

    gtk_toggle_button_set_active(GTK_TOGGLE_BUTTON(w), value);
    return xs_prop_row(box, label, desc, w);
}

GtkWidget *xs_prop_add_string(GtkBox *box, const char *label, const char *desc,
                              const char *value)
{
    GtkWidget *w = gtk_entry_new();

    if (value)
        gtk_entry_set_text(GTK_ENTRY(w), value);
    return xs_prop_row(box, label, desc, w);
}

GtkWidget *xs_prop_add_choices(GtkBox *box, const char *label, const char *desc,
                               const char *const *choices, const char *value)
{
    GtkWidget *w = gtk_combo_box_text_new();
    int pos = -1;
    int i;

    if (choices) {
        for (i = 0; choices[i]; i++) {
            gtk_combo_box_text_append_text(GTK_COMBO_BOX_TEXT(w), choices[i]);
            if (value && strcmp(choices[i], value) == 0)
                pos = i;
        }
    }
    if (pos >= 0)
        gtk_combo_box_set_active(GTK_COMBO_BOX(w), pos);
    return xs_prop_row(box, label, desc, w);
}

static GtkWidget *xs_prop_spin(GtkBox *box, const char *label, const char *desc,
                               double value, double min, double max,
                               double step, int digits)
{
    GtkWidget *w = gtk_spin_button_new_with_range(min, max, step);

    gtk_spin_button_set_digits(GTK_SPIN_BUTTON(w), digits);
    gtk_spin_button_set_value(GTK_SPIN_BUTTON(w), value);
    return xs_prop_row(box, label, desc, w);
}

GtkWidget *xs_prop_add_int(GtkBox *box, const char *label, const char *desc,
                           double value, double min, double max, double step)
{
    return xs_prop_spin(box, label, desc, value, min, max, step, 0);
}

GtkWidget *xs_prop_add_float(GtkBox *box, const char *label, const char *desc,
                             double value, double min, double max,
                             double step, int digits)
{
    return xs_prop_spin(box, label, desc, value, min, max, step, digits);
}

#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wdeprecated-declarations"
GtkWidget *xs_prop_add_color(GtkBox *box, const char *label, const char *desc,
                             double r, double g, double b, double a)
{
    GtkWidget *w = gtk_color_button_new_with_rgba(
        &(GdkRGBA){(gdouble)r, (gdouble)g, (gdouble)b, (gdouble)a});

    gtk_color_button_set_use_alpha(GTK_COLOR_BUTTON(w), TRUE);
    return xs_prop_row(box, label, desc, w);
}

GtkWidget *xs_prop_add_font(GtkBox *box, const char *label, const char *desc,
                            const char *value)
{
    GtkWidget *w = gtk_font_button_new();

    if (value)
        gtk_font_button_set_font_name(GTK_FONT_BUTTON(w), value);
    return xs_prop_row(box, label, desc, w);
}
#pragma GCC diagnostic pop

GtkWidget *xs_prop_add_time(GtkBox *box, const char *label, const char *desc,
                            int h, int m, int s)
{
    GtkWidget *w = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 3);
    GtkWidget *spin;
    struct { int val; int max; const char *suffix; } parts[3] = {
        {h, 23, "h"}, {m, 59, "m"}, {s, 59, "s"}
    };
    size_t i;

    for (i = 0; i < G_N_ELEMENTS(parts); i++) {
        spin = gtk_spin_button_new_with_range(0, parts[i].max, 1);
        gtk_spin_button_set_value(GTK_SPIN_BUTTON(spin), parts[i].val);
        gtk_box_pack_start(GTK_BOX(w), spin, FALSE, TRUE, 0);
        gtk_box_pack_start(GTK_BOX(w),
                           gtk_label_new(parts[i].suffix), FALSE, TRUE, 0);
    }
    return xs_prop_row(box, label, desc, w);
}

void xs_prop_add_group_header(GtkBox *box, const char *info)
{
    GtkWidget *lbl;
    GtkWidget *sep;

    if (!box || !info || !info[0])
        return;
    /* Перевод здесь, как и в xs_prop_row: заголовок группы приходит
     * литералом из xs_prop_add_group_header(box, "..."), их много. */
    lbl = gtk_label_new(_(info));
    /* Перенос обязателен. Здесь ставят пояснения в два-три абзаца
     * (см. acpi_battery.c, frame_launcher.c), а окно Properties
     * фиксировано 490x450. Без переноса Gtk считает естественную ширину
     * label равной длине самой длинной строки и растягивает на неё всё
     * окно, из-за чего нижние поля уезжают за край экрана. */
    gtk_label_set_line_wrap(GTK_LABEL(lbl), TRUE);
    gtk_label_set_line_wrap_mode(GTK_LABEL(lbl), PANGO_WRAP_WORD_CHAR);
    /* Ширина в символах, а не в пикселях: не даёт метке стать шире окна
     * даже при длинном слове без пробелов (путь, идентификатор). */
    gtk_label_set_width_chars(GTK_LABEL(lbl), 46);
    gtk_label_set_max_width_chars(GTK_LABEL(lbl), 46);
    gtk_widget_set_halign(lbl, GTK_ALIGN_START);
    gtk_widget_set_valign(lbl, GTK_ALIGN_START);
    gtk_box_pack_start(box, lbl, FALSE, FALSE, 7);
    sep = gtk_separator_new(GTK_ORIENTATION_HORIZONTAL);
    gtk_box_pack_start(box, sep, FALSE, FALSE, 5);
}

GtkWidget *xs_prop_add_row(GtkBox *box, const char *label, const char *desc,
                           GtkWidget *input)
{
    return xs_prop_row(box, label, desc, input);
}

/* ==== конец хелперов Properties ==== */

static void xs_core_themes_cursor_changed(GtkTreeView *tv, gpointer data)
{
    XsPlugin *p = data;
    GtkTreeSelection *sel;
    GtkTreeModel *model;
    GtkTreeIter it;
    char *name;

    sel = gtk_tree_view_get_selection(tv);
    if (!sel || !gtk_tree_selection_get_selected(sel, &model, &it))
        return;
    gtk_tree_model_get(model, &it, 0, &name, -1);
    if (!name)
        return;
    /* как cursor-changed в оригинале: смена темы сразу, диалог живёт */
    if (p && p->ops && p->ops->menu_cmd) {
        GKeyFile *kf = xs_core_plugin_conf(p->name);
        char *cur = conf_str(kf, p->name, "theme", NULL);
        if (g_strcmp0(cur, name) != 0) {
            char *cmd = g_strdup_printf("theme:%s", name);
            p->ops->menu_cmd(p, cmd);
            g_free(cmd);
        }
        g_free(cur);
    }
    g_free(name);
}

/* Разметка строки темы — как __render_cell оригинала: name (ultrabold large)
 * « v<version>», info (small), by author (i, small); нет данных →
 * «<Имя> (no info available)». */
static void xs_core_themes_render(GtkTreeViewColumn *col, GtkCellRenderer *cell,
                                  GtkTreeModel *model, GtkTreeIter *it,
                                  gpointer data)
{
    char *name;
    char *info;
    char *author;
    char *version;
    char *mu;

    (void)col;
    (void)data;
    gtk_tree_model_get(model, it, 0, &name, 1, &info, 2, &author,
                       3, &version, -1);
    if (!name)
        return;
    if ((!info || !info[0]) && (!author || !author[0])) {
        mu = g_strdup_printf(
            "<b><span weight=\"ultrabold\" size=\"large\">%s</span></b> (no info available)",
            name);
    } else {
        if (!info)
            info = g_strdup("-");
        if (!author)
            author = g_strdup("-");
        if (!version)
            version = g_strdup("-");
        mu = g_strdup_printf(
            "<b><span weight=\"ultrabold\" size=\"large\">%s</span></b> v%s\n"
            "<small><span color=\"#555555\">%s</span></small>\n"
            "<i><small>by %s</small></i>",
            name, version, info, author);
    }
    g_object_set(cell, "markup", mu, NULL);
    g_free(mu);
    g_free(name);
    g_free(info);
    g_free(author);
    g_free(version);
}

static GtkWidget *xs_core_about_page(XsPlugin *p)
{
    GtkWidget *hbox = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 10);
    GtkWidget *img = gtk_image_new();
    GtkWidget *label = gtk_label_new(NULL);
    char *name_up = NULL;
    char *icon_path;
    GError *err = NULL;
    GdkPixbuf *pix;
    GString *mu;
    const char *ver;
    const char *desc;
    const char *author;

    if (p && p->name && p->name[0])
        name_up = g_strdup_printf("%c%s",
                                  g_ascii_toupper(p->name[0]), p->name + 1);
    icon_path = g_strdup_printf("/usr/share/screenlets/%s/icon.svg",
                                name_up ? name_up : "Clock");
    pix = gdk_pixbuf_new_from_file_at_size(icon_path, 64, 64, &err);
    if (!pix) {
        g_clear_error(&err);
        g_free(icon_path);
        icon_path = g_strdup_printf("/usr/share/screenlets/%s/icon.png",
                                    name_up ? name_up : "Clock");
        pix = gdk_pixbuf_new_from_file_at_size(icon_path, 64, 64, &err);
    }
    if (!pix) {
        g_clear_error(&err);
        gtk_image_set_from_icon_name(GTK_IMAGE(img), "gtk-properties",
                                     GTK_ICON_SIZE_DIALOG);
    } else {
        gtk_image_set_from_pixbuf(GTK_IMAGE(img), pix);
        g_object_unref(pix);
    }
    g_free(icon_path);
    g_free(name_up);
    gtk_widget_set_valign(img, GTK_ALIGN_START);
    gtk_box_pack_start(GTK_BOX(hbox), img, FALSE, TRUE, 10);

    /* Метаданные для разметки: desc/author/version — из дескриптора плагина.
     * desc/author — произвольный текст (могут содержать '&', '<' и т.п.),
     * поэтому экранируем для Pango-разметки. */
    desc = (p && p->desc) ? p->desc : "";
    author = (p && p->author) ? p->author : "";
    ver = (p && p->version) ? p->version : "";
    mu = g_string_new("");
    g_string_append_printf(mu, "\n<b><span size=\"xx-large\">%s</span></b>",
                           p && p->name ? p->name : "?");
    if (ver[0])
        g_string_append_printf(mu,
                               "  <span size=\"large\"><b>v%s</b></span>",
                               ver);
    {
        char *esc = g_markup_escape_text(desc, -1);

        g_string_append_printf(mu, "\n\n%s", esc);
        g_free(esc);
    }
    if (author[0]) {
        char *esc = g_markup_escape_text(author, -1);

        g_string_append_printf(mu, "\n<span size=\"small\">\n(c) %s</span>",
                               esc);
        g_free(esc);
    }
    gtk_label_set_markup(GTK_LABEL(label), mu->str);
    gtk_label_set_line_wrap(GTK_LABEL(label), TRUE);
    gtk_widget_set_halign(label, GTK_ALIGN_START);
    gtk_widget_set_valign(label, GTK_ALIGN_START);
    gtk_box_pack_start(GTK_BOX(hbox), label, TRUE, TRUE, 5);
    gtk_widget_show_all(hbox);
    g_string_free(mu, TRUE);
    return hbox;
}

static void xs_core_show_properties(XsPlugin *p)
{
    XsWinState *state;
    GtkWidget *dialog;
    GtkWidget *notebook;
    GtkWidget *page;
    GtkWidget *sw;
    GtkWidget *tree;
    GtkWidget *w;
    GtkListStore *store;
    GtkTreeViewColumn *col;
    GtkCellRenderer *cell;
    GKeyFile *kf;
    static const struct { const char *label; const char *what; } rows[] = {
        {"Lock position", "lock"},
        {"Sticky", "sticky"},
        {"Widget", "widget"},
        {"Keep above", "above"},
        {"Keep below", "below"},
    };
    size_t i;
    gint row;

    if (!p || !p->name || !p->win)
        return;
    state = g_object_get_data(G_OBJECT(p->win), "xs-state");
    if (!state || state->freed)
        return;
    kf = xs_core_plugin_conf(p->name);

    /* Без DESTROY_WITH_PARENT: при смене Scale/Theme окно плагина
     * пересоздаётся (recreate), старое окно уничтожается — диалог не должен
     * гибнуть вместе с ним. Транзиент-связь не задаём по той же причине. */
    dialog = gtk_dialog_new_with_buttons(p->name, NULL,
                                         0,
                                         "Close", GTK_RESPONSE_CLOSE, NULL);
    gtk_window_set_default_size(GTK_WINDOW(dialog), 490, 450);
    gtk_window_set_keep_above(GTK_WINDOW(dialog), TRUE);
    gtk_container_set_border_width(GTK_CONTAINER(dialog), 10);
    g_props_dialog = GTK_WINDOW(dialog);
    g_signal_connect(dialog, "destroy", G_CALLBACK(xs_core_props_destroyed),
                     NULL);

    notebook = gtk_notebook_new();

    /* --- About --- */
    gtk_notebook_append_page(GTK_NOTEBOOK(notebook),
                             xs_core_about_page(p),
                             gtk_label_new(_("About")));

    /* --- Options: внутренний notebook: Window + страницы плагина --- */
    page = gtk_box_new(GTK_ORIENTATION_VERTICAL, 3);
    gtk_container_set_border_width(GTK_CONTAINER(page), 5);
    {
        GtkWidget *inner_nb = gtk_notebook_new();
        GtkWidget *inner_page;

        inner_page = gtk_grid_new();
        gtk_grid_set_row_spacing(GTK_GRID(inner_page), 6);
        gtk_grid_set_column_spacing(GTK_GRID(inner_page), 12);
        gtk_container_set_border_width(GTK_CONTAINER(inner_page), 10);
        row = 0;

        /* Scale рисует core, ЕСЛИ плагин не просил наоборот
         * (лаунчер: свой комбо «метод + поля») */
        if (!(p->type && strcmp(p->type, "launcher") == 0)) {
        gtk_grid_attach(GTK_GRID(inner_page), gtk_label_new(_("Scale")),
                        0, row, 1, 1);
        {
            /* сотые доли масштаба: шаг 0.01, клавиши/кнопки 0.01|0.1.
             * Для гостей frame_launcher в mode=end_size Scale спиннер
             * глушится (он всё равно не влияет на размер). */
            double cur_scale = conf_dbl(kf, p->name, "scale", 1.0);
            const char *smode = conf_str(kf, p->name, "scale_mode",
                                         NULL);
            GtkWidget *spin;
            GtkAdjustment *adj = gtk_adjustment_new(cur_scale, 0.01,
                                                    10.0, 0.01, 0.1, 0.0);

            spin = gtk_spin_button_new(adj, 0.01, 2);
            gtk_spin_button_set_digits(GTK_SPIN_BUTTON(spin), 2);
            if (smode && strcmp(smode, "end_size") == 0) {
                gtk_widget_set_sensitive(spin, FALSE);
                gtk_widget_set_tooltip_text(
                    spin, _("Size is set by End width/height "
                            "(Resize mode = end_size)"));
            }
            gtk_grid_attach(GTK_GRID(inner_page), spin, 1, row, 1, 1);
            g_signal_connect(spin, "value-changed",
                             G_CALLBACK(xs_core_prop_scale_changed), p);
        }
        row++;
        }
        gtk_grid_attach(GTK_GRID(inner_page), gtk_label_new(_("Opacity")),
                        0, row, 1, 1);
        {
            GtkAdjustment *adj = gtk_adjustment_new(state->opacity, 0.1, 1.0,
                                                    0.05, 0.1, 0.0);
            GtkWidget *scale = gtk_scale_new(GTK_ORIENTATION_HORIZONTAL, adj);
            gtk_widget_set_size_request(scale, 200, -1);
            gtk_grid_attach(GTK_GRID(inner_page), scale, 1, row, 1, 1);
            g_signal_connect(scale, "value-changed",
                             G_CALLBACK(xs_core_prop_opacity_changed), p);
        }
        row++;
        gtk_grid_attach(GTK_GRID(inner_page), gtk_label_new(_("X-Position")),
                        0, row, 1, 1);
        {
            int cur_x = state->x;
            GtkAdjustment *adj = gtk_adjustment_new(cur_x, 0, 10000, 1, 10, 0.0);
            GtkWidget *spin = gtk_spin_button_new(adj, 1, 0);
            g_object_set_data_full(G_OBJECT(spin), "xs-pos",
                                   g_strdup("x"), g_free);
            gtk_grid_attach(GTK_GRID(inner_page), spin, 1, row, 1, 1);
            g_signal_connect(spin, "value-changed",
                             G_CALLBACK(xs_core_prop_pos_changed), p);
            g_props_spin_x = GTK_SPIN_BUTTON(spin);
            g_props_spin_plugin = p;
            g_object_add_weak_pointer(G_OBJECT(spin), (gpointer *)&g_props_spin_x);
        }
        row++;
        gtk_grid_attach(GTK_GRID(inner_page), gtk_label_new(_("Y-Position")),
                        0, row, 1, 1);
        {
            int cur_y = state->y;
            GtkAdjustment *adj = gtk_adjustment_new(cur_y, 0, 10000, 1, 10, 0.0);
            GtkWidget *spin = gtk_spin_button_new(adj, 1, 0);
            g_object_set_data_full(G_OBJECT(spin), "xs-pos",
                                   g_strdup("y"), g_free);
            gtk_grid_attach(GTK_GRID(inner_page), spin, 1, row, 1, 1);
            g_signal_connect(spin, "value-changed",
                             G_CALLBACK(xs_core_prop_pos_changed), p);
            g_props_spin_y = GTK_SPIN_BUTTON(spin);
            g_object_add_weak_pointer(G_OBJECT(spin), (gpointer *)&g_props_spin_y);
        }
        row++;
        /* User label: пользовательская метка инстанса (тип-UUID-label) */
        {
            char *lab = conf_str(kf, p->name, "user_label", "blank_label");
            GtkWidget *entry = gtk_entry_new();

            gtk_entry_set_text(GTK_ENTRY(entry), lab ? lab : "blank_label");
            g_free(lab);
            gtk_grid_attach(GTK_GRID(inner_page),
                            gtk_label_new(_("User label")), 0, row, 1, 1);
            gtk_grid_attach(GTK_GRID(inner_page), entry, 1, row, 1, 1);
            g_signal_connect(entry, "changed",
                             G_CALLBACK(xs_core_prop_label_changed), p);
        }
        row++;
        for (i = 0; i < G_N_ELEMENTS(rows); i++) {
            GtkWidget *cb = gtk_check_button_new_with_label(rows[i].label);
            gtk_toggle_button_set_active(GTK_TOGGLE_BUTTON(cb),
                                         xs_core_win_flag_get(state, rows[i].what));
            g_object_set_data_full(G_OBJECT(cb), "xs-what",
                                   g_strdup(rows[i].what), g_free);
            g_signal_connect(cb, "toggled",
                             G_CALLBACK(xs_core_prop_win_toggled), p);
            gtk_grid_attach(GTK_GRID(inner_page), cb, 0, row, 2, 1);
            row++;
        }
        gtk_notebook_append_page(GTK_NOTEBOOK(inner_nb), inner_page,
                                 gtk_label_new(_("Window")));
        gtk_widget_show_all(inner_nb);
        gtk_box_pack_start(GTK_BOX(page), inner_nb, TRUE, TRUE, 0);
    }
    gtk_widget_show_all(page);
    gtk_notebook_append_page(GTK_NOTEBOOK(notebook), page,
                             gtk_label_new(_("Options")));

    /* Свои страницы-группы плагина (Clock/Alarm/Face у clock) */
    if (p->ops && p->ops->properties)
        p->ops->properties(p, GTK_NOTEBOOK(notebook));

    /* --- Themes --- */
    if (p->ops && p->ops->fill_themes) {
        page = gtk_box_new(GTK_ORIENTATION_VERTICAL, 5);
        gtk_container_set_border_width(GTK_CONTAINER(page), 10);
        /* Строки склеиваются конкатенацией ДО вызова _: переводить
         * нужно весь текст целиком, иначе xgettext не сможет его
         * извлечь, а частичная строка вроде "Themes allow you to " в
         * каталоге переводов бессмысленна. */
        w = gtk_label_new(_("Themes allow you to easily switch the appearance "
                            "of your Screenlets. On this page you find a list "
                            "of all available themes for this Screenlet."));
        gtk_label_set_line_wrap(GTK_LABEL(w), TRUE);
        gtk_widget_set_halign(w, GTK_ALIGN_START);
        gtk_box_pack_start(GTK_BOX(page), w, FALSE, TRUE, 0);
        store = gtk_list_store_new(4, G_TYPE_STRING, G_TYPE_STRING,
                                   G_TYPE_STRING, G_TYPE_STRING);
        p->ops->fill_themes(p, store);
        tree = gtk_tree_view_new_with_model(GTK_TREE_MODEL(store));
        gtk_tree_view_set_headers_visible(GTK_TREE_VIEW(tree), FALSE);
        g_object_unref(store);
        cell = gtk_cell_renderer_text_new();
        col = gtk_tree_view_column_new();
        gtk_tree_view_column_pack_start(col, cell, TRUE);
        gtk_tree_view_column_set_cell_data_func(col, cell,
                                                xs_core_themes_render, p,
                                                NULL);
        gtk_tree_view_append_column(GTK_TREE_VIEW(tree), col);
        /* Предвыбор текущей темы ИЗ КОНФИГА до подключения cursor-changed:
         * иначе при показе страницы TreeView получает курсор на строку 0 и
         * handler переключает тему на первую в списке. */
        {
            char *cur = conf_str(kf, p->name, "theme", NULL);
            GtkTreeIter it;
            gboolean valid =
                gtk_tree_model_get_iter_first(GTK_TREE_MODEL(store), &it);

            while (valid) {
                char *name = NULL;

                gtk_tree_model_get(GTK_TREE_MODEL(store), &it, 0, &name, -1);
                if (name && g_strcmp0(name, cur) == 0) {
                    GtkTreePath *path = gtk_tree_model_get_path(
                        GTK_TREE_MODEL(store), &it);
                    gtk_tree_selection_select_iter(
                        gtk_tree_view_get_selection(GTK_TREE_VIEW(tree)), &it);
                    if (path) {
                        gtk_tree_view_scroll_to_cell(GTK_TREE_VIEW(tree),
                                                     path, NULL, TRUE, 0.5,
                                                     0.0);
                        gtk_tree_path_free(path);
                    }
                    g_free(name);
                    break;
                }
                g_free(name);
                valid = gtk_tree_model_iter_next(GTK_TREE_MODEL(store), &it);
            }
            g_free(cur);
            g_signal_connect(tree, "cursor-changed",
                             G_CALLBACK(xs_core_themes_cursor_changed), p);
        }
        sw = gtk_scrolled_window_new(NULL, NULL);
        gtk_scrolled_window_set_shadow_type(GTK_SCROLLED_WINDOW(sw),
                                            GTK_SHADOW_IN);
        gtk_scrolled_window_set_policy(GTK_SCROLLED_WINDOW(sw),
                                       GTK_POLICY_AUTOMATIC,
                                       GTK_POLICY_AUTOMATIC);
        gtk_container_add(GTK_CONTAINER(sw), tree);
        gtk_box_pack_start(GTK_BOX(page), sw, TRUE, TRUE, 0);
        gtk_widget_show_all(page);
        gtk_notebook_append_page(GTK_NOTEBOOK(notebook), page,
                                 gtk_label_new(_("Themes")));
    }

    gtk_widget_show_all(notebook);
    gtk_container_add(GTK_CONTAINER(gtk_dialog_get_content_area(
                          GTK_DIALOG(dialog))), notebook);
    g_signal_connect(dialog, "response", G_CALLBACK(gtk_widget_destroy), NULL);
    gtk_widget_show(dialog);
}

static void xs_core_show_about(XsPlugin *p)
{
    if (!p || !p->name)
        return;
    gtk_show_about_dialog(NULL,
                          "program-name", p->name,
                          "comments", "Xscreenlets plugin (GTK3)",
                          "version", "1.0",
                          NULL);
}

static void xs_core_menu_connect_cmd(GtkWidget *mi, XsPlugin *p, const char *cmd)
{
    g_object_set_data_full(G_OBJECT(mi), "xs-cmd", g_strdup(cmd), g_free);
    g_signal_connect(mi, "activate", G_CALLBACK(xs_core_menu_activate), p);
}

static void xs_core_popup_menu(XsPlugin *p, GdkEventButton *event)
{
    GtkWidget *menu;
    GtkWidget *mi;
    GtkWidget *item;
    GtkWidget *sub;
    XsWinState *state;
    size_t i;

    if (!p || !p->win || !event)
        return;
    state = g_object_get_data(G_OBJECT(p->win), "xs-state");
    menu = gtk_menu_new();

    /* Plugin items — как в оригинале, свои пункты Screenlet сверху.
     * Разделитель между своими и core-пунктами плагин добавляет сам. */
    if (p->ops && p->ops->menu)
        p->ops->menu(p, GTK_MENU(menu));

    /* Size — оригинальный список 16 значений */
    item = gtk_menu_item_new_with_label(_("Size"));
    sub = gtk_menu_new();
    {
        static const double sizes[] = {0.2, 0.3, 0.4, 0.5, 0.6, 0.7, 0.8, 0.9,
                                       1.0, 1.5, 2.0, 3.0, 4.0, 5.0, 7.5, 10.0};
        for (i = 0; i < G_N_ELEMENTS(sizes); i++) {
            char *lbl = g_strdup_printf("%.0f %%", sizes[i] * 100);
            char *cmd = g_strdup_printf("scale:%g", sizes[i]);
            mi = gtk_menu_item_new_with_label(lbl);
            g_free(lbl);
            xs_core_menu_connect_cmd(mi, p, cmd);
            g_free(cmd);
            gtk_menu_shell_append(GTK_MENU_SHELL(sub), mi);
        }
    }
    gtk_menu_item_set_submenu(GTK_MENU_ITEM(item), sub);
    gtk_menu_shell_append(GTK_MENU_SHELL(menu), item);

    /* Window — как в оригинале: Lock/Sticky/Widget/Keep above/Keep below */
    item = gtk_menu_item_new_with_label(_("Window"));
    sub = gtk_menu_new();
    {
        static const struct { const char *label; const char *what; } wins[] = {
            {"Lock", "lock"},
            {"Sticky", "sticky"},
            {"Widget", "widget"},
            {"Keep above", "above"},
            {"Keep below", "below"},
        };
        for (i = 0; i < G_N_ELEMENTS(wins); i++) {
            mi = gtk_check_menu_item_new_with_label(wins[i].label);
            gtk_check_menu_item_set_active(GTK_CHECK_MENU_ITEM(mi),
                                           xs_core_win_flag_get(state, wins[i].what));
            g_object_set_data_full(G_OBJECT(mi), "xs-what",
                                   g_strdup(wins[i].what), g_free);
            g_signal_connect(mi, "toggled",
                             G_CALLBACK(xs_core_window_toggled), p);
            gtk_menu_shell_append(GTK_MENU_SHELL(sub), mi);
        }
    }
    gtk_menu_item_set_submenu(GTK_MENU_ITEM(item), sub);
    gtk_menu_shell_append(GTK_MENU_SHELL(menu), item);

    xs_core_add_separator(menu);
    mi = gtk_menu_item_new_with_label(_("Properties..."));
    xs_core_menu_connect_cmd(mi, p, "properties");
    gtk_menu_shell_append(GTK_MENU_SHELL(menu), mi);
    mi = gtk_menu_item_new_with_label(_("Info..."));
    xs_core_menu_connect_cmd(mi, p, "about");
    gtk_menu_shell_append(GTK_MENU_SHELL(menu), mi);

    /* Add one more / Delete this — мультиинстанс (как в оригинале) */
    {
        const char *type = xs_core_plugin_type(p);
        char *lbl;

        xs_core_add_separator(menu);
        lbl = g_strdup_printf("Add one more %s", type);
        mi = gtk_menu_item_new_with_label(lbl);
        g_free(lbl);
        xs_core_menu_connect_cmd(mi, p, "add:");
        gtk_menu_shell_append(GTK_MENU_SHELL(menu), mi);
        lbl = g_strdup_printf("Delete this %s", type);
        mi = gtk_menu_item_new_with_label(lbl);
        g_free(lbl);
        xs_core_menu_connect_cmd(mi, p, "delete");
        gtk_menu_shell_append(GTK_MENU_SHELL(menu), mi);
    }

    xs_core_add_separator(menu);
    mi = gtk_menu_item_new_with_label(_("Quit"));
    xs_core_menu_connect_cmd(mi, p, "quit");
    gtk_menu_shell_append(GTK_MENU_SHELL(menu), mi);

    g_signal_connect(menu, "selection-done",
                     G_CALLBACK(xs_core_menu_selection_done), menu);
    gtk_widget_show_all(menu);
#if GTK_CHECK_VERSION(3, 22, 0)
    gtk_menu_popup_at_pointer(GTK_MENU(menu), (GdkEvent *)event);
#else
    gtk_menu_popup(GTK_MENU(menu), NULL, NULL, NULL, NULL, 3,
                   gdk_event_get_time((GdkEvent *)event));
#endif
}

static gboolean on_button(GtkWidget *widget, GdkEventButton *event,
                          gpointer data)
{
    XsPlugin *p = data;
    XsWinState *state;
    gboolean handled;

    (void)widget;
    if (!p || !p->win)
        return FALSE;
    state = g_object_get_data(G_OBJECT(p->win), "xs-state");
    if (!state)
        return FALSE;
    /* press → плагин (popup/анимация); release → плагину тоже:
     * click-анимация лаунчера завершается на release.
     * ОДИН вызов на событие — иначе launch срабатывает дважды. */
    handled = p->ops && p->ops->button ? p->ops->button(p, event) : FALSE;
    if (!handled && event->button == 3) {
        xs_core_popup_menu(p, event);
        return TRUE;
    }
    return handled ? TRUE : FALSE;
}

static gboolean on_scroll(GtkWidget *widget, GdkEventScroll *event,
                          gpointer data)
{
    XsPlugin *p = data;

    (void)widget;
    if (!p || !p->win)
        return FALSE;
    if (p->ops && p->ops->scroll && p->ops->scroll(p, event))
        return TRUE;
    return FALSE;
}

/* Курсор вошёл/вышел: коллбеки плагина (подсветка и т.п.). */
static gboolean on_enter_notify(GtkWidget *widget, GdkEventCrossing *ev,
                                gpointer data)
{
    XsPlugin *p = data;

    (void)widget;
    (void)ev;
    if (!p || !p->win)
        return FALSE;
    if (p->ops && p->ops->enter)
        p->ops->enter(p);
    return FALSE;
}

static gboolean on_leave_notify(GtkWidget *widget, GdkEventCrossing *ev,
                                gpointer data)
{
    XsPlugin *p = data;

    (void)widget;
    (void)ev;
    if (!p || !p->win)
        return FALSE;
    if (p->ops && p->ops->leave)
        p->ops->leave(p);
    return FALSE;
}

static gboolean on_motion(GtkWidget *widget, GdkEventMotion *event,
                          gpointer data)
{
    XsPlugin *p = data;
    XsWinState *state;

    (void)widget;
    if (!p || !p->win)
        return FALSE;
    state = g_object_get_data(G_OBJECT(p->win), "xs-state");
    if (!state)
        return FALSE;
    if (p->ops && p->ops->motion && p->ops->motion(p, event))
        return TRUE;
    return FALSE;
}

void xs_core_dispatch_cmd(XsPlugin *p, const char *cmd)
{
    XsWinState *state;

    if (!p || !p->name || !cmd)
        return;
    state = p->win ? g_object_get_data(G_OBJECT(p->win), "xs-state") : NULL;

    if (g_str_has_prefix(cmd, "scale:")) {
        double scale = g_ascii_strtod(cmd + 6, NULL);
        scale = CLAMP(scale, 0.2, 10.0);
        xs_host_api()->conf_set_dbl(xs_core_plugin_conf(p->name), p->name,
                                    "scale", scale);
        xs_core_plugin_conf_flush(p->name);
        /* Размер окна НЕ задаётся здесь жёстко (240*x — это канон часов,
         * у других плагинов свой): плагин сам подгонит окно под свой
         * канон на ближайшем tick (см. calendar: cal_fit_height), а draw
         * в любом случае масштабируется от фактических w/h окна. */
    } else if (g_str_has_prefix(cmd, "theme:")) {
        xs_host_api()->conf_set_str(xs_core_plugin_conf(p->name), p->name,
                                    "theme", cmd + 6);
        xs_core_plugin_conf_flush(p->name);
        xs_core_recreate_plugin(p);
    } else if (g_str_has_prefix(cmd, "op:")) {
        double opacity = g_ascii_strtod(cmd + 3, NULL);
        opacity = CLAMP(opacity, 0.1, 1.0);
        xs_host_api()->conf_set_dbl(xs_core_plugin_conf(p->name), p->name,
                                    "opacity", opacity);
        xs_core_plugin_conf_flush(p->name);
        xs_core_recreate_plugin(p);
    } else if (g_str_has_prefix(cmd, "win:")) {
        /* win:<flag>:<0|1> — живое применение, без recreate */
        const char *rest = cmd + 4;
        const char *colon = strchr(rest, ':');
        char what[16];
        size_t n;

        if (!colon || !state || state->freed)
            return;
        n = (size_t)(colon - rest);
        if (n == 0 || n >= sizeof(what))
            return;
        memcpy(what, rest, n);
        what[n] = '\0';
        xs_core_set_win_flag(state, what, colon[1] == '1');
        xs_core_apply_window_flags(state);
        xs_core_save_window_flags(p);
    } else if (strcmp(cmd, "properties") == 0) {
        xs_core_show_properties(p);
    } else if (strcmp(cmd, "about") == 0) {
        xs_core_show_about(p);
    } else if (strcmp(cmd, "lock:toggle") == 0) {
        if (state) {
            state->locked = !state->locked;
            xs_log_impl("%s position is now %s", p->name,
                        state->locked ? "LOCKED" : "unlocked");
        }
    } else if (g_str_has_prefix(cmd, "keepbelow:")) {
        gboolean keep_below = (cmd[10] == '1');
        g_key_file_set_boolean(xs_core_conf(), "global", "keep_below",
                               keep_below);
        xs_core_conf_flush();
        xs_log_impl("Keep below applies after Reload");
    } else if (strcmp(cmd, "reload") == 0) {
        xs_core_reload();
    } else if (g_str_has_prefix(cmd, "add:")) {
        /* add:<type> — новый инстанс типа (как "Add one more" в оригинале) */
        const char *type = cmd[4] ? cmd + 4 : xs_core_plugin_type(p);

        xs_core_add_instance(type);
    } else if (strcmp(cmd, "delete") == 0) {
        /* удалить этот инстанс ВМЕСТЕ с конфигом (окно уже уничтожится) */
        xs_core_delete_instance_full(p, TRUE);
    } else if (strcmp(cmd, "quit") == 0) {
        xs_core_shutdown_all();
        gtk_main_quit();
    }
}

/* ==== public plugin API ==== */

void xs_core_shutdown_all(void)
{
    if (g_conf && g_conf_path && g_conf_path[0] != '\0')
        xs_core_conf_flush();
    if (g_plugins) {
        /* Рамка во время shutdown сама удаляет и освобождает гостей.
         * Поэтому не обходим старый snapshot: повторный проход по нему
         * читал бы p->type у уже освобождённого гостя. Берём всегда
         * первый текущий элемент массива и удаляем его после полного
         * lifecycle; удалённые гости исчезают из массива сами. */
        while (g_plugins->len) {
            XsPlugin *p = g_ptr_array_index(g_plugins, 0);

            xs_core_shutdown_plugin(p);
            xs_core_unregister_plugin(p);
            xs_core_free_plugin(p);
        }
        g_ptr_array_free(g_plugins, TRUE);
        g_plugins = NULL;
    }
    if (g_conf) {
        g_key_file_free(g_conf);
        g_conf = NULL;
    }
    if (g_plugin_confs) {
        g_hash_table_destroy(g_plugin_confs);
        g_plugin_confs = NULL;
    }
    g_free(g_conf_path);
    g_conf_path = NULL;
}

GKeyFile *xs_core_conf(void)
{
    return g_conf;
}

const char *xs_core_conf_path(void)
{
    return g_conf_path ? g_conf_path : "";
}

void xs_core_register_plugin(XsPlugin *p)
{
    if (!p)
        return;
    if (!g_plugins)
        g_plugins = g_ptr_array_new();
    g_ptr_array_add(g_plugins, p);
}

void xs_core_unregister_plugin(XsPlugin *p)
{
    if (!g_plugins || !p)
        return;
    g_ptr_array_remove(g_plugins, p);
}

void xs_core_shutdown_plugin(XsPlugin *p)
{
    if (!p)
        return;
    xs_tray_remove_plugin(p);
    xs_core_save_plugin_position(p);
    if (p->ops && p->ops->shutdown)
        p->ops->shutdown(p);
    xs_core_cleanup_plugin_window(p);
}

/* Перезапуск одного инстанса в reload: skip если уже удалён из
 * g_plugins (рамка остановила гостя во время её shutdown). */
static gboolean plugin_alive(XsPlugin *p);

/* Полный перезапуск рамки: гасим живых гостей (конфиги сохраняем),
 * затем саму рамку; fl_init при re-init сам стартует гостей из
 * guests_N — все reparent/backdrop-процедуры отрабатывают штатно. */
static void am_reload_frame(XsPlugin *host)
{
    gsize i;
    XsWinState *st;

    if (!host || !host->win)
        return;
    st = g_object_get_data(G_OBJECT(host->win), "xs-state");
    /* живые гости: shutdown+free, конфиг сохраняется */
    for (i = 0; g_plugins && i < g_plugins->len; i++) {
        XsPlugin *g = g_ptr_array_index(g_plugins, i);

        if (!g || !g->win)
            continue;
        if (g_object_get_data(G_OBJECT(g->win),
                              "xs-guest-host") &&
            g_strcmp0(g_object_get_data(G_OBJECT(g->win),
                                        "xs-guest-host"),
                      host->name) == 0) {
            xs_core_delete_instance_full(g, FALSE);
            i = (gsize)-1; /* массив изменился — начать заново */
            continue;
        }
    }
    /* сама рамка */
    xs_tray_remove_plugin(host);
    if (host->ops && host->ops->shutdown)
        host->ops->shutdown(host);
    xs_core_cleanup_plugin_window(host);
    if (host->ops && host->ops->init &&
        host->ops->init(host,
                        xs_core_plugin_conf(host->name)) == 0 &&
        host->win) {
        xs_core_save_plugin_position(host);
    } else {
        xs_log_impl("reload: %s (frame) init failed", host->name);
    }
}

static void am_reload_one(XsPlugin *p, GPtrArray *snap)
{
    gsize idx = 0;

    (void)snap;
    /* рамка могла остановить гостя во время её shutdown */
    if (!plugin_alive(p))
        return;
    xs_tray_remove_plugin(p);
    if (p->ops && p->ops->shutdown)
        p->ops->shutdown(p);
    xs_core_cleanup_plugin_window(p);
    if (!p->ops || !p->ops->init ||
        p->ops->init(p, xs_core_plugin_conf(p->name)) != 0 ||
        !p->win) {
        xs_log_impl("reload: %s initialization failed", p->name);
        if (g_plugins && g_ptr_array_find(g_plugins, p, (guint *)&idx)) {
            g_ptr_array_remove_index(g_plugins, idx);
        }
        xs_core_free_plugin(p);
    } else {
        xs_core_save_plugin_position(p);
    }
}

void xs_core_reload(void)
{
    gsize i;

    if (g_conf_path && g_conf_path[0] != '\0') {
        GError *error = NULL;
        if (!g_key_file_load_from_file(g_conf, g_conf_path, G_KEY_FILE_NONE, &error)) {
            xs_log_impl("reload: failed to load config %s: %s", g_conf_path,
                        error ? error->message : "?");
            if (error)
                g_error_free(error);
        }
    }
    xs_plugin_confs_reload();

    if (!g_plugins)
        return;
    /* Снимок указателей + два прохода (гости, затем рамки):
     * fl_shutdown рамки удаляет гостей из g_plugins — итерация
     * по живому массиву давала use-after-free (BadWindow/падение
     * при «Restart Applets»). */
    {
        /* ТРИ списка, типы копируются ДО любых удалений: после
         * am_reload_frame указатели гостей в snap становятся
         * невалидными — читать p->type по ним нельзя (SIGSEGV). */
        GPtrArray *frames = g_ptr_array_sized_new(g_plugins->len);
        GPtrArray *plain = g_ptr_array_sized_new(g_plugins->len);

        for (i = 0; i < g_plugins->len; i++) {
            XsPlugin *p = g_ptr_array_index(g_plugins, i);

            if (!p)
                continue;
            /* гость рамки? (тип определяем ДО удалений) */
            if (p->win && G_IS_OBJECT(p->win) &&
                g_object_get_data(G_OBJECT(p->win),
                                  "xs-guest-host"))
                continue; /* жизненный цикл = рамка-хост */
            if (p->type &&
                strcmp(p->type, "frame_launcher") == 0)
                g_ptr_array_add(frames, p);
            else
                g_ptr_array_add(plain, p);
        }
        /* 1) корневые апплеты */
        for (i = 0; i < plain->len; i++)
            am_reload_one(g_ptr_array_index(plain, i), plain);
        /* 2) рамки: полным циклом, с гостями */
        for (i = 0; i < frames->len; i++)
            am_reload_frame(g_ptr_array_index(frames, i));
        g_ptr_array_free(plain, TRUE);
        g_ptr_array_free(frames, TRUE);
    }
    xs_tray_rebuild();
}

gboolean xs_core_recreate_plugin(XsPlugin *p)
{
    if (!p)
        return FALSE;
    xs_core_save_plugin_position(p);
    if (p->ops && p->ops->shutdown)
        p->ops->shutdown(p);
    xs_core_cleanup_plugin_window(p);
    if (!p->ops || !p->ops->init)
        return FALSE;
    if (p->ops->init(p, xs_core_plugin_conf(p->name)) != 0)
        return FALSE;
    xs_tray_remove_plugin(p);
    /* гость рамки в трей не попадает (started_by=plugin) */
    if (!(p->win && g_object_get_data(G_OBJECT(p->win),
                                      "xs-guest-host")))
        xs_tray_add_plugin(p);
    xs_tray_rebuild();
    return FALSE;
}

void xs_core_show_plugin(XsPlugin *p, gboolean visible)
{
    XsWinState *state;

    if (!p || !p->win)
        return;
    state = g_object_get_data(G_OBJECT(p->win), "xs-state");
    if (!state)
        return;
    gtk_widget_set_visible(state->win, visible);
    xs_tray_set_plugin_visibility(p, visible);
}

gsize xs_core_plugin_count(void)
{
    return g_plugins ? g_plugins->len : 0;
}

XsPlugin *xs_core_plugin_at(gsize index)
{
    if (!g_plugins || index >= g_plugins->len)
        return NULL;
    return g_ptr_array_index(g_plugins, index);
}

const char *xs_core_plugin_type(XsPlugin *p)
{
    return (p && p->type) ? p->type : (p ? p->name : NULL);
}

/* Первое свободное имя инстанса типа type: "type", "type-2", "type-3"... */
char *xs_core_next_instance_name(const char *type)
{
    gsize i;
    char uuid[9];
    char *cand;
    gboolean taken;
    int guard = 0;

    if (!type || !type[0])
        return NULL;
    /* Схема имён: тип-UUID8-user_label. user_label по умолчанию
     * blank_label (переопределяется в конфиге секции). */
    do {
        xs_short_uuid(uuid);
        cand = g_strdup_printf("%s-%s-blank_label", type, uuid);
        taken = FALSE;
        for (i = 0; g_plugins && i < g_plugins->len; i++) {
            XsPlugin *p = g_ptr_array_index(g_plugins, i);

            if (p && p->name && strcmp(p->name, cand) == 0) {
                taken = TRUE;
                break;
            }
        }
        if (!taken) {
            char *path = xs_plugin_conf_path(cand);

            taken = g_file_test(path, G_FILE_TEST_EXISTS);
            g_free(path);
        }
        if (taken)
            g_free(cand);
    } while (taken && ++guard < 64);
    if (taken) {
        g_free(cand);
        return NULL;
    }
    return cand;
}

/* Создать инстанс типа type. Модуль не перезагружаем: берём ops/метаданные
 * у существующего инстанса этого типа (gmodule держит модуль живым). */
int xs_core_add_instance(const char *type)
{
    /* Прототип не обязателен: xs_core_add_instance_for_host сам
     * умеет взять ops из загруженного модуля (.so), когда живых
     * инстансов типа нет (например, frame_launcher ещё не запущен). */
    if (!type || !type[0] || !g_plugins)
        return -1;
    return xs_core_add_instance_for_host(type, NULL) != NULL ? 0 : -1;
}

/* Слабая ссылка на загруженные модули (заполняет main.c): позволяет
 * common.c стартовать гостя типа, среди живых инстансов которого нет. */
static GPtrArray **g_loaded_modules_ref = NULL;

void xs_core_set_loaded_modules_ref(GPtrArray **ref)
{
    g_loaded_modules_ref = ref;
}
/* Создать инстанс типа type с именем iname (iname == NULL → UUID-имя).
 * Гости: xs_type пишется в конфиг, symlink в plugins_on НЕ создаётся
 * (гость включается хозяином). */
XsPlugin *xs_core_add_instance_for_host(const char *type,
                                        const char *iname)
{
    XsPlugin *proto = NULL;
    XsPlugin *p;
    char *name;
    GKeyFile *kf;
    gsize i;

    if (!type || !type[0] || !g_plugins)
        return NULL;
    for (i = 0; i < g_plugins->len; i++) {
        XsPlugin *q = g_ptr_array_index(g_plugins, i);

        if (q && xs_core_plugin_type(q) && strcmp(xs_core_plugin_type(q), type) == 0) {
            proto = q;
            break;
        }
    }
    /* Живого инстанса типа нет — берём ops из загруженного модуля. */
    if (!proto && g_loaded_modules_ref && *g_loaded_modules_ref) {
        GPtrArray *mods = *g_loaded_modules_ref;

        for (i = 0; i < mods->len; i++) {
            XsLoadedPlugin *lp = g_ptr_array_index(mods, i);

            if (lp && lp->desc && lp->desc->name &&
                strcmp(lp->desc->name, type) == 0) {
                static XsPluginOps ops_copy;
                static const char *st_name, *st_desc, *st_author,
                                  *st_version;

                ops_copy = *lp->desc->ops;
                st_name = lp->desc->name;
                st_desc = lp->desc->desc;
                st_author = lp->desc->author;
                st_version = lp->desc->version;
                proto = g_new0(XsPlugin, 1);
                proto->type = g_strdup(type);
                proto->name = st_name;
                proto->ops = &ops_copy;
                proto->desc = st_desc;
                proto->author = st_author;
                proto->version = st_version;
                proto->priv = NULL;
                break;
            }
        }
    }
    if (!proto || !proto->ops || !proto->ops->init) {
        xs_log_impl("add: no loaded plugin of type '%s'", type);
        return NULL;
    }
    if (iname && iname[0]) {
        name = g_strdup(iname);
    } else {
        name = xs_core_next_instance_name(type);
        if (!name)
            return NULL;
    }
    kf = xs_core_plugin_conf(name);
    /* xs_type: тип плагина в конфиге (нужен для запуска гостей). */
    g_key_file_set_string(kf, name, "xs_type", type);
    /* Новый инстанс создаётся незаблокированным: при lock=true ядро в
     * on_configure_event выходит по первому же return FALSE, окно
     * двигается (это делает WM), а state->x/y не обновляются и в
     * Properties, ни в конфиге — перетаскивание перестаёт работать,
     * причём выглядит как «перетаскивается, но координаты не меняются».
     *
     * В коде дефолт и так FALSE, и ключ раньше просто не писался, так
     * что поведение не меняется. Запись явная: значение видно в
     * конфиге и не поедет, если дефолт когда-нибудь поменяют. */
    g_key_file_set_boolean(kf, name, "lock", FALSE);
    /* Новый инстанс через Launch Applet трея — main_daemon (гость,
     * созданный рамкой, проходит через start_guest_new и здесь
     * получает started_by=plugin, чтобы не попасть в трей/автостарт). */
    g_key_file_set_string(kf, name, "started_by",
                          iname && iname[0] ? "plugin" : "main_daemon");
    /* Новый инстанс наследует вид от секции типа (theme/scale/opacity),
     * если у него ещё нет своих значений. */
    /* наследование вида от прототипа УБРАНО для scale: у каждого
     * лаунчера свой масштаб, перетирать конфиг гостя значением
     * прототипа нельзя (гость создан со своим scale). Наследуем
     * только theme и opacity. */
    if (proto->type && strcmp(proto->type, proto->name) != 0) {
        GKeyFile *pkf = xs_core_plugin_conf(proto->name);
        static const char *inherit[] = { "theme", "opacity" };
        size_t k;

        for (k = 0; k < G_N_ELEMENTS(inherit); k++) {
            char *v = conf_str(pkf, proto->name, inherit[k], NULL);

            /* только если у инстанса ещё НЕТ своего значения:
             * при рестарте гостя конфиг существует — свой theme
             * перетирать нельзя (баг «тема гостя = тема родителя») */
            if (v && !g_key_file_has_key(kf, name, inherit[k], NULL)) {
                g_key_file_set_string(kf, name, inherit[k], v);
                g_free(v);
            } else
                g_free(v);
        }
    }
    p = g_new0(XsPlugin, 1);
    p->name = name;
    p->type = g_strdup(type);
    p->host = xs_host_api();
    p->ops = proto->ops;
    p->desc = proto->desc;
    p->author = proto->author;
    p->version = proto->version;
    p->priv = NULL;
    if (p->ops->init(p, kf) != 0 || !p->win) {
        xs_log_impl("add: init failed for instance '%s'", p->name);
        xs_core_free_plugin(p);
        return NULL;
    }
    xs_core_register_plugin(p);
    /* гость рамки не попадает в трей: его запуск управляется хозяином */
    xs_tray_add_plugin_wrapper(p);
    xs_tray_rebuild();
    xs_core_plugin_conf_flush(p->name);
    if (!iname || !iname[0]) {
        /* Обычный инстанс (Launch Applet): symlink в plugins_on. */
        xs_onoff_relink(NULL, p->name);
        xs_core_save_instances();
    }
    xs_log_impl("added instance %s (type %s)", p->name, type);
    return p;
}

/* Диалог «мёртвый symlink»: удалить конфиг или выбрать другой.
 * Возвращает: 1 = удалить конфиг, 0 = указан новый конфиг (new_name
 * заполнен), -1 = отменено/нет GTK. new_name: g_free() обязан вызвать
 * вызывающий, если вернулся 0. */
int xs_dead_link_dialog(GtkWindow *parent, const char *linkname,
                        char **new_name)
{
    GtkWidget *dlg;
    int res = -1;

    if (!new_name)
        return -1;
    *new_name = NULL;
    dlg = gtk_message_dialog_new(parent, GTK_DIALOG_MODAL,
                                 GTK_MESSAGE_QUESTION, GTK_BUTTONS_NONE,
                                 _("Dead symlink: %s\n\n"
                                   "Config not found. Delete the symlink?"),
                                 linkname);
    gtk_dialog_add_buttons(GTK_DIALOG(dlg),
                           _("Delete"), 1,
                           _("Point to another config"), 2,
                           _("Cancel"), 3,
                           NULL);
    res = gtk_dialog_run(GTK_DIALOG(dlg));
    if (res == 2) {
        GtkWidget *fc = gtk_file_chooser_dialog_new(
            _("Choose a config in .plugins"), parent,
            GTK_FILE_CHOOSER_ACTION_OPEN, _("Cancel"), GTK_RESPONSE_CANCEL,
            _("Select"), GTK_RESPONSE_ACCEPT, NULL);
        char *dotdir = g_path_get_dirname(g_plugin_conf_dir);
        char *sub = g_build_filename(dotdir, ".plugins", NULL);

        gtk_file_chooser_set_current_folder(GTK_FILE_CHOOSER(fc), sub);
        gtk_file_chooser_set_select_multiple(GTK_FILE_CHOOSER(fc), FALSE);
        g_free(dotdir);
        g_free(sub);
        if (gtk_dialog_run(GTK_DIALOG(fc)) == GTK_RESPONSE_ACCEPT) {
            char *picked = gtk_file_chooser_get_filename(
                GTK_FILE_CHOOSER(fc));

            if (picked) {
                *new_name = g_path_get_basename(picked);
                if (g_str_has_suffix(*new_name, ".conf")) {
                    char *t = *new_name;

                    *new_name = g_strndup(t, strlen(t) - 5);
                    g_free(t);
                }
                g_free(picked);
                res = 0;
            }
        }
        gtk_widget_destroy(fc);
    }
    gtk_widget_destroy(dlg);
    while (gtk_events_pending())
        gtk_main_iteration_do(FALSE);
    return res;
}


/* Удалить гостевую запись name из guests_-списка всех frame_launcher
 * конфигов (сдвиг хвоста, чтобы список оставался плотным). */
void xs_core_remove_guest_entry(const char *name)
{
    GDir *dir;

    if (!name || !name[0] || !g_plugin_conf_dir)
        return;
    dir = g_dir_open(g_plugin_conf_dir, 0, NULL);
    if (!dir)
        return;
    for (;;) {
        const char *fn = g_dir_read_name(dir);
        char *path, *inst;
        GKeyFile *kf;
        gboolean cache;
        gsize i;
        int removed = 0;

        if (!fn)
            break;
        if (!g_str_has_suffix(fn, ".conf") ||
            !g_str_has_prefix(fn, "frame_launcher-"))
            continue;
        inst = g_strndup(fn, strlen(fn) - 5); /* имя без .conf */
        path = g_build_filename(g_plugin_conf_dir, fn, NULL);
        /* ПРАВИМ КЭШ-запись (источник flush'ей), если она есть —
         * правка только файла перетиралась следующим flush
         * родителя, и гостя «воскрешало» в списке. */
        cache = g_plugin_confs &&
                g_hash_table_contains(g_plugin_confs, inst);
        if (cache) {
            kf = g_hash_table_lookup(g_plugin_confs, inst);
        } else {
            kf = g_key_file_new();
            if (!g_key_file_load_from_file(kf, path,
                                           G_KEY_FILE_NONE, NULL)) {
                g_key_file_free(kf);
                g_free(path);
                g_free(inst);
                continue;
            }
        }
        {
            gchar **groups = g_key_file_get_groups(kf, NULL);
            const char *host = groups && groups[0] ? groups[0] : NULL;

            if (host) {
                for (i = 1; i <= 64; i++) {
                    char *k = g_strdup_printf("guests_%d", (int)i);
                    char *v = g_key_file_get_string(kf, host, k,
                                                    NULL);

                    if (v && v[0]) {
                        if (strcmp(v, name) == 0) {
                            int j;

                            /* сдвиг хвоста: guests_i = guests_i+1 */
                            for (j = i; j < 64; j++) {
                                char *kj =
                                    g_strdup_printf("guests_%d", j);
                                char *kj1 = g_strdup_printf(
                                    "guests_%d", j + 1);
                                char *v1 = g_key_file_get_string(
                                    kf, host, kj1, NULL);

                                if (v1 && v1[0]) {
                                    g_key_file_set_string(kf, host,
                                                          kj, v1);
                                    g_free(v1);
                                } else {
                                    g_key_file_remove_key(kf, host,
                                                          kj, NULL);
                                }
                                g_free(kj);
                                g_free(kj1);
                                if (!v1 || !v1[0])
                                    break;
                            }
                            removed++;
                            i--; /* тот же слот может содержать
                                    имя ещё раз */
                        }
                        g_free(v);
                    } else {
                        break;
                    }
                    g_free(k);
                }
                if (removed) {
                    if (cache)
                        xs_core_plugin_conf_flush(inst);
                    else
                        g_key_file_save_to_file(kf, path, NULL);
                    xs_log_impl("guest entry '%s' removed from %s",
                                name, fn);
                }
            }
            g_strfreev(groups);
        }
        if (!cache)
            g_key_file_free(kf);
        g_free(path);
        g_free(inst);
    }
    g_dir_close(dir);
}

void xs_core_delete_instance_full(XsPlugin *p, gboolean delete_conf)
{
    char *name_copy;
    char *host_name_copy = NULL;

    if (!p)
        return;
    xs_log_impl("deleting instance %s", p->name);
    name_copy = g_strdup(p->name);
    /* имя хоста (если удаляемый — гость) читаем ДО shutdown:
     * gtk_widget_destroy снимает все data с окна */
    {
        char *h = NULL;

        if (p->win && G_IS_OBJECT(p->win))
            h = g_strdup(g_object_get_data(G_OBJECT(p->win),
                                           "xs-guest-host"));
        host_name_copy = h;
    }
    xs_core_shutdown_plugin(p);
    xs_core_unregister_plugin(p);
    xs_core_free_plugin(p);
    xs_tray_rebuild();
    /* Удалить symlink из plugins_on */
    if (g_plugin_onoff_dir) {
        char *link = g_build_filename(g_plugin_onoff_dir, name_copy, NULL);

        unlink(link);
        g_free(link);
    }
    /* Явное «Delete this» — удалить и конфиг (иначе после рестарта
     * инстанс «воскресает»). Временная остановка (галочка frame,
     * рестарт-кнопка) конфиг сохраняет. */
    if (delete_conf) {
        char *path = xs_plugin_conf_path(name_copy);

        if (unlink(path) != 0 && errno != ENOENT)
            xs_log_impl("cannot delete config %s: %s", path,
                        g_strerror(errno));
        else
            xs_log_impl("config deleted: %s", path);
        g_free(path);
        if (g_plugin_confs)
            g_hash_table_remove(g_plugin_confs, name_copy);
        /* вычистить имя из guests_N всех frame-конфигов */
        xs_core_remove_guest_entry(name_copy);
        /* уведомить frame-хоста (если удалённый был его гостем):
         * он перечитает список и обновит Properties */
        if (host_name_copy && host_name_copy[0]) {
            XsPlugin *host = find_plugin_by_name(host_name_copy);

            if (host && host->ops && host->ops->guest_list_changed)
                host->ops->guest_list_changed(host);
        }
    }
    g_free(host_name_copy);
    g_free(name_copy);
    xs_core_save_instances();
}

/* Слабая реализация: список инстансов поддерживает демон (main.c);
 * standalone-сборки (xclock) не сохраняют список. */
__attribute__((weak)) void xs_core_save_instances(void)
{
}

static void
xs_core_save_plugin_position(XsPlugin *p)
{
    XsWinState *state;

    if (!p || !p->win)
        return;
    /* Гость рамки: его «настольную» позицию не сохраняем. */
    if (g_object_get_data(G_OBJECT(p->win), "xs-guest-host"))
        return;
    state = g_object_get_data(G_OBJECT(p->win), "xs-state");
    if (!state)
        return;

    g_key_file_set_integer(xs_core_plugin_conf(p->name), p->name, "x",
                           state->x);
    g_key_file_set_integer(xs_core_plugin_conf(p->name), p->name, "y",
                           state->y);
    xs_core_plugin_conf_flush(p->name);
}

/* ==== end public API ==== */

static cairo_surface_t *capture_frame(cairo_t *cr, int width, int height)
{
    cairo_surface_t *source;
    cairo_surface_t *image;
    cairo_t *copy;

    if (width <= 0 || height <= 0)
        return NULL;
    source = cairo_get_target(cr);
    if (cairo_surface_status(source) != CAIRO_STATUS_SUCCESS)
        return NULL;
    image = cairo_image_surface_create(CAIRO_FORMAT_ARGB32, width, height);
    if (cairo_surface_status(image) != CAIRO_STATUS_SUCCESS) {
        cairo_surface_destroy(image);
        return NULL;
    }
    copy = cairo_create(image);
    if (!copy) {
        cairo_surface_destroy(image);
        return NULL;
    }
    cairo_set_operator(copy, CAIRO_OPERATOR_SOURCE);
    cairo_set_source_surface(copy, source, 0, 0);
    cairo_paint(copy);
    cairo_destroy(copy);
    if (cairo_surface_status(image) != CAIRO_STATUS_SUCCESS) {
        cairo_surface_destroy(image);
        return NULL;
    }
    return image;
}

static void reset_shape_schedule(XsWinState *state)
{
    if (state->shape_idle_id) {
        g_source_remove(state->shape_idle_id);
        state->shape_idle_id = 0;
    }
    if (state->shape_retry_id) {
        g_source_remove(state->shape_retry_id);
        state->shape_retry_id = 0;
    }
    state->shape_pending = TRUE;
    state->shape_idle_id = g_idle_add((GSourceFunc)xs_core_input_shape_idle,
                                      state->plug);
}

static gboolean xs_core_input_shape_idle(XsPlugin *p)
{
    XsWinState *state;
    cairo_surface_t *surface;
    cairo_region_t *region;
    GdkWindow *window;
    const unsigned char *pixels;
    int width, height, stride;
    guint64 now;
    int y, x;

    if (!p)
        return G_SOURCE_REMOVE;
    state = g_object_get_data(G_OBJECT(p->win), "xs-state");
    if (!state || state->freed)
        return G_SOURCE_REMOVE;
    state->shape_idle_id = 0;
    if (!state->shape_pending)
        return G_SOURCE_REMOVE;

    now = g_get_monotonic_time();
    if (now - state->last_shape_us < 250 * 1000) {
        state->shape_retry_id = g_timeout_add(250 * 1000,
                                             (GSourceFunc)xs_core_input_shape_idle,
                                             p);
        return G_SOURCE_REMOVE;
    }

    surface = state->frame;
    if (!surface || cairo_surface_status(surface) != CAIRO_STATUS_SUCCESS) {
        state->shape_pending = FALSE;
        return G_SOURCE_REMOVE;
    }
    width = cairo_image_surface_get_width(surface);
    height = cairo_image_surface_get_height(surface);
    stride = cairo_image_surface_get_stride(surface);
    pixels = cairo_image_surface_get_data(surface);
    if (!pixels || width <= 0 || height <= 0) {
        state->shape_pending = FALSE;
        return G_SOURCE_REMOVE;
    }

    region = cairo_region_create();
    if (!region) {
        state->shape_pending = FALSE;
        return G_SOURCE_REMOVE;
    }
    /* frame_launcher: input = ВСЁ окно. У nobg-тем внутренность
     * прозрачна (alpha=0) — пиксельный регион оставлял бы только
     * рамку, и ПКМ «пробивал» прозрачную зону до рабочего стола. */
    if (p->type && strcmp(p->type, "frame_launcher") == 0) {
        cairo_rectangle_int_t full;

        full.x = 0;
        full.y = 0;
        full.width = width;
        full.height = height;
        cairo_region_union_rectangle(region, &full);
        window = gtk_widget_get_window(state->win);
        if (window)
            gdk_window_input_shape_combine_region(window, region,
                                                  0, 0);
        cairo_region_destroy(region);
        state->last_shape_us = now;
        state->shape_pending = FALSE;
        return G_SOURCE_REMOVE;
    }
    /* Строчное построение региона: один прямоугольник на строку непрозрачности */
    for (y = 0; y < height; y++) {
        const unsigned char *row = pixels + (gsize)y * (gsize)stride;
        int run_start = -1;
        for (x = 0; x <= width; x++) {
            int opaque = (x < width) && (row[(gsize)x * 4U + 3] > 8);
            if (opaque && run_start < 0) {
                run_start = x;
            } else if (!opaque && run_start >= 0) {
                cairo_rectangle_int_t rect;
                rect.x = run_start;
                rect.y = y;
                rect.width = x - run_start;
                rect.height = 1;
                cairo_region_union_rectangle(region, &rect);
                run_start = -1;
            }
        }
    }
    window = gtk_widget_get_window(state->win);
    if (window)
        gdk_window_input_shape_combine_region(window, region, 0, 0);
    cairo_region_destroy(region);
    state->last_shape_us = now;
    state->shape_pending = FALSE;
    return G_SOURCE_REMOVE;
}

static gboolean xs_core_tick_cb(XsPlugin *p)
{
    XsWinState *state;

    if (!p)
        return G_SOURCE_REMOVE;
    state = p->win ? g_object_get_data(G_OBJECT(p->win), "xs-state") : NULL;
    if (!state || state->freed)
        return G_SOURCE_REMOVE;
    state->frame_dirty = TRUE;
    gtk_widget_queue_draw(state->area);
    if (p->ops && p->ops->tick) {
        guint interval = p->ops->tick(p);
        if (interval > 0 && interval != state->tick_ms)
            set_tick(p, interval);
    }
    {
        gint64 now = g_get_monotonic_time() / 1000;
        if (now - state->last_tick_log_us >= 5000000) {
            if (g_debug)
                xs_log_impl("tick: widgets=%zu tick_ms=%u",
                            xs_core_plugin_count(),
                            state ? state->tick_ms : 0);
            state->last_tick_log_us = now;
        }
    }
    return G_SOURCE_CONTINUE;
}

/* Элемент темы: RsvgHandle (*.svg) или GdkPixbuf (*.png) */
typedef struct {
	gboolean is_svg;
	union {
		RsvgHandle *svg;
		GdkPixbuf *png;
	} u;
} XsThemeItem;

static void xs_theme_item_free(gpointer v)
{
	XsThemeItem *it = v;

	if (!it)
		return;
	if (it->is_svg) {
		if (it->u.svg)
			g_object_unref(it->u.svg);
	} else {
		if (it->u.png)
			g_object_unref(it->u.png);
	}
	g_free(it);
}

/* Каталог системных тем. Хранится здесь, а не в main.c, потому что
 * common.o линкуется и в xclock, где main.c нет. Задаётся демоном при
 * старте: из --themedir либо из XS_THEME_DIR, зашитой при сборке. */
static char *g_themedir = NULL;

void xs_core_set_themedir(const char *dir)
{
    g_free(g_themedir);
    g_themedir = (dir && dir[0]) ? g_strdup(dir) : NULL;
}

const char *xs_core_themedir(void)
{
    return g_themedir;
}

/* Поиск каталога темы с приоритетом пользователь -> система.
 *
 * Раньше каждый апплет искал тему по-своему, и все пять способов
 * различались: dirname(plugdir)/themes у clearrss, $HOME/lib/.../plugins/
 * clearweather_theme у clearweather, $XDG_CONFIG_HOME и $HOME/.xscreenlets
 * у clock и calendar, "FrameLauncher"/themes у frame_launcher. При переносе
 * плагинов в /usr/libexec/xscreenlets половина этих путей перестала бы
 * существовать, причём у clearrss dirname дал бы /usr/libexec.
 *
 * Теперь порядок один для всех:
 *   1. $XDG_CONFIG_HOME/xscreenlets/themes/<plugin>/<theme>
 *   2. $HOME/.xscreenlets/themes/<plugin>/<theme>      (legacy)
 *   3. <XS_THEME_DIR>/<plugin>/<theme>                (система, -D/--themedir)
 *
 * Пользовательские темы первыми намеренно: установленный пакет не должен
 * перекрывать настройку конкретного пользователя. */
/* Имена каталогов тем родных апплетов screenlets отличаются от типов
 * наших плагинов: тип clock, а каталог /usr/share/screenlets/Clock.
 * Без этого сопоставления темы, установленные вместе с оригинальным
 * screenlets, перестают находиться после перехода на xs_core_find_theme.
 * Запись отсутствует, если у апплета нет темы в старой раскладке. */
static const char *xs_legacy_theme_dir(const char *plugin)
{
    static const struct { const char *type, *dir; } map[] = {
        { "clock",         "Clock"         },
        { "calendar",      "ClearCalendar" },
        { "clearrss",      "ClearRss"      },
        { "clearweather",  "ClearWeather"  },
    };
    gsize i;

    if (!plugin)
        return NULL;
    for (i = 0; i < G_N_ELEMENTS(map); i++)
        if (g_strcmp0(map[i].type, plugin) == 0)
            return map[i].dir;
    return NULL;
}

char *xs_core_find_theme(const char *plugin, const char *theme)
{
    char *user_xdg, *user_legacy, *system, *out;
    const char *td = xs_core_themedir();

    if (!plugin || !plugin[0])
        return NULL;
    if (!theme || !theme[0])
        theme = "default";

    /* ВАЖНО: out обязан быть инициализирован сразу. Если системный каталог
     * не задан (XS_THEME_DIR пуст и --themedir не передан) и пользовательских
     * тем нет, ни одна из веток ниже не выполнится - а без инициализации
     * последующая проверка if (!out) прочитала бы мусор, и g_free() на
     * мусоре роняет демон с "free(): invalid pointer". */
    out = NULL;

    user_xdg = g_build_filename(g_get_user_config_dir(), "xscreenlets",
                                "themes", plugin, theme, NULL);
    user_legacy = g_build_filename(g_get_home_dir(), ".xscreenlets",
                                   "themes", plugin, theme, NULL);
    /* Системный каталог может быть не задан (обычная сборка из дерева).
     * Тогда последний шаг пропускается и остаются пользовательские. */
    system = (td && td[0])
                 ? g_build_filename(td, plugin, theme, NULL)
                 : NULL;

    if (g_file_test(user_xdg, G_FILE_TEST_IS_DIR))
        out = user_xdg;
    else if (g_file_test(user_legacy, G_FILE_TEST_IS_DIR))
        out = user_legacy;
    else if (system) {
        /* Системный каталог задан, но конкретной темы в нём может не
         * оказаться: у пакета не все темы, а у пользователя своя. */
        out = g_file_test(system, G_FILE_TEST_IS_DIR) ? system : NULL;
    }

    /* Темы, установленные рядом с плагинами: <каталог плагинов>/../themes/
     * <плагин>/<тема>. Для ~/lib/xscreenlets/plugins это даёт
     * ~/lib/xscreenlets/themes/clearrss/default - то самое место, куда
     * make install кладёт темы. Это пользовательский каталог, поэтому он
     * проверяется раньше каталогов системы. */
    if (!out) {
        const char *pd = xs_core_plugdir();
        if (pd && pd[0]) {
            /* g_path_get_dirname() возвращает НОВУЮ строку - её нужно
             * освободить, иначе утечка на каждый вызов (проверено ASan:
             * 37 байт на вызов). */
            char *parent = g_path_get_dirname(pd);
            char *near = g_build_filename(parent, "themes", plugin, theme, NULL);
            g_free(parent);
            if (g_file_test(near, G_FILE_TEST_IS_DIR))
                out = near;
            else
                g_free(near);
        }
    }

    /* Последний шанс: тема родного апплета screenlets, если она стоит в
     * системе. Имена каталогов там с заглавной буквы, поэтому требуется
     * сопоставление с типом плагина. */
    if (!out) {
        const char *lname = xs_legacy_theme_dir(plugin);
        if (lname) {
            char *old_sys = g_build_filename("/usr/share/screenlets",
                                             lname, "themes", theme, NULL);
            if (g_file_test(old_sys, G_FILE_TEST_IS_DIR))
                out = old_sys;
            else
                g_free(old_sys);
        }
    }

    if (!out) {
        /* Ничего не нашлось: отдаём XDG-путь, чтобы theme_load() внятно
         * сказал, чего не хватает, вместо молчаливого NULL. */
        out = user_xdg;
    }

    g_free(user_legacy);
    /* освобождаем только те кандидаты, которые не стали ответом */
    if (out != user_xdg)
        g_free(user_xdg);
    if (system && out != system)
        g_free(system);
    return out;
}

static gboolean theme_load(XsPlugin *p, const char *dir)
{
    XsWinState *state;
    GDir *directory;
    const char *filename;
    XsTheme *theme;

    if (!p || !dir)
        return FALSE;
    /* окно может быть ещё не создано (плагин грузит тему в init до
     * make_window) — тему кэшируем без state */
    if (!p->win || !G_IS_OBJECT(p->win))
        return FALSE;
    state = g_object_get_data(G_OBJECT(p->win), "xs-state");
    if (!state)
        return FALSE;
    if (state->theme) {
        xs_theme_free(state->theme);
        state->theme = NULL;
    }
    directory = g_dir_open(dir, 0, NULL);
    if (!directory)
        return FALSE;
    theme = g_new0(XsTheme, 1);
    theme->dir = g_strdup(dir);
    theme->svgs = g_hash_table_new_full(g_str_hash, g_str_equal, g_free,
                                        xs_theme_item_free);
    while ((filename = g_dir_read_name(directory))) {
        gboolean is_svg = g_str_has_suffix(filename, ".svg");
        gboolean is_png = g_str_has_suffix(filename, ".png");

        if (!is_svg && !is_png)
            continue;
        char *path = g_build_filename(dir, filename, NULL);
        XsThemeItem *it = g_new0(XsThemeItem, 1);

        if (is_svg) {
            it->is_svg = TRUE;
            it->u.svg = rsvg_handle_new_from_file(path, NULL);
        } else {
            it->is_svg = FALSE;
            it->u.png = gdk_pixbuf_new_from_file(path, NULL);
        }
        if (it->u.svg || it->u.png) {
            char *key = g_strdup(filename);
            char *dot = strrchr(key, '.');
            if (dot)
                *dot = '\0';
            g_hash_table_replace(theme->svgs, key, it);
        } else {
            g_free(it);
        }
        g_free(path);
    }
    g_dir_close(directory);
    if (g_hash_table_size(theme->svgs) == 0) {
        xs_theme_free(theme);
        return FALSE;
    }
    state->theme = theme;
    return TRUE;
}

/* Взять элемент темы (svg или png) */
static XsThemeItem *theme_item(XsPlugin *p, const char *element)
{
    XsWinState *state;

    if (!p || !element || !p->win)
        return NULL;
    state = g_object_get_data(G_OBJECT(p->win), "xs-state");
    if (!state || !state->theme)
        return NULL;
    return g_hash_table_lookup(state->theme->svgs, element);
}

gboolean xs_core_theme_has(XsPlugin *p, const char *element)
{
    return theme_item(p, element) != NULL;
}

static void theme_draw_full(XsPlugin *p, cairo_t *cr, const char *element,
                            double x, double y, double width, double height)
{
    XsThemeItem *it;
    RsvgRectangle viewport;
    GdkRectangle png_rect;
    double nw = 0.0, nh = 0.0;

    if (!p || !cr || !element || width <= 0.0 || height <= 0.0)
        return;
    /* A/B-замер для охоты за ростом RSS: XSCREENLETS_NO_SVG=1 полностью
     * отключает рендер SVG. Если при выключенном рендере RSS всё равно
     * растёт с той же скоростью, значит рендер НЕ является причиной и
     * кэшировать поверхности бессмысленно.
     *
     * Обратимо и отрисовку не портит: SVG просто не рисуется. Переменная
     * читается на каждый вызов - getenv дешёвый, зато переключать режим
     * можно без перезапуска демона, а для A/B это удобно. */
    if (g_getenv("XSCREENLETS_NO_SVG") != NULL)
        return;
    it = theme_item(p, element);
    if (!it)
        return;
    cairo_save(cr);
    if (it->is_svg) {
        gboolean has_w = FALSE, has_h = FALSE;
        RsvgLength w = {0, RSVG_UNIT_PX}, h = {0, RSVG_UNIT_PX};

        rsvg_handle_get_intrinsic_dimensions(it->u.svg, &has_w, &w,
                                             &has_h, &h, NULL, NULL);
        if (has_w && w.unit == RSVG_UNIT_PX)
            nw = w.length;
        if (has_h && h.unit == RSVG_UNIT_PX)
            nh = h.length;
        if (nw <= 0.0 || nh <= 0.0) {
            cairo_restore(cr);
            return;
        }
        /* Растяжка БЕЗ preserveAspectRatio: масштаб по осям отдельно,
         * viewport = натуральный размер SVG (letterbox исключён).
         * x/y применяем переводом до scale. */
        cairo_translate(cr, x, y);
        cairo_scale(cr, width / nw, height / nh);
        viewport.x = 0;
        viewport.y = 0;
        viewport.width = nw;
        viewport.height = nh;
        rsvg_handle_render_document(it->u.svg, cr, &viewport, NULL);
    } else {
        if (!it->u.png)
            return;
        png_rect.x = (int)x;
        png_rect.y = (int)y;
        png_rect.width = (int)width;
        png_rect.height = (int)height;
        gdk_cairo_set_source_pixbuf(cr, it->u.png, png_rect.x,
                                    png_rect.y);
        cairo_rectangle(cr, png_rect.x, png_rect.y, png_rect.width,
                        png_rect.height);
        cairo_fill(cr);
    }
    cairo_restore(cr);
}

static void theme_draw_native(XsPlugin *p, cairo_t *cr, const char *element,
                              double x, double y)
{
    XsThemeItem *it;
    double nw = 0.0, nh = 0.0;

    if (!p || !cr || !element)
        return;
    it = theme_item(p, element);
    if (!it)
        return;
    if (it->is_svg) {
        gboolean has_w = FALSE, has_h = FALSE;
        RsvgLength w = {0, RSVG_UNIT_PX}, h = {0, RSVG_UNIT_PX};

        rsvg_handle_get_intrinsic_dimensions(it->u.svg, &has_w, &w,
                                             &has_h, &h, NULL, NULL);
        if (has_w && w.unit == RSVG_UNIT_PX)
            nw = w.length;
        if (has_h && h.unit == RSVG_UNIT_PX)
            nh = h.length;
    } else {
        if (!it->u.png)
            return;
        nw = gdk_pixbuf_get_width(it->u.png);
        nh = gdk_pixbuf_get_height(it->u.png);
    }
    if (nw <= 0.0 || nh <= 0.0)
        return;
    theme_draw_full(p, cr, element, x, y, nw, nh);
}

static void theme_draw(XsPlugin *p, cairo_t *cr, const char *element,
                       double x, double y, double width)
{
    theme_draw_full(p, cr, element, x, y, width, width);
}

/* Called when the window is moved/resized by the WM */
static gboolean
on_configure_event(GtkWidget *window, GdkEventConfigure *event, gpointer data)
{
    XsPlugin *p = data;
    XsWinState *state;
    int x, y;

    (void)event; /* unused */

    if (!p || !p->win)
        return FALSE;
    state = g_object_get_data(G_OBJECT(p->win), "xs-state");
    if (!state)
        return FALSE;

    /* Если позиция заблокирована, не обновлять конфиг */
    if (state->locked)
        return FALSE;

    /* Гость внутри frame_launcher: координаты относительно рамки не
     * пишем в конфиг; при драге КЛАМПИМ гостя в content-зону хозяина
     * (гость не может вылезти за рамку) и обновляем снимок фона. */
    if (g_object_get_data(G_OBJECT(window), "xs-guest-host")) {
        const char *host_name = g_object_get_data(G_OBJECT(window),
                                                  "xs-guest-host");
        XsPlugin *host = find_plugin_by_name(host_name);
        XsWinState *hs;
        gint *xy;
        int cx, cy, cw, ch, nx, ny, gw, gh;

        /* первый configure после репарента несёт устаревшие
         * (экранные) координаты — игнорируем, позицию уже задал
         * gtk_window_move в host-ветке */
        if (g_object_get_data(G_OBJECT(window), "xs-guest-first-cfg")) {
            g_object_set_data(G_OBJECT(window), "xs-guest-first-cfg",
                              NULL);
            return FALSE;
        }
        if (!host || !host->win)
            return FALSE;
        hs = g_object_get_data(G_OBJECT(host->win), "xs-state");
        if (!hs)
            return FALSE;
        xy = g_object_get_data(G_OBJECT(host->win), "xs-content-xy");
        cx = xy ? xy[0] : 0;
        cy = xy ? xy[1] : 0;
        /* content-зона в координатах родителя */
        cw = hs->w - cx * 2;
        ch = hs->h - cy * 2;
        /* Позиция гостя известна ТОЛЬКО нам (state->x/y): child-окно
         * двигается исключительно нашими move (репарент/спиннеры).
         * Любое чтение позиции из GDK после репарента ненадёжно
         * (возвращает экранные координаты) — поэтому здесь НИЧЕГО
         * не читаем и не клампим. configure от наших move просто
         * обновляет снимок фона. */
        (void)x; (void)y;
        {
            GuestBgCtx *c = guest_bg_ctx_new(host, p);

            g_timeout_add(300, host_update_guest_backdrop_idle, c);
        }
        return FALSE;
    }

    /* Заморозка на время правки User label: configure-event не пишет
     * x/y в конфиг (rename может двигать окно через WM). Снимается в
     * xs_prop_label_rename_cb после завершения переименования. */
    if (g_props_label_freeze)
        return FALSE;
    gtk_window_get_position(GTK_WINDOW(window), &x, &y);
    state->x = x;
    state->y = y;

    /* Properties открыт: обновить поля X/Y (debounce 500 мс после
     * прекращения перемещения). */
    xs_core_props_pos_schedule(p);

    /* Debounce: save config at most once every 2 seconds */
    if (g_get_monotonic_time() - state->last_conf_save_us >= 2000000) {
        g_key_file_set_integer(xs_core_plugin_conf(p->name), p->name, "x",
                               state->x);
        g_key_file_set_integer(xs_core_plugin_conf(p->name), p->name, "y",
                               state->y);
        xs_core_plugin_conf_flush(p->name);
        state->last_conf_save_us = g_get_monotonic_time();
    }
    return FALSE;
}

static gboolean on_draw_frame(GtkWidget *area, cairo_t *cr, gpointer data)
{
    XsPlugin *p = data;
    XsWinState *state;
    cairo_surface_t *frame;
    cairo_surface_t *tmp;
    cairo_t *tcr;
    int width, height;

    if (!p || !p->win)
        return FALSE;
    state = g_object_get_data(G_OBJECT(p->win), "xs-state");
    if (!state || state->freed)
        return FALSE;
    width = gtk_widget_get_allocated_width(area);
    height = gtk_widget_get_allocated_height(area);
    if (g_debug)
        xs_log_impl("draw: %dx%d op=%.2f", width, height, state->opacity);
    cairo_set_operator(cr, CAIRO_OPERATOR_CLEAR);
    cairo_paint(cr);
    cairo_set_operator(cr, CAIRO_OPERATOR_OVER);

    if (state->opacity < 1.0 && state->opacity > 0.0) {
        /* Полупрозрачность рендером: кадр собирается на промежуточной
         * поверхности, на окно наносится paint_with_alpha — WM видит окно
         * без _NET_WM_WINDOW_OPACITY, Alt+drag сохраняется. */
        tmp = cairo_image_surface_create(CAIRO_FORMAT_ARGB32, width, height);
        tcr = cairo_create(tmp);
        cairo_set_operator(tcr, CAIRO_OPERATOR_CLEAR);
        cairo_paint(tcr);
        cairo_set_operator(tcr, CAIRO_OPERATOR_OVER);
        if (p->ops && p->ops->draw)
            p->ops->draw(p, tcr, width, height);
        cairo_set_source_surface(cr, tmp, 0, 0);
        cairo_paint_with_alpha(cr, state->opacity);
        cairo_destroy(tcr);
        cairo_surface_destroy(tmp);
    } else if (p->ops && p->ops->draw) {
        p->ops->draw(p, cr, width, height);
    }

    if (state->frame_dirty) {
        frame = capture_frame(cr, width, height);
        if (state->frame)
            cairo_surface_destroy(state->frame);
        state->frame = frame;
        state->w = width;
        state->h = height;
        state->frame_dirty = FALSE;
        reset_shape_schedule(state);
    }
    return FALSE;
}

static void on_window_destroy(GtkWidget *widget, gpointer data)
{
    XsPlugin *p = data;
    if (p && p->win == widget)
        p->win = NULL;
}

static void invalidate(XsPlugin *p)
{
    XsWinState *state;

    if (!p || !p->win)
        return;
    state = g_object_get_data(G_OBJECT(p->win), "xs-state");
    if (state)
        gtk_widget_queue_draw(state->area);
}

static void set_tick(XsPlugin *p, guint ms)
{
    XsWinState *state;

    if (!p)
        return;
    state = g_object_get_data(G_OBJECT(p->win), "xs-state");
    if (!state)
        return;
    if (state->tick_id) {
        g_source_remove(state->tick_id);
        state->tick_id = 0;
    }
    state->tick_ms = ms;
    if (ms > 0)
        state->tick_id = g_timeout_add(ms, (GSourceFunc)xs_core_tick_cb, p);
}

static gint conf_int(GKeyFile *kf, const char *section, const char *key,
                     gint def)
{
    GError *error = NULL;
    gint value = g_key_file_get_integer(kf, section, key, &error);

    if (error) {
        g_error_free(error);
        return def;
    }
    return value;
}

static gdouble conf_dbl(GKeyFile *kf, const char *section, const char *key,
                        gdouble def)
{
    GError *error = NULL;
    gdouble value = g_key_file_get_double(kf, section, key, &error);

    if (error) {
        g_error_free(error);
        return def;
    }
    return value;
}

static char *conf_str(GKeyFile *kf, const char *section, const char *key,
                      const char *def)
{
    GError *error = NULL;
    char *value = g_key_file_get_string(kf, section, key, &error);

    if (error) {
        g_error_free(error);
        return g_strdup(def);
    }
    return value;
}

static gboolean conf_bool(GKeyFile *kf, const char *section, const char *key,
                          gboolean def)
{
    GError *error = NULL;
    gboolean value = g_key_file_get_boolean(kf, section, key, &error);

    if (error) {
        g_error_free(error);
        return def;
    }
    return value;
}

static GdkWindowTypeHint conf_window_type(GKeyFile *kf)
{
    char *type = conf_str(kf, "global", "type", "dock");
    GdkWindowTypeHint hint;

    if (g_ascii_strcasecmp(type, "desktop") == 0)
        hint = GDK_WINDOW_TYPE_HINT_DESKTOP;
    else if (g_ascii_strcasecmp(type, "normal") == 0)
        hint = GDK_WINDOW_TYPE_HINT_NORMAL;
    else if (g_ascii_strcasecmp(type, "dock") == 0)
        hint = GDK_WINDOW_TYPE_HINT_DOCK;
    else {
        if (g_debug)
            xs_log_impl("invalid global type '%s', using dock", type);
        hint = GDK_WINDOW_TYPE_HINT_DOCK;
    }
    g_free(type);
    return hint;
}

static void conf_set_int(GKeyFile *kf, const char *section, const char *key,
                         gint value)
{
    g_key_file_set_integer(kf, section, key, value);
}

static void conf_set_str(GKeyFile *kf, const char *section, const char *key,
                         const char *value)
{
    g_key_file_set_string(kf, section, key, value);
}

static GtkWidget *make_window(XsPlugin *p, int x, int y, int width, int height)
{
    GtkWidget *window;
    GtkWidget *area;
    GdkScreen *screen;
    GdkVisual *visual;
    XsWinState *state;
    GdkWindowTypeHint type_hint = conf_window_type(xs_core_conf());
    gboolean keep_below = conf_bool(xs_core_conf(), "global", "keep_below", TRUE);

    if (!p || width <= 0 || height <= 0)
        return NULL;
    window = gtk_window_new(GTK_WINDOW_TOPLEVEL);
    gtk_window_set_decorated(GTK_WINDOW(window), FALSE);
    gtk_window_set_skip_taskbar_hint(GTK_WINDOW(window), TRUE);
    gtk_window_set_skip_pager_hint(GTK_WINDOW(window), TRUE);
    gtk_window_set_type_hint(GTK_WINDOW(window), type_hint);
    gtk_window_set_keep_below(GTK_WINDOW(window), keep_below);
    gtk_window_set_resizable(GTK_WINDOW(window), FALSE);
    gtk_window_set_default_size(GTK_WINDOW(window), width, height);
    gtk_widget_set_size_request(window, width, height);

    screen = gtk_window_get_screen(GTK_WINDOW(window));
    visual = gdk_screen_get_rgba_visual(screen);
    if (!visual)
        visual = gdk_screen_get_system_visual(screen);
    gtk_widget_set_visual(window, visual);
    gtk_widget_set_app_paintable(window, TRUE);

    area = gtk_drawing_area_new();
    gtk_widget_set_size_request(area, width, height);
    gtk_container_add(GTK_CONTAINER(window), area);
    gtk_window_move(GTK_WINDOW(window), x, y);

    state = g_new0(XsWinState, 1);
    state->opacity = 1.0;
    state->locked = FALSE;  /* позиция не заблокирована по умолчанию */
    state->sticky = conf_bool(xs_core_conf(), "global", "sticky", TRUE);
    state->widget = FALSE;
    state->keep_above = FALSE;
    state->keep_below = keep_below;
    state->win = window;
    state->area = area;
    state->x = x;
    state->y = y;
    state->w = width;
    state->h = height;
    state->plug = p;
    state->last_shape_us = 0;
    state->last_conf_save_us = 0;
    state->frame_dirty = TRUE;
    p->win = window;
    g_object_set_data_full(G_OBJECT(window), "xs-state", state, g_free);

    g_signal_connect(window, "destroy", G_CALLBACK(on_window_destroy), p);
    g_signal_connect(area, "draw", G_CALLBACK(on_draw_frame), p);
    g_signal_connect(window, "configure-event", G_CALLBACK(on_configure_event), p);
    g_signal_connect(window, "button-press-event", G_CALLBACK(on_button), p);
    g_signal_connect(window, "button-release-event", G_CALLBACK(on_button), p);
    g_signal_connect(window, "scroll-event", G_CALLBACK(on_scroll), p);
    g_signal_connect(window, "enter-notify-event",
                     G_CALLBACK(on_enter_notify), p);
    g_signal_connect(window, "leave-notify-event",
                     G_CALLBACK(on_leave_notify), p);
    if (p->ops && p->ops->motion)
        g_signal_connect(window, "motion-notify-event",
                         G_CALLBACK(on_motion), p);
    {
        GdkEventMask events = GDK_BUTTON_PRESS_MASK |
                              GDK_BUTTON_RELEASE_MASK |
                              GDK_SCROLL_MASK |
                              GDK_ENTER_NOTIFY_MASK |
                              GDK_LEAVE_NOTIFY_MASK;
        if (p->ops && p->ops->motion)
            events |= GDK_POINTER_MOTION_MASK |
                      GDK_POINTER_MOTION_HINT_MASK;
        gtk_widget_add_events(window, events);
    }
    gtk_widget_show_all(window);

    /* Флаги окна из конфига плагина (перекрывают глобальные дефолты) */
    if (p->name) {
        GKeyFile *kf = xs_core_plugin_conf(p->name);
        state->locked = conf_bool(kf, p->name, "lock", state->locked);
        state->sticky = conf_bool(kf, p->name, "sticky", state->sticky);
        state->widget = conf_bool(kf, p->name, "widget", state->widget);
        state->keep_above = conf_bool(kf, p->name, "keep_above", state->keep_above);
        state->keep_below = conf_bool(kf, p->name, "keep_below", state->keep_below);
    }
    xs_core_apply_window_flags(state);

    if (g_debug)
        xs_log_impl("window: type=%s keep_below=%s",
                    type_hint == GDK_WINDOW_TYPE_HINT_DESKTOP ? "desktop" :
                    type_hint == GDK_WINDOW_TYPE_HINT_NORMAL ? "normal" : "dock",
                    keep_below ? "true" : "false");
    return window;
}

static void host_cleanup_window(XsPlugin *p)
{
    xs_core_cleanup_plugin_window(p);
}

static GtkWidget *host_make_window(XsPlugin *p, int x, int y, int w, int h)
{
    return make_window(p, x, y, w, h);
}

static void host_invalidate(XsPlugin *p)
{
    invalidate(p);
}

static void host_set_tick(XsPlugin *p, guint ms)
{
    set_tick(p, ms);
}

static gint host_conf_int(GKeyFile *kf, const char *section, const char *key,
                          gint def)
{
    return conf_int(kf, section, key, def);
}

static gdouble host_conf_dbl(GKeyFile *kf, const char *section,
                             const char *key, gdouble def)
{
    return conf_dbl(kf, section, key, def);
}

static char *host_conf_str(GKeyFile *kf, const char *section, const char *key,
                           const char *def)
{
    return conf_str(kf, section, key, def);
}

static void host_conf_set_int(GKeyFile *kf, const char *section,
                              const char *key, gint value)
{
    conf_set_int(kf, section, key, value);
}

static void host_conf_set_str(GKeyFile *kf, const char *section,
                              const char *key, const char *value)
{
    conf_set_str(kf, section, key, value);
}

static void host_conf_set_dbl(GKeyFile *kf, const char *section,
                              const char *key, double value)
{
    g_key_file_set_double(kf, section, key, value);
}
static gboolean host_theme_load(XsPlugin *p, const char *dir)
{
    return theme_load(p, dir);
}

static void host_theme_draw(XsPlugin *p, cairo_t *cr, const char *element,
                            double x, double y, double width)
{
    theme_draw(p, cr, element, x, y, width);
}

static void host_theme_draw_full(XsPlugin *p, cairo_t *cr, const char *element,
                                 double x, double y, double width,
                                 double height)
{
    theme_draw_full(p, cr, element, x, y, width, height);
}

static void host_theme_draw_native(XsPlugin *p, cairo_t *cr,
                                   const char *element, double x, double y)
{
    theme_draw_native(p, cr, element, x, y);
}

static void host_log(const char *fmt, ...)
{
    va_list ap;

    va_start(ap, fmt);
    xs_log_implv(fmt, ap);
    va_end(ap);
}

static void host_set_opacity(XsPlugin *p, double opacity)
{
    XsWinState *state;

    if (!p || !p->win)
        return;
    state = g_object_get_data(G_OBJECT(p->win), "xs-state");
    if (!state)
        return;
    /* Прозрачность реализована рендером (cairo), а не window-opacity:
     * окно остаётся «непрозрачным» для WM — двигаемость Alt+drag сохраняется. */
    state->opacity = CLAMP(opacity, 0.0, 1.0);
    state->frame_dirty = TRUE;
    gtk_widget_queue_draw(state->area);
}

/* Живой ресайз окна плагина (без recreate): окно то же — позиция, sticky,
 * keep-флаги и открытый Properties-диалог не затрагиваются. Input shape
 * пересчитается после перерисовки (capture_frame → shape idle). */
static void host_resize(XsPlugin *p, int w, int h)
{
    XsWinState *state;

    if (!p || !p->win || w <= 0 || h <= 0)
        return;
    state = g_object_get_data(G_OBJECT(p->win), "xs-state");
    if (!state || state->freed)
        return;
    gtk_widget_set_size_request(state->area, w, h);
    gtk_widget_set_size_request(p->win, w, h);
    gtk_window_resize(GTK_WINDOW(p->win), w, h);
    state->w = w;
    state->h = h;
    state->frame_dirty = TRUE;
    gtk_widget_queue_draw(state->area);
    if (g_debug)
        xs_log_impl("resize: %s -> %dx%d", p->name, w, h);
}

static void host_recreate(XsPlugin *p)
{
    xs_core_recreate_plugin(p);
}

/* === Хостинг гостей (frame_launcher) ===
 * Гость — обычный инстанс из .plugins/<name>.conf, но:
 *  - окно гостя репарентится в content-окно хоста (X сам обрезает
 *    всё, что вышло за пределы хоста);
 *  - keep_below=True у гостя, чтобы быть ниже рамки хоста;
 *  - защита от циклов: DFS по конфигам guests_* до MAX глубины.
 * Цикл проверяется ДО запуска, самозапуск невозможен. */

#define XS_GUEST_MAX_DEPTH 8

/* DFS: найдёт цикл host→...→host по конфигам (guests_1..guests_N). */
static gboolean guest_cycle_check(const char *start_name,
                                  const char *cur_name, int depth)
{
    GKeyFile *kf;
    char *path;
    gboolean cyc = FALSE;
    gchar **keys = NULL;
    gsize n = 0;

    if (depth > XS_GUEST_MAX_DEPTH)
        return TRUE; /* глубже лимита считаем циклом */
    if (g_strcmp0(cur_name, start_name) == 0 && depth > 0)
        return TRUE;
    path = xs_plugin_conf_path(cur_name);
    if (!g_file_test(path, G_FILE_TEST_EXISTS)) {
        g_free(path);
        return FALSE; /* конфига нет — не запустится, цикла нет */
    }
    kf = g_key_file_new();
    if (g_key_file_load_from_file(kf, path, G_KEY_FILE_NONE, NULL)) {
        /* секция может называться и старым именем — берём первую */
        gchar **groups = g_key_file_get_groups(kf, NULL);

        for (gsize g = 0; groups && groups[g] && !cyc; g++) {
            int i = 1;

            for (; i <= 32 && !cyc; i++) {
                char *key = g_strdup_printf("guests_%d", i);
                char *gn = g_key_file_get_string(kf, groups[g], key,
                                                 NULL);

                g_free(key);
                if (!gn || !gn[0]) {
                    g_free(gn);
                    break;
                }
                cyc = guest_cycle_check(start_name, gn, depth + 1);
                g_free(gn);
            }
        }
        g_strfreev(groups);
    }
    g_key_file_free(kf);
    g_free(path);
    return cyc;
}

static XsPlugin *find_plugin_by_name(const char *name)
{
    for (gsize i = 0; g_plugins && i < g_plugins->len; i++) {
        XsPlugin *q = g_ptr_array_index(g_plugins, i);

        if (q && q->name && strcmp(q->name, name) == 0)
            return q;
    }
    return NULL;
}

/* Внешняя точка входа для main.c: тип по имени конфига, запуск. */
XsPlugin *xs_core_start_guest_instance(XsPlugin *host,
                                       const char *guest_name)
{
    const char *type = NULL;
    XsPlugin *g;
    XsWinState *hs;
    XsWinState *gs;
    GtkWidget *content;

    if (!host || !host->win || !guest_name || !guest_name[0])
        return NULL;
    /* уже запущен? — это норма (гость мог стартовать раньше демона):
     * просто репарентим его окно в хоста. */
    if (find_plugin_by_name(guest_name)) {
        XsPlugin *g = find_plugin_by_name(guest_name);
        XsWinState *gs;
        GtkWidget *content;
        GdkWindow *gw, *cw;

        /* окно гостя может быть уже уничтожено (инстанс умирает);
         * то же для host->win */
        if (!g->win || !G_IS_OBJECT(g->win))
            return g;
        gs = g_object_get_data(G_OBJECT(g->win), "xs-state");
        content = host_content_widget(host);

        xs_log_impl("guest '%s': exists, win=%p content=%p",
                    guest_name, (void *)g->win, (void *)content);
        if (!gs || !content || !g->win)
            return g;
        gw = gtk_widget_get_window(g->win);
        cw = gtk_widget_get_window(content);
        /* GtkFixed не имеет собственного X-окна → репарентим в окно
         * хоста с оффсетом content-зоны (данные "xs-content-xy"). */
        if (gw && !cw)
            cw = gtk_widget_get_window(host->win);
        if (gw && cw) {
            int cx = 0, cy = 0;

            if (cw == gtk_widget_get_window(host->win)) {
                gint *xy = g_object_get_data(G_OBJECT(host->win),
                                             "xs-content-xy");

                if (xy) {
                    cx = xy[0];
                    cy = xy[1];
                }
            }
            /* флаг гостя ДО XReparentWindow: иначе configure-event от
             * репарента успеет записать позицию в конфиг */
            g_object_set_data(G_OBJECT(g->win), "xs-guest-host",
                              (gpointer)host->name);
            /* гость позиционируется ВНУТРИ content-зоны по своим x/y
             * (оффсеты), иначе все гости лягут в одну точку */
            XReparentWindow(gdk_x11_display_get_xdisplay(
                                gtk_widget_get_display(g->win)),
                            gdk_x11_window_get_xid(gw),
                            gdk_x11_window_get_xid(cw),
                            cx + gs->x, cy + gs->y);
            /* синхронизировать GTK-кэш позиции: первый configure после
             * репарента несёт СТАРЫЕ (экранные) координаты, из-за них
             * кламп уносил гостя в угол. Явный move + флаг первого
             * configure: */
            gtk_window_move(GTK_WINDOW(g->win),
                            cx + gs->x, cy + gs->y);
            g_object_set_data(G_OBJECT(g->win), "xs-guest-first-cfg",
                              GINT_TO_POINTER(1));
            /* Гость — child окна рамки: рисуется поверх родителя
             * автоматически, keep_below НЕ нужен (иначе WM рвёт
             * иерархию и появляется «прозрачный квадрат»).
             * Сбрасываем флаг, унаследованный из конфига. */
            gs->keep_below = FALSE;
            xs_core_apply_window_flags(gs);
            /* гость больше не main_daemon: за него отвечает frame */
            xs_core_guest_set_started_by(g, "plugin");
            /* фон рамки под гостем: снимок кадра (idle, после draw) */
            g_timeout_add(120, host_update_guest_backdrop_idle,
                          guest_bg_ctx_new(host, g));
            /* переприменить позицию: child-окна «съедают» первый move
             * после репарента (60/200/450 мс) */
            g_timeout_add(60, guest_reapply_pos_idle,
                          guest_reapply_ctx_new(host, g));
            g_timeout_add(200, guest_reapply_pos_idle,
                          guest_reapply_ctx_new(host, g));
            g_timeout_add(450, guest_reapply_pos_idle,
                          guest_reapply_ctx_new(host, g));
            xs_log_impl("guest '%s' re-parented into '%s' at %d,%d",
                        guest_name, host->name, cx + gs->x, cy + gs->y);
        }
        return g;
    }
    /* цикл/глубина: до создания чего-либо */
    if (guest_cycle_check(guest_name, guest_name, 0)) {
        xs_log_impl("guest '%s': ЦИКЛ или глубина > %d — запуск отменён",
                    guest_name, XS_GUEST_MAX_DEPTH);
        return NULL;
    }
    /* тип определяем по префиксу имени из загруженных модулей */
    {
        /* обратный вызов в main.c недоступен отсюда; тип получаем из
         * конфига гостя: сохранённый ключ xs_type пишет демон. */
        GKeyFile *kf = xs_core_plugin_conf(guest_name);
        gchar **groups = g_key_file_get_groups(kf, NULL);

        if (groups && groups[0])
            type = g_key_file_get_string(kf, groups[0], "xs_type", NULL);
        g_strfreev(groups);
    }
    if (!type) {
        xs_log_impl("guest '%s': нет xs_type в конфиге", guest_name);
        return NULL;
    }
    g = xs_core_add_instance_for_host(type, guest_name);
    if (!g || !g->win)
        return NULL;
    hs = g_object_get_data(G_OBJECT(host->win), "xs-state");
    gs = g_object_get_data(G_OBJECT(g->win), "xs-state");
    content = host_content_widget(host);
    xs_log_impl("guest '%s' hosted by '%s' at %d,%d (win x/y)",
                guest_name, host->name, gs ? gs->x : -1, gs ? gs->y : -1);
    /* репарент окна гостя в content-окно хоста: X обрежет выход за
     * границы (зона отображения = внешний размер − рамка − тень). */
    content = host_content_widget(host);
    if (hs && gs && content) {
        GdkWindow *gw = gtk_widget_get_window(g->win);
        GdkWindow *cw = gtk_widget_get_window(content);
        int cx = 0, cy = 0;

        xs_log_impl("refit: gw=%p cw=%p (fixed has %s)",
                    (void *)gw, (void *)cw,
                    cw ? "win" : "NO win");

        if (gw && !cw)
            cw = gtk_widget_get_window(host->win);
        if (cw == gtk_widget_get_window(host->win)) {
            gint *xy = g_object_get_data(G_OBJECT(host->win),
                                         "xs-content-xy");

            if (xy) {
                cx = xy[0];
                cy = xy[1];
            }
        }
        if (gw && cw) {
            /* флаг гостя ДО XReparentWindow: иначе configure-event от
             * репарента успеет записать позицию в конфиг */
            g_object_set_data(G_OBJECT(g->win), "xs-guest-host",
                              (gpointer)host->name);
            XReparentWindow(gdk_x11_display_get_xdisplay(
                                gtk_widget_get_display(g->win)),
                            gdk_x11_window_get_xid(gw),
                            gdk_x11_window_get_xid(cw),
                            cx + gs->x, cy + gs->y);
            /* синхронизировать GTK-кэш позиции: первый configure после
             * репарента несёт СТАРЫЕ (экранные) координаты, из-за них
             * кламп уносил гостя в угол. Явный move + флаг первого
             * configure: */
            gtk_window_move(GTK_WINDOW(g->win),
                            cx + gs->x, cy + gs->y);
            g_object_set_data(G_OBJECT(g->win), "xs-guest-first-cfg",
                              GINT_TO_POINTER(1));
            /* Гость — child окна рамки: рисуется поверх родителя
             * автоматически, keep_below НЕ нужен. */
            gs->keep_below = FALSE;
            xs_core_apply_window_flags(gs);
            xs_core_guest_set_started_by(g, "plugin");
            /* фон рамки под гостем: снимок кадра (idle, после draw) */
            g_timeout_add(120, host_update_guest_backdrop_idle,
                          guest_bg_ctx_new(host, g));
            /* переприменить позицию: child-окна «съедают» первый move
             * после репарента (60/200/450 мс) */
            g_timeout_add(60, guest_reapply_pos_idle,
                          guest_reapply_ctx_new(host, g));
            g_timeout_add(200, guest_reapply_pos_idle,
                          guest_reapply_ctx_new(host, g));
            g_timeout_add(450, guest_reapply_pos_idle,
                          guest_reapply_ctx_new(host, g));
        }
    }
    xs_log_impl("guest '%s' hosted by '%s'", guest_name, host->name);
    return g;
}

/* Content-виджет хоста для репарента гостей (frame_launcher). Хост-плагин
 * помечает свой дочерний GtkWidget данными "xs-content". */
GtkWidget *host_content_widget(XsPlugin *p)
{
    if (!p || !p->win)
        return NULL;
    return g_object_get_data(G_OBJECT(p->win), "xs-content");
}

/* Гость передан хозяину-рамке: started_by=plugin в конфиге, symlink из
 * plugins_on удалить (демон его больше не запускает сам). */
void xs_core_guest_set_started_by(XsPlugin *g, const char *who)
{
    GKeyFile *kf;
    char *link;

    if (!g || !g->name || !who)
        return;
    kf = xs_core_plugin_conf(g->name);
    g_key_file_set_string(kf, g->name, "started_by", who);
    xs_core_plugin_conf_flush(g->name);
    if (g_plugin_onoff_dir) {
        char *name_conf = g_strdup_printf("%s.conf", g->name);

        link = g_build_filename(g_plugin_onoff_dir, name_conf, NULL);
        unlink(link);
        g_free(name_conf);
        g_free(link);
        xs_log_impl("guest '%s': started_by=%s, symlink removed",
                    g->name, who);
    }
}

static XsPlugin *start_guest(XsPlugin *host, const char *guest_name)
{
    return xs_core_start_guest_instance(host, guest_name);
}

static void stop_guest(XsPlugin *host, const char *guest_name)
{
    XsPlugin *g = find_plugin_by_name(guest_name);

    (void)host;
    if (g)
        xs_core_delete_instance_full(g, FALSE);
}

/* Копия куска отрисованного кадра хоста под гостем: гость рисует её
 * своим фоном (композитор не смешивает child-окно с родителем, поэтому
 * прозрачные пиксели гостя показывали бы рабочий стол). */
static cairo_surface_t *host_backdrop_snapshot(XsPlugin *host, int gx,
                                               int gy, int gw, int gh)
{
    XsWinState *hs;
    cairo_surface_t *src, *dst;
    cairo_t *cr;

    if (!host || !host->win || gw <= 0 || gh <= 0)
        return NULL;
    hs = g_object_get_data(G_OBJECT(host->win), "xs-state");
    if (!hs || !hs->frame)
        return NULL;
    src = hs->frame;
    {
        int sw = cairo_image_surface_get_width(src);
        int sh = cairo_image_surface_get_height(src);

        if (gx < 0) gx = 0;
        if (gy < 0) gy = 0;
        if (gx + gw > sw) gw = sw - gx;
        if (gy + gh > sh) gh = sh - gy;
        if (gw <= 0 || gh <= 0)
            return NULL;
    }
    dst = cairo_image_surface_create(CAIRO_FORMAT_ARGB32, gw, gh);
    cr = cairo_create(dst);
    cairo_set_source_surface(cr, src, -gx, -gy);
    cairo_set_operator(cr, CAIRO_OPERATOR_SOURCE);
    cairo_paint(cr);
    cairo_destroy(cr);
    return dst;
}

static void host_update_guest_backdrop(XsPlugin *host, XsPlugin *g)
{
    XsWinState *gs;
    cairo_surface_t *snap;
    gint *xy;

    if (!host || !host->win || !g || !g->win)
        return;
    gs = g_object_get_data(G_OBJECT(g->win), "xs-state");
    if (!gs)
        return;
    xy = host->win ? g_object_get_data(G_OBJECT(host->win),
                                       "xs-content-xy") : NULL;
    /* КЛАМП oversized гостя: окно не должно вылезать в кайму */
    {
        XsWinState *hs = g_object_get_data(G_OBJECT(host->win),
                                           "xs-state");
        int gw = 0, gh = 0, max_x, max_y;
        gboolean clamped = FALSE;

        if (hs) {
            gtk_window_get_size(GTK_WINDOW(g->win), &gw, &gh);
            max_x = hs->w - (xy ? 2 * xy[0] : 0) - gw;
            max_y = hs->h - (xy ? 2 * xy[1] : 0) - gh;
            if (max_x < 0) max_x = 0;
            if (max_y < 0) max_y = 0;
            if (gs->x > max_x) { gs->x = max_x; clamped = TRUE; }
            if (gs->y > max_y) { gs->y = max_y; clamped = TRUE; }
            if (gs->x < 0) { gs->x = 0; clamped = TRUE; }
            if (gs->y < 0) { gs->y = 0; clamped = TRUE; }
            if (clamped)
                gtk_window_move(GTK_WINDOW(g->win),
                                (xy ? xy[0] : 0) + gs->x,
                                (xy ? xy[1] : 0) + gs->y);
        }
    }
    /* снимок куска кадра ХОСТА там, где физически лежит окно гостя:
     * его позиция в координатах родителя = content_xy + state->x/y */
    {
        int ox = xy ? xy[0] : 0;
        int oy = xy ? xy[1] : 0;

        snap = host_backdrop_snapshot(host, ox + gs->x, oy + gs->y,
                                      gs->w ? gs->w : 1,
                                      gs->h ? gs->h : 1);
    }
    if (!snap)
        return;
    g_object_set_data_full(G_OBJECT(g->win), "xs-host-backdrop",
                           snap,
                           (GDestroyNotify)cairo_surface_destroy);
    gtk_widget_queue_draw(g->win);
}

/* контекст отложенного снимка (GuestBgCtx объявлен в форвардах) */
static gpointer guest_bg_ctx_new(XsPlugin *host, XsPlugin *guest)
{
    GuestBgCtx *c = g_new0(GuestBgCtx, 1);

    c->host = host;
    c->guest = guest;
    return c;
}

/* Инстанс ещё зарегистрирован (указатель валиден, не уничтожен)? */
static gboolean plugin_alive(XsPlugin *p)
{
    gsize i;

    if (!p || !g_plugins)
        return FALSE;
    for (i = 0; i < g_plugins->len; i++) {
        if (g_ptr_array_index(g_plugins, i) == p)
            return TRUE;
    }
    return FALSE;
}

/* Повторно применить позицию гостя (child-окна иногда «съедают»
 * первый move сразу после репарента — спиннеры 0→1→0 это лечили;
 * теперь лечит демон автоматически). */
static gboolean guest_reapply_pos_idle(gpointer data)
{
    GuestReapplyCtx *c = data;
    XsWinState *ggs;
    gint *xy;
    int cx = 0, cy = 0;

    if (plugin_alive(c->host) && plugin_alive(c->guest) &&
        c->host->win && c->host->priv && c->guest->win &&
        c->guest->priv) {
        ggs = g_object_get_data(G_OBJECT(c->guest->win), "xs-state");
        xy = g_object_get_data(G_OBJECT(c->host->win),
                               "xs-content-xy");
        if (ggs && xy) {
            cx = xy[0];
            cy = xy[1];
            gtk_window_move(GTK_WINDOW(c->guest->win),
                            cx + ggs->x, cy + ggs->y);
        }
    }
    g_free(c);
    return G_SOURCE_REMOVE;
}

static gpointer guest_reapply_ctx_new(XsPlugin *host, XsPlugin *guest)
{
    GuestReapplyCtx *c = g_new0(GuestReapplyCtx, 1);

    c->host = host;
    c->guest = guest;
    return c;
}

static gboolean host_update_guest_backdrop_idle(gpointer data)
{
    GuestBgCtx *c = data;

    /* оба должны быть живы (зарегистрированы + priv != NULL) */
    if (plugin_alive(c->host) && plugin_alive(c->guest) &&
        c->host->priv && c->guest->priv)
        host_update_guest_backdrop(c->host, c->guest);
    g_free(c);
    return G_SOURCE_REMOVE;
}

/* Гость: фон из кадра хозяина (data окна "xs-host-backdrop"). */
static cairo_surface_t *host_get_backdrop(XsPlugin *p)
{
    if (!p || !p->win)
        return NULL;
    return g_object_get_data(G_OBJECT(p->win), "xs-host-backdrop");
}

/* Пересчитать гостей под новую content-зону (смена темы/размера):
 * кламп позиций в [0..cw-gw]/[0..ch-gh], move, переснятие фона. */
static void host_refit_guests(XsPlugin *host, int cx, int cy, int cw,
                              int ch)
{
    gsize i;

    if (!host || !host->win || !host->name || cw <= 0 || ch <= 0)
        return;
    for (i = 0; g_plugins && i < g_plugins->len; i++) {
        XsPlugin *g = g_ptr_array_index(g_plugins, i);
        const char *gHost;
        XsWinState *ggs;
        GKeyFile *gkf;
        int gw = 0, gh = 0, max_x, max_y;
        gboolean changed = FALSE;
        GuestBgCtx *c1, *c2;

        if (!g || !g->win || !g->name)
            continue;
        gHost = g_object_get_data(G_OBJECT(g->win), "xs-guest-host");

        if (!gHost || strcmp(gHost, host->name) != 0)
            continue;
        ggs = g_object_get_data(G_OBJECT(g->win), "xs-state");
        if (!ggs)
            continue;
        gtk_window_get_size(GTK_WINDOW(g->win), &gw, &gh);
        max_x = cw - gw;
        max_y = ch - gh;
        if (max_x < 0) max_x = 0;
        if (max_y < 0) max_y = 0;
        if (ggs->x > max_x) { ggs->x = max_x; changed = TRUE; }
        if (ggs->y > max_y) { ggs->y = max_y; changed = TRUE; }
        if (ggs->x < 0) { ggs->x = 0; changed = TRUE; }
        if (ggs->y < 0) { ggs->y = 0; changed = TRUE; }
        if (changed) {
            gkf = xs_core_plugin_conf(g->name);
            g_key_file_set_integer(gkf, g->name, "x", ggs->x);
            g_key_file_set_integer(gkf, g->name, "y", ggs->y);
            xs_core_plugin_conf_flush(g->name);
        }
        gtk_window_move(GTK_WINDOW(g->win),
                        cx + ggs->x, cy + ggs->y);
        c1 = guest_bg_ctx_new(host, g);
        g_timeout_add(120, host_update_guest_backdrop_idle, c1);
        c2 = guest_bg_ctx_new(host, g);
        g_timeout_add(500, host_update_guest_backdrop_idle, c2);
    }
}

/* Создать НОВЫЙ инстанс типа type и включить его гостем хоста:
 * генерируем UUID-конфиг (started_by=plugin, symlink не создаётся),
 * позиция гостя — внутри видимой зоны рамки. */
static XsPlugin *start_guest_new(XsPlugin *host, const char *type)
{
    XsPlugin *g;
    XsWinState *hs, *gs;
    GKeyFile *kf;

    if (!host || !type || !type[0])
        return NULL;
    g = xs_core_add_instance_for_host(type, NULL);
    if (!g || !g->win)
        return NULL;
    /* стартовая позиция гостя: внутри content-зоны хоста, каскадом */
    hs = g_object_get_data(G_OBJECT(host->win), "xs-state");
    gs = g_object_get_data(G_OBJECT(g->win), "xs-state");
    kf = xs_core_plugin_conf(g->name);
    if (hs && gs && kf) {
        gint *xy = g_object_get_data(G_OBJECT(host->win),
                                     "xs-content-xy");
        int cx = xy ? xy[0] : 12, cy = xy ? xy[1] : 12;
        int avail_w = hs->w - 2 * cx - 24;
        int avail_h = hs->h - 2 * cy - 24;
        int n = 0, i;
        char *key, *v;

        /* каскад: сколько уже гостей у хоста — такой оффсет */
        for (i = 1; i <= 64; i++) {
            key = g_strdup_printf("guests_%d", i);
            v = conf_str(kf, host->name, key, "");
            g_free(key);
            if (v[0]) {
                n++;
                g_free(v);
            } else {
                g_free(v);
                break;
            }
        }
        /* state->x/y — координаты ВНУТРИ content-зоны (без cx/cy!) */
        gs->x = 16 * (n % 6);
        gs->y = 16 * (n % 6);
        if (gs->x > avail_w) gs->x = 0;
        if (gs->y > avail_h) gs->y = 0;
        g_key_file_set_integer(kf, g->name, "x", gs->x);
        g_key_file_set_integer(kf, g->name, "y", gs->y);
    }
    /* включить в рамку через общий путь (репарент, флаги, started_by) */
    return xs_core_start_guest_instance(host, g->name);
}

/* --- списки для диалогов frame_launcher --- */

char **xs_core_list_plugin_types(void)
{
    char **out;
    int n = 0, i = 0;

    if (!g_loaded_modules_ref || !*g_loaded_modules_ref) {
        out = g_new0(char *, 1);
        return out;
    }
    n = (*g_loaded_modules_ref)->len;
    out = g_new0(char *, n + 1);
    for (i = 0; i < n; i++) {
        XsLoadedPlugin *lp = g_ptr_array_index(*g_loaded_modules_ref, i);

        out[i] = (lp && lp->desc && lp->desc->name)
                     ? g_strdup(lp->desc->name) : g_strdup("?");
    }
    return out;
}

int xs_core_type_count(void)
{
    return (g_loaded_modules_ref && *g_loaded_modules_ref)
               ? (*g_loaded_modules_ref)->len : 0;
}

char **xs_core_list_running_daemon_instances(int *count)
{
    char **out;
    int n = 0, i;
    gsize j;

    if (count)
        *count = 0;
    if (!g_plugins)
        return g_new0(char *, 1);
    out = g_new0(char *, g_plugins->len + 1);
    for (j = 0; j < g_plugins->len; j++) {
        XsPlugin *q = g_ptr_array_index(g_plugins, j);
        GKeyFile *kf;

        if (!q || !q->name || !q->win)
            continue;
        kf = xs_core_plugin_conf(q->name);
        {
            char *sb = kf ? g_key_file_get_string(kf, q->name,
                                                  "started_by", NULL)
                          : NULL;

            /* main_daemon-инстансы; ОТСУТСТВИЕ started_by =
             * main_daemon (старые конфиги clock/calendar) */
            if (!sb || strcmp(sb, "main_daemon") == 0)
                out[n++] = g_strdup(q->name);
            g_free(sb);
        }
    }
    if (count)
        *count = n;
    return out;
}

static void xs_tray_add_plugin_wrapper(XsPlugin *p)
{
    /* Гость рамки (started_by=plugin) не показывается в трей-меню:
     * его запуском/остановкой управляет хозяин, не демон. */
    if (p && p->win &&
        g_object_get_data(G_OBJECT(p->win), "xs-guest-host"))
        return;
    xs_tray_add_plugin(p);
}

static XsHostApi host_api = {
    .make_window = host_make_window,
    .invalidate = host_invalidate,
    .set_tick = host_set_tick,
    .conf_int = host_conf_int,
    .conf_dbl = host_conf_dbl,
    .conf_str = host_conf_str,
    .conf_set_int = host_conf_set_int,
    .conf_set_str = host_conf_set_str,
    .conf_set_dbl = host_conf_set_dbl,
    .theme_load = host_theme_load,
    .theme_draw = host_theme_draw,
    .log = host_log,
    .cleanup_window = host_cleanup_window,
    .set_opacity = host_set_opacity,
    .recreate = host_recreate,
    .resize = host_resize,
    .theme_draw_full = host_theme_draw_full,
    .theme_draw_native = host_theme_draw_native,
    .start_guest = start_guest,
    .stop_guest = stop_guest,
    .start_guest_new = start_guest_new,
    .get_host_backdrop = host_get_backdrop,
    .refit_guests = host_refit_guests
};

void xs_core_cleanup_plugin_window(XsPlugin *p)
{
    XsWinState *state;

    if (!p || !p->win)
        return;
    state = g_object_get_data(G_OBJECT(p->win), "xs-state");
    if (state) {
        if (state->tick_id) {
            g_source_remove(state->tick_id);
            state->tick_id = 0;
        }
        if (state->shape_idle_id) {
            g_source_remove(state->shape_idle_id);
            state->shape_idle_id = 0;
        }
        if (state->shape_retry_id) {
            g_source_remove(state->shape_retry_id);
            state->shape_retry_id = 0;
        }
        if (state->frame) {
            cairo_surface_destroy(state->frame);
            state->frame = NULL;
        }
        if (state->theme) {
            xs_theme_free(state->theme);
            state->theme = NULL;
        }
        state->freed = TRUE;
        g_object_set_data_full(G_OBJECT(p->win), "xs-state", NULL, NULL);
    }
    gtk_widget_destroy(p->win);
    p->win = NULL;
}

void xs_core_free_plugin(XsPlugin *p)
{
    if (!p)
        return;
    xs_core_cleanup_plugin_window(p);
    g_free((char *)p->name);
    g_free((char *)p->type);
    g_free(p);
}

XsHostApi *xs_host_api(void)
{
    return &host_api;
}
