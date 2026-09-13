#include "common.h"
#include "tray.h"

#include <glib-unix.h>
#include <gtk/gtk.h>
#include <glib.h>
#include <gmodule.h>
#include <stdlib.h>
#include <string.h>

static char *g_conf_path = NULL;
static char *g_plugdir = NULL;
static GPtrArray *g_loaded_modules = NULL;
static guint g_sighup_source_id = 0;

/* Описание загруженного типа плагина: модуль + дескриптор.
 * Инстансов может быть много на один тип. */
typedef struct {
    GModule *mod;
    XsPluginDesc *desc;
} XsLoadedPlugin;

static gboolean on_sighup(gpointer data)
{
    (void)data;
    xs_log_impl("SIGHUP received, reloading plugins");
    xs_core_reload();
    return G_SOURCE_CONTINUE;
}

static void setup_sighup_handler(void)
{
    if (g_sighup_source_id) {
        g_source_remove(g_sighup_source_id);
        g_sighup_source_id = 0;
    }
    g_sighup_source_id = g_unix_signal_add(SIGHUP, on_sighup, NULL);
    if (!g_sighup_source_id)
        xs_log_impl("cannot install SIGHUP handler");
}

static void uninstall_sighup_handler(void)
{
    if (g_sighup_source_id) {
        g_source_remove(g_sighup_source_id);
        g_sighup_source_id = 0;
    }
}

static void free_loaded_modules(void)
{
    if (!g_loaded_modules)
        return;
    for (gsize i = 0; i < g_loaded_modules->len; i++) {
        XsLoadedPlugin *lp = g_ptr_array_index(g_loaded_modules, i);

        g_module_close(lp->mod);
        g_free(lp);
    }
    g_ptr_array_free(g_loaded_modules, TRUE);
    g_loaded_modules = NULL;
}

/* Загрузить все .so из plugdir (только модули+дескрипторы, без инстансов). */
static void load_plugin_modules(void)
{
    GDir *d = g_dir_open(g_plugdir, 0, NULL);

    if (!d) {
        xs_log_impl("plugin dir %s: cannot open", g_plugdir);
        return;
    }
    const char *fn;
    while ((fn = g_dir_read_name(d))) {
        char *path;
        GModule *mod;
        XsPluginDescFn fn_desc;
        XsLoadedPlugin *lp;

        if (!g_str_has_suffix(fn, ".so"))
            continue;
        path = g_build_filename(g_plugdir, fn, NULL);
        mod = g_module_open(path, G_MODULE_BIND_LAZY | G_MODULE_BIND_LOCAL);
        if (!mod) {
            xs_log_impl("load %s: %s", path, g_module_error());
            g_free(path);
            continue;
        }
        fn_desc = NULL;
        if (!g_module_symbol(mod, "xs_plugin_desc", (gpointer *)&fn_desc) ||
            !fn_desc) {
            xs_log_impl("load %s: no xs_plugin_desc", path);
            g_module_close(mod);
            g_free(path);
            continue;
        }
        lp = g_new0(XsLoadedPlugin, 1);
        lp->desc = fn_desc();
        if (!lp->desc || lp->desc->api_version != XS_API_VERSION) {
            xs_log_impl("load %s: api_version mismatch (%u != %u)",
                        path, lp->desc ? lp->desc->api_version : 0,
                        XS_API_VERSION);
            g_free(lp);
            g_module_close(mod);
            g_free(path);
            continue;
        }
        if (!g_loaded_modules)
            g_loaded_modules = g_ptr_array_new();
        g_ptr_array_add(g_loaded_modules, lp);
        xs_log_impl("module %s: type '%s' ready", path, lp->desc->name);
        g_free(path);
    }
    g_dir_close(d);
}

static XsLoadedPlugin *find_loaded(const char *type)
{
    if (!g_loaded_modules || !type)
        return NULL;
    for (gsize i = 0; i < g_loaded_modules->len; i++) {
        XsLoadedPlugin *lp = g_ptr_array_index(g_loaded_modules, i);

        if (lp->desc && strcmp(lp->desc->name, type) == 0)
            return lp;
    }
    return NULL;
}

/* Создать инстанс типа type с именем iname. */
static XsPlugin *create_instance(const char *type, const char *iname)
{
    XsLoadedPlugin *lp = find_loaded(type);
    XsPlugin *p;

    if (!lp) {
        xs_log_impl("instance '%s': plugin type '%s' not loaded",
                    iname, type);
        return NULL;
    }
    p = g_new0(XsPlugin, 1);
    p->name = g_strdup(iname);
    p->type = g_strdup(type);
    p->host = xs_host_api();
    p->ops = lp->desc->ops;
    p->desc = lp->desc->desc;
    p->author = lp->desc->author;
    p->version = lp->desc->version;
    p->priv = NULL;
    if (p->ops && p->ops->init) {
        GKeyFile *kf = xs_core_plugin_conf(p->name);

        if (p->ops->init(p, kf) != 0 || !p->win) {
            xs_log_impl("instance %s: init failed", p->name);
            xs_core_free_plugin(p);
            return NULL;
        }
    }
    xs_core_register_plugin(p);
    xs_tray_add_plugin(p);
    xs_log_impl("loaded %s (api %u)", p->name, lp->desc->api_version);
    return p;
}

/* Список инстансов: главный конфиг, секция [instances], ключи = имена,
 * значения = тип. Нет секции/ключа → дефолтный инстанс "type" = type
 * (обратная совместимость со старыми конфигами). */
static void create_instances(void)
{
    GKeyFile *cf = xs_core_conf();
    gchar **keys = NULL;
    gsize n = 0;

    if (g_key_file_has_group(cf, "instances"))
        keys = g_key_file_get_keys(cf, "instances", &n, NULL);
    if (!keys || n == 0) {
        /* Дефолт: по одному инстансу на загруженный тип. */
        if (g_loaded_modules) {
            for (gsize i = 0; i < g_loaded_modules->len; i++) {
                XsLoadedPlugin *lp = g_ptr_array_index(g_loaded_modules, i);

                create_instance(lp->desc->name, lp->desc->name);
            }
        }
        if (keys)
            g_strfreev(keys);
        return;
    }
    for (gsize i = 0; i < n; i++) {
        char *type = g_key_file_get_string(cf, "instances", keys[i], NULL);

        if (type && type[0])
            create_instance(type, keys[i]);
        else
            create_instance(keys[i], keys[i]); /* значение пусто: имя = тип */
        g_free(type);
    }
    g_strfreev(keys);
}

/* Записать текущий набор инстансов обратно в [instances] (после
 * add/delete, чтобы список переживал перезапуск). */
void xs_core_save_instances(void)
{
    GKeyFile *cf = xs_core_conf();
    gsize n = xs_core_plugin_count();

    g_key_file_remove_group(cf, "instances", NULL);
    for (gsize i = 0; i < n; i++) {
        XsPlugin *p = xs_core_plugin_at(i);

        if (p && p->name && xs_core_plugin_type(p))
            g_key_file_set_string(cf, "instances", p->name,
                                  xs_core_plugin_type(p));
    }
    xs_core_conf_flush();
}

int main(int argc, char **argv)
{
    g_free(g_conf_path);
    g_conf_path = g_build_filename(g_get_user_config_dir(),
                                   "xscreenlets", "xscreenletsd.conf", NULL);
    /* Плагины по умолчанию — наш install-каталог (/usr/lib64/... доступен
     * через --plugdir при системной установке). */
    g_plugdir = g_build_filename(g_get_home_dir(), "lib", "xscreenlets",
                                 "plugins", NULL);

    for (int i = 1; i < argc; i++) {
        if (g_strcmp0(argv[i], "--conf") == 0 && i + 1 < argc) {
            g_free(g_conf_path);
            g_conf_path = g_strdup(argv[++i]);
        } else if (g_strcmp0(argv[i], "--plugdir") == 0 && i + 1 < argc) {
            g_free(g_plugdir);
            g_plugdir = g_strdup(argv[++i]);
        } else if (g_strcmp0(argv[i], "--debug") == 0) {
            xs_core_set_debug(TRUE);
        }
    }

    /* SIGHUP reload setup */
    setup_sighup_handler();

    gtk_init(&argc, &argv);
    xs_core_init(g_conf_path);
    xs_tray_init();
    load_plugin_modules();
    create_instances();

    gtk_main();

    xs_core_shutdown_all();
    xs_tray_shutdown();
    uninstall_sighup_handler();
    free_loaded_modules();
    g_free(g_conf_path);
    g_free(g_plugdir);
    return 0;
}
