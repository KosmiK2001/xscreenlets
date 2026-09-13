#define _POSIX_C_SOURCE 200809L
#include "xs_api.h"
#include "common.h"

#include <gtk/gtk.h>
#include <glib.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <stdio.h>

/* Declare the plugin descriptor from clock.c */
extern XsPluginDesc *xs_plugin_desc(void);

int main(int argc, char **argv)
{
    char *conf_path;
    XsPluginDesc *desc;
    XsPlugin *p;
    int fd;

    gtk_init(&argc, &argv);

    /* Create a temporary config file so the core can use xs_core_conf(). */
    conf_path = g_build_filename(g_get_tmp_dir(),
                                 "xscreenlets-standalone-XXXXXX", NULL);
    fd = g_mkstemp(conf_path);
    if (fd < 0) {
        g_printerr("standalone: cannot create temp config %s\n", conf_path);
        g_free(conf_path);
        return 1;
    }
    close(fd);

    xs_core_init(conf_path);

    desc = xs_plugin_desc();
    if (!desc || desc->api_version != XS_API_VERSION) {
        g_printerr("standalone: plugin descriptor missing or api mismatch\n");
        remove(conf_path);
        g_free(conf_path);
        xs_core_shutdown_all();
        return 1;
    }

    p = g_new0(XsPlugin, 1);
    p->name = g_strdup(desc->name);
    p->host = xs_host_api();
    p->ops = desc->ops;
    p->desc = desc->desc;
    p->author = desc->author;
    p->version = desc->version;
    p->priv = NULL;

    xs_core_register_plugin(p);

    if (p->ops && p->ops->init) {
        GKeyFile *kf = xs_core_plugin_conf(p->name);
        if (p->ops->init(p, kf) != 0) {
            g_printerr("standalone: plugin init failed\n");
            xs_core_unregister_plugin(p);
            xs_core_free_plugin(p);
            remove(conf_path);
            g_free(conf_path);
            xs_core_shutdown_all();
            return 1;
        }
    }

    gtk_main();

    xs_core_shutdown_all();
    remove(conf_path);
    g_free(conf_path);
    return 0;
}