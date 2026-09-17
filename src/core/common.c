#include <unistd.h> /* symlink, unlink, rmdir */
#include <stdio.h>  /* remove */

#include "common.h"
#include "tray.h"

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
    dir = g_path_get_dirname(g_plugin_onoff_dir);
    newlink = g_build_filename(g_plugin_onoff_dir, new_name, NULL);
    unlink(newlink);
    {
        char *target = g_build_filename(dir, ".plugins",
                                        new_name, NULL);

        if (symlink(target, newlink) != 0)
            xs_log_impl("symlink %s -> %s failed", newlink, target);
        g_free(target);
    }
    if (old_name && old_name[0]) {
        oldlink = g_build_filename(g_plugin_onoff_dir, old_name, NULL);
        unlink(oldlink);
        g_free(oldlink);
    }
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

/* Редактирование user_label (пользовательская метка инстанса). Меняет
 * метку в конфиге; файл конфига НЕ переименовываем (имя файла =
 * тип-UUID-label фиксируется при создании; метка живёт отдельно). */
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
}

/* Текущее открытое окно Properties (для восстановления keep-above после
 * recreate). Диалог один на процесс — как в оригинале se.run(). */
static GtkWindow *g_props_dialog = NULL;
/* Spin-кнопки X/Y открытого Properties-диалога: обновляются при перемещении
 * окна апплета (debounce: только когда перемещение прекратилось на 500 мс). */
static GtkSpinButton *g_props_spin_x = NULL;
static GtkSpinButton *g_props_spin_y = NULL;
static guint g_props_pos_update_id = 0;

static void xs_core_props_destroyed(GtkWidget *w, gpointer data)
{
    (void)w;
    (void)data;
    g_props_dialog = NULL;
    g_props_spin_x = NULL;
    g_props_spin_y = NULL;
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
 * Если позиция заблокирована (Window > Lock) — только в конфиг, без move,
 * чтобы не спорить с WM. */
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
    if (!state->locked)
        gtk_window_move(GTK_WINDOW(p->win),
                        strcmp(key, "x") == 0 ? v : state->x,
                        strcmp(key, "x") == 0 ? state->y : v);
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
    if (g_props_spin_x)
        gtk_spin_button_set_value(g_props_spin_x, state->x);
    if (g_props_spin_y)
        gtk_spin_button_set_value(g_props_spin_y, state->y);
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
    GtkWidget *hbox = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 5);
    GtkWidget *lbl = gtk_label_new(label);

    gtk_widget_set_halign(lbl, GTK_ALIGN_START);
    gtk_widget_set_valign(lbl, GTK_ALIGN_START);
    gtk_widget_set_size_request(lbl, 180, 28);
    gtk_box_pack_start(GTK_BOX(hbox), lbl, FALSE, TRUE, 0);
    if (input) {
        if (desc)
            gtk_widget_set_tooltip_text(input, desc);
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
    lbl = gtk_label_new(info);
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
                             gtk_label_new("About"));

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

        gtk_grid_attach(GTK_GRID(inner_page), gtk_label_new("Scale"),
                        0, row, 1, 1);
        {
            GtkAdjustment *adj = gtk_adjustment_new(
                conf_dbl(kf, p->name, "scale", 1.0), 0.2, 10.0, 0.1, 0.5, 0.0);
            GtkWidget *spin = gtk_spin_button_new(adj, 0.1, 2);
            gtk_grid_attach(GTK_GRID(inner_page), spin, 1, row, 1, 1);
            g_signal_connect(spin, "value-changed",
                             G_CALLBACK(xs_core_prop_scale_changed), p);
        }
        row++;
        gtk_grid_attach(GTK_GRID(inner_page), gtk_label_new("Opacity"),
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
        gtk_grid_attach(GTK_GRID(inner_page), gtk_label_new("X-Position"),
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
            g_object_add_weak_pointer(G_OBJECT(spin), (gpointer *)&g_props_spin_x);
        }
        row++;
        gtk_grid_attach(GTK_GRID(inner_page), gtk_label_new("Y-Position"),
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
                            gtk_label_new("User label"), 0, row, 1, 1);
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
                                 gtk_label_new("Window"));
        gtk_widget_show_all(inner_nb);
        gtk_box_pack_start(GTK_BOX(page), inner_nb, TRUE, TRUE, 0);
    }
    gtk_widget_show_all(page);
    gtk_notebook_append_page(GTK_NOTEBOOK(notebook), page,
                             gtk_label_new("Options"));

    /* Свои страницы-группы плагина (Clock/Alarm/Face у clock) */
    if (p->ops && p->ops->properties)
        p->ops->properties(p, GTK_NOTEBOOK(notebook));

    /* --- Themes --- */
    if (p->ops && p->ops->fill_themes) {
        page = gtk_box_new(GTK_ORIENTATION_VERTICAL, 5);
        gtk_container_set_border_width(GTK_CONTAINER(page), 10);
        w = gtk_label_new("Themes allow you to easily switch the appearance "
                          "of your Screenlets. On this page you find a list "
                          "of all available themes for this Screenlet.");
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
                                 gtk_label_new("Themes"));
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
    item = gtk_menu_item_new_with_label("Size");
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
    item = gtk_menu_item_new_with_label("Window");
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
    mi = gtk_menu_item_new_with_label("Properties...");
    xs_core_menu_connect_cmd(mi, p, "properties");
    gtk_menu_shell_append(GTK_MENU_SHELL(menu), mi);
    mi = gtk_menu_item_new_with_label("Info...");
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
    mi = gtk_menu_item_new_with_label("Quit");
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
    if (event->type != GDK_BUTTON_PRESS)
        return FALSE; /* popup только на press: release от меню повторно его не открывает */
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
        /* удалить этот инстанс (окно уже уничтожится, меню закрыто через idle) */
        xs_core_delete_instance(p);
    } else if (strcmp(cmd, "quit") == 0) {
        xs_core_shutdown_all();
        gtk_main_quit();
    }
}

/* ==== public plugin API ==== */

void xs_core_shutdown_all(void)
{
    gsize i;

    if (g_conf && g_conf_path && g_conf_path[0] != '\0')
        xs_core_conf_flush();
    if (g_plugins) {
        for (i = 0; i < g_plugins->len; i++) {
            XsPlugin *p = g_ptr_array_index(g_plugins, i);
            if (p) {
                xs_core_shutdown_plugin(p);
                g_free((char *)p->name);
            }
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
    for (i = 0; i < g_plugins->len; i++) {
        XsPlugin *p = g_ptr_array_index(g_plugins, i);
        if (!p)
            continue;
        xs_tray_remove_plugin(p);
        if (p->ops && p->ops->shutdown)
            p->ops->shutdown(p);
        xs_core_cleanup_plugin_window(p);
        if (!p->ops || !p->ops->init ||
            p->ops->init(p, xs_core_plugin_conf(p->name)) != 0 || !p->win) {
            xs_log_impl("reload: %s initialization failed", p->name);
            g_ptr_array_remove_index_fast(g_plugins, i);
            i--;
            xs_core_free_plugin(p);
        } else {
            xs_core_save_plugin_position(p);
        }
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
    XsPlugin *proto = NULL;
    XsPlugin *p;
    char *iname;
    GKeyFile *kf;
    gsize i;

    if (!type || !type[0] || !g_plugins)
        return -1;
    for (i = 0; i < g_plugins->len; i++) {
        XsPlugin *q = g_ptr_array_index(g_plugins, i);

        if (q && xs_core_plugin_type(q) && strcmp(xs_core_plugin_type(q), type) == 0) {
            proto = q;
            break;
        }
    }
    if (!proto || !proto->ops || !proto->ops->init) {
        xs_log_impl("add: no loaded plugin of type '%s'", type);
        return -1;
    }
    iname = xs_core_next_instance_name(type);
    kf = xs_core_plugin_conf(iname);
    /* Новый инстанс наследует вид от секции типа (theme/scale/opacity),
     * если у него ещё нет своих значений. */
    if (proto->type && strcmp(proto->type, proto->name) != 0) {
        GKeyFile *pkf = xs_core_plugin_conf(proto->name);
        static const char *inherit[] = { "theme", "scale", "opacity" };
        size_t k;

        for (k = 0; k < G_N_ELEMENTS(inherit); k++) {
            char *v = conf_str(pkf, proto->name, inherit[k], NULL);

            if (v) {
                g_key_file_set_string(kf, iname, inherit[k], v);
                g_free(v);
            }
        }
    }
    p = g_new0(XsPlugin, 1);
    p->name = iname;
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
        return -1;
    }
    xs_core_register_plugin(p);
    xs_tray_add_plugin(p);
    xs_tray_rebuild();
    xs_core_plugin_conf_flush(p->name);
    /* Новый инстанс включён: symlink в plugins_on. */
    xs_onoff_relink(NULL, p->name);
    xs_log_impl("added instance %s (type %s)", p->name, type);
    xs_core_save_instances();
    return 0;
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
                                 "Мёртвый symlink: %s\n\n"
                                 "Конфиг не найден. Удалить symlink?",
                                 linkname);
    gtk_dialog_add_buttons(GTK_DIALOG(dlg),
                           "Удалить", 1,
                           "Указать другой конфиг", 2,
                           "Отмена", 3,
                           NULL);
    res = gtk_dialog_run(GTK_DIALOG(dlg));
    if (res == 2) {
        GtkWidget *fc = gtk_file_chooser_dialog_new(
            "Выберите конфиг в .plugins", parent,
            GTK_FILE_CHOOSER_ACTION_OPEN, "Отмена", GTK_RESPONSE_CANCEL,
            "Выбрать", GTK_RESPONSE_ACCEPT, NULL);
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

void xs_core_delete_instance(XsPlugin *p)
{
    char *name_copy;

    if (!p)
        return;
    xs_log_impl("deleting instance %s", p->name);
    name_copy = g_strdup(p->name);
    xs_core_shutdown_plugin(p);
    xs_core_unregister_plugin(p);
    xs_core_free_plugin(p);
    xs_tray_rebuild();
    /* Убрать symlink из plugins_on; конфиг в .plugins остаётся
     * (инстанс можно снова включить, выбрав его при мёртвом symlink). */
    if (g_plugin_onoff_dir) {
        char *link = g_build_filename(g_plugin_onoff_dir, name_copy, NULL);

        unlink(link);
        g_free(link);
    }
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

static gboolean theme_load(XsPlugin *p, const char *dir)
{
    XsWinState *state;
    GDir *directory;
    const char *filename;
    XsTheme *theme;

    if (!p || !dir)
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
        png_rect.x = (int)x;
        png_rect.y = (int)y;
        png_rect.width = (int)width;
        png_rect.height = (int)height;
        gdk_cairo_set_source_pixbuf(cr, it->u.png, png_rect.x, png_rect.y);
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
    g_signal_connect(window, "motion-notify-event", G_CALLBACK(on_motion), p);
    gtk_widget_add_events(window,
                          GDK_BUTTON_PRESS_MASK | GDK_BUTTON_RELEASE_MASK |
                              GDK_SCROLL_MASK |
                              GDK_POINTER_MOTION_MASK |
                              GDK_POINTER_MOTION_HINT_MASK);
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
    .theme_draw_native = host_theme_draw_native
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
