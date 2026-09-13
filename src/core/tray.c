#include "tray.h"
#include "common.h"

/* Tray intentionally uses the deprecated GtkStatusIcon API as required by the
 * specification; silence the corresponding -Wdeprecated-declarations warnings
 * for this translation unit so the build stays clean under -Wall -Wextra. */
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wdeprecated-declarations"

#include <gtk/gtk.h>
#include <glib.h>

static GtkStatusIcon *g_tray = NULL;
static GtkMenu *g_menu = NULL;

static void on_reload_activate(GtkMenuItem *mi, gpointer data)
{
    (void)mi;
    (void)data;
    xs_core_reload();
}

static void on_quit_activate(GtkMenuItem *mi, guint data)
{
    (void)mi;
    (void)data;
    gtk_main_quit();
}

static void on_toggle_activate(GtkMenuItem *mi, gpointer data)
{
    XsPlugin *p = data;
    gboolean active = gtk_check_menu_item_get_active(GTK_CHECK_MENU_ITEM(mi));
    xs_core_show_plugin(p, active);
}

static void on_popup_menu(GtkStatusIcon *icon, guint button,
                          guint32 activate_time, guint data)
{
    (void)icon;
    (void)data;
    if (g_menu) {
        gtk_menu_popup(g_menu, NULL, NULL,
                       gtk_status_icon_position_menu, icon,
                       button, activate_time);
    }
}

void xs_tray_init(void)
{
    g_tray = gtk_status_icon_new();
    if (!g_tray) {
        xs_log_impl("tray: failed to create GtkStatusIcon, continuing without tray");
        return;
    }
    gtk_status_icon_set_from_icon_name(g_tray, "xscreenlets");
    gtk_status_icon_set_tooltip_text(g_tray, "Xscreenlets");
    g_object_ref_sink(g_tray);
    g_menu = GTK_MENU(gtk_menu_new());
    g_object_ref_sink(g_menu);
    gtk_status_icon_set_visible(g_tray, TRUE);
    g_signal_connect(g_tray, "popup-menu", G_CALLBACK(on_popup_menu), 0);
}

void xs_tray_add_plugin(XsPlugin *p)
{
    if (!g_menu || !p)
        return;
    GtkCheckMenuItem *mi = GTK_CHECK_MENU_ITEM(
        gtk_check_menu_item_new_with_label(p->name));
    gtk_check_menu_item_set_active(mi, TRUE);
    g_signal_connect(mi, "toggled", G_CALLBACK(on_toggle_activate), p);
    gtk_menu_shell_append(GTK_MENU_SHELL(g_menu), GTK_WIDGET(mi));
    gtk_widget_show_all(GTK_WIDGET(mi));
}

void xs_tray_remove_plugin(XsPlugin *p)
{
    if (!g_menu || !p)
        return;
    GList *children = gtk_container_get_children(GTK_CONTAINER(g_menu));
    for (GList *l = children; l; l = l->next) {
        GtkMenuItem *mi = l->data;
        if (gtk_menu_item_get_label(mi) &&
            g_strcmp0(gtk_menu_item_get_label(mi), p->name) == 0) {
            gtk_container_remove(GTK_CONTAINER(g_menu), GTK_WIDGET(mi));
            break;
        }
    }
    g_list_free(children);
}

void xs_tray_set_plugin_visibility(XsPlugin *p, gboolean visible)
{
    if (!g_menu || !p)
        return;
    GList *children = gtk_container_get_children(GTK_CONTAINER(g_menu));
    for (GList *l = children; l; l = l->next) {
        GtkMenuItem *mi = l->data;
        if (gtk_menu_item_get_label(mi) &&
            g_strcmp0(gtk_menu_item_get_label(mi), p->name) == 0) {
            GtkCheckMenuItem *mi_check = GTK_CHECK_MENU_ITEM(mi);
            gtk_check_menu_item_set_active(mi_check, visible);
            break;
        }
    }
    g_list_free(children);
}

void xs_tray_rebuild(void)
{
    if (!g_menu)
        return;
    GList *children = gtk_container_get_children(GTK_CONTAINER(g_menu));
    for (GList *l = children; l; l = l->next) {
        GtkMenuItem *mi = l->data;
        gtk_container_remove(GTK_CONTAINER(g_menu), GTK_WIDGET(mi));
    }
    g_list_free(children);

    for (gsize i = 0; i < xs_core_plugin_count(); i++) {
        XsPlugin *p = xs_core_plugin_at(i);
        if (p)
            xs_tray_add_plugin(p);
    }

    GtkWidget *sep = gtk_separator_menu_item_new();
    GtkWidget *reload = gtk_menu_item_new_with_label("Reload");
    GtkWidget *quit = gtk_menu_item_new_with_label("Quit");
    g_signal_connect(reload, "activate", G_CALLBACK(on_reload_activate), NULL);
    g_signal_connect(quit, "activate", G_CALLBACK(on_quit_activate), NULL);
    gtk_menu_shell_append(GTK_MENU_SHELL(g_menu), sep);
    gtk_menu_shell_append(GTK_MENU_SHELL(g_menu), reload);
    gtk_menu_shell_append(GTK_MENU_SHELL(g_menu), quit);
    gtk_widget_show_all(GTK_WIDGET(g_menu));
}

void xs_tray_shutdown(void)
{
    if (g_tray) {
        g_object_unref(g_tray);
        g_tray = NULL;
    }
    if (g_menu) {
        g_object_unref(g_menu);
        g_menu = NULL;
    }
}