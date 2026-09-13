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
    for (gsize i = 0; i < g_loaded_modules->len; i++)
        g_module_close(g_ptr_array_index(g_loaded_modules, i));
    g_ptr_array_free(g_loaded_modules, TRUE);
    g_loaded_modules = NULL;
}

void scan_plugins(void)
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
        XsPluginDesc *desc;
        XsPlugin *p;

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
        if (!g_module_symbol(mod, "xs_plugin_desc", (gpointer *)&fn_desc)) {
            xs_log_impl("load %s: no xs_plugin_desc", path);
            g_module_close(mod);
            g_free(path);
            continue;
        }
        desc = fn_desc();
        if (!desc || desc->api_version != XS_API_VERSION) {
            xs_log_impl("load %s: api_version mismatch (%u != %u)",
                       path, desc ? desc->api_version : 0, XS_API_VERSION);
            g_module_close(mod);
            g_free(path);
            continue;
        }
        p = g_new0(XsPlugin, 1);
        p->name = g_strdup(desc->name);
        p->host = xs_host_api();
        p->ops = desc->ops;
        p->desc = desc->desc;
        p->author = desc->author;
        p->version = desc->version;
        p->priv = NULL;
        if (p->ops && p->ops->init) {
            GKeyFile *kf = xs_core_plugin_conf(p->name);
            if (p->ops->init(p, kf) != 0) {
                xs_log_impl("load %s: init failed", p->name);
                g_free((char *)p->name);
                g_free(p);
                g_module_close(mod);
                g_free(path);
                continue;
            }
        }
        if (!g_loaded_modules)
            g_loaded_modules = g_ptr_array_new();
        g_ptr_array_add(g_loaded_modules, mod);
        xs_core_register_plugin(p);
        xs_tray_add_plugin(p);
        xs_tray_rebuild();
        xs_log_impl("loaded %s (api %u)", p->name, desc->api_version);
        g_free(path);
    }
    g_dir_close(d);
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
    scan_plugins();

    gtk_main();

    xs_core_shutdown_all();
    xs_tray_shutdown();
    uninstall_sighup_handler();
    free_loaded_modules();
    g_free(g_conf_path);
    g_free(g_plugdir);
    return 0;
}