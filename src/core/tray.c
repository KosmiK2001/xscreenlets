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

/* --- callbacks --- */

static void on_quit_activate(GtkMenuItem *mi, gpointer data)
{
    (void)mi;
    (void)data;
    xs_core_shutdown_all();
    gtk_main_quit();
}

static void on_toggle_activate(GtkMenuItem *mi, gpointer data)
{
    XsPlugin *p = data;

    xs_core_show_plugin(p,
                        gtk_check_menu_item_get_active(GTK_CHECK_MENU_ITEM(mi)));
}

/* Launch <тип>: создать инстанс типа (как launch_screenlet в оригинале) */
static void on_launch_activate(GtkMenuItem *mi, gpointer data)
{
    const char *type = g_object_get_data(G_OBJECT(mi), "xs-type");

    (void)data;
    if (type)
        xs_core_add_instance(type);
}

/* Restart all: пересоздать все инстансы из [instances] (как
 * restart_all_screenlets: всё закрыть и запустить заново) */
static void on_restart_activate(GtkMenuItem *mi, gpointer data)
{
    (void)mi;
    (void)data;
    xs_core_save_instances();
    xs_core_reload();
}

/* Stop all: закрыть окна всех инстансов, но НЕ удалять их из [instances] —
 * после Restart/перезапуска демона апплеты вернутся (как quit_all_screenlets
 * + автостарт) */
static void on_stop_all_activate(GtkMenuItem *mi, gpointer data)
{
    gsize n;

    (void)mi;
    (void)data;
    xs_core_save_instances();
    n = xs_core_plugin_count();
    for (gsize i = 0; i < n; i++) {
        XsPlugin *p = xs_core_plugin_at(i);

        if (p)
            xs_core_show_plugin(p, FALSE);
    }
}

static void on_about_activate(GtkMenuItem *mi, gpointer data)
{
    (void)mi;
    (void)data;
    {
        GtkWidget *dlg = gtk_about_dialog_new();
        const char *authors[] = { "kosmik2001 (Hermes agents)", NULL };

        gtk_about_dialog_set_program_name(GTK_ABOUT_DIALOG(dlg),
                                          "Xscreenlets");
        gtk_about_dialog_set_version(GTK_ABOUT_DIALOG(dlg), "0.2");
        gtk_about_dialog_set_comments(GTK_ABOUT_DIALOG(dlg),
                                      "C/GTK3 replacement for python2 "
                                      "screenlets (daemon + gmodule plugins)");
        gtk_about_dialog_set_authors(GTK_ABOUT_DIALOG(dlg), authors);
        gtk_window_present(GTK_WINDOW(dlg));
        g_signal_connect(dlg, "response", G_CALLBACK(gtk_widget_destroy),
                         NULL);
    }
}

static void on_popup_menu(GtkStatusIcon *icon, guint button,
                          guint32 activate_time, gpointer data)
{
    (void)icon;
    (void)data;
    if (g_menu)
        gtk_menu_popup(g_menu, NULL, NULL,
                       gtk_status_icon_position_menu, icon,
                       button, activate_time);
}

/* --- меню --- */

void xs_tray_init(void)
{
    g_tray = gtk_status_icon_new();
    if (!g_tray) {
        xs_log_impl("tray: failed to create GtkStatusIcon, continuing without tray");
        return;
    }
    /* Иконка "xscreenlets" в теме отсутствует — берём svg оригинального
     * screenlets (как в python2-daemon), fallback — icon-name. */
    if (g_file_test("/usr/share/icons/screenlets.svg", G_FILE_TEST_EXISTS))
        gtk_status_icon_set_from_file(g_tray,
                                      "/usr/share/icons/screenlets.svg");
    else
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
    GtkWidget *run_mi, *sub;
    char *label;
    const char *name;
    char *lab;
    GKeyFile *kf;

    if (!g_menu || !p)
        return;
    /* Подпись пункта: "Plugin: Label" (тип + user_label из конфига). */
    name = xs_core_plugin_type(p) ? xs_core_plugin_type(p) : p->name;
    lab = NULL;
    kf = xs_core_plugin_conf(p->name);
    if (kf)
        lab = g_key_file_get_string(kf, p->name, "user_label", NULL);
    if (!lab || !lab[0])
        lab = g_strdup("blank_label");
    label = g_strdup_printf("%s: %s", name, lab);
    g_free(lab);
    GtkCheckMenuItem *mi = GTK_CHECK_MENU_ITEM(
        gtk_check_menu_item_new_with_label(label));
    g_free(label);
    g_object_set_data(G_OBJECT(mi), "xs-plugin", p);
    gboolean visible = p->win && gtk_widget_get_visible(p->win);

    gtk_check_menu_item_set_active(mi, visible);
    g_signal_connect(mi, "toggled", G_CALLBACK(on_toggle_activate), p);
    /* Чекбокс инстанса живёт в подменю "Running Instances": находим его
     * в корневом меню (создаём при первом инстансе) и добавляем туда. */
    run_mi = NULL;
    {
        GList *children = gtk_container_get_children(GTK_CONTAINER(g_menu));

        for (GList *l = children; l; l = l->next) {
            GtkMenuItem *it = l->data;

            if (gtk_menu_item_get_label(it) &&
                g_strcmp0(gtk_menu_item_get_label(it),
                          "Running Instances") == 0) {
                run_mi = GTK_WIDGET(it);
                break;
            }
        }
        g_list_free(children);
    }
    if (!run_mi) {
        run_mi = gtk_menu_item_new_with_label("Running Instances");
        sub = gtk_menu_new();
        gtk_menu_item_set_submenu(GTK_MENU_ITEM(run_mi), sub);
        gtk_menu_shell_append(GTK_MENU_SHELL(g_menu), run_mi);
    }
    sub = gtk_menu_item_get_submenu(GTK_MENU_ITEM(run_mi));
    gtk_menu_shell_append(GTK_MENU_SHELL(sub), GTK_WIDGET(mi));
    gtk_widget_show_all(run_mi);
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
        const char *lbl = gtk_menu_item_get_label(mi);

        /* Чекбоксы живут в подменю "Running Instances" с подписью
         * "Plugin: Label" — ищем по подменю, а не по корню. */
        if (lbl && g_strcmp0(lbl, "Running Instances") == 0) {
            GtkWidget *sub = gtk_menu_item_get_submenu(mi);
            GList *schildren;

            if (!sub)
                continue;
            schildren = gtk_container_get_children(GTK_CONTAINER(sub));
            for (GList *sl = schildren; sl; sl = sl->next) {
                if (g_object_get_data(G_OBJECT(sl->data),
                                      "xs-plugin") == p) {
                    gtk_check_menu_item_set_active(
                        GTK_CHECK_MENU_ITEM(sl->data), visible);
                    break;
                }
            }
            g_list_free(schildren);
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
        gtk_container_remove(GTK_CONTAINER(g_menu), GTK_WIDGET(l->data));
    }
    g_list_free(children);

    /* --- Launch <тип>: все загруженные типы плагинов --- */
    {
        GtkWidget *launch_mi = gtk_menu_item_new_with_label("Launch Applet");
        GtkWidget *sub = gtk_menu_new();
        GHashTable *types = g_hash_table_new(g_str_hash, g_str_equal);
        GList *sorted = NULL;

        for (gsize i = 0; i < xs_core_plugin_count(); i++) {
            XsPlugin *p = xs_core_plugin_at(i);
            const char *t = p ? xs_core_plugin_type(p) : NULL;

            if (t && !g_hash_table_contains(types, t)) {
                g_hash_table_add(types, (gpointer)t);
                sorted = g_list_prepend(sorted, (gpointer)t);
            }
        }
        sorted = g_list_sort(sorted, (GCompareFunc)strcmp);
        for (GList *l = sorted; l; l = l->next) {
            const char *t = l->data;
            GtkWidget *it = gtk_menu_item_new_with_label(t);

            g_object_set_data_full(G_OBJECT(it), "xs-type",
                                   g_strdup(t), g_free);
            g_signal_connect(it, "activate",
                             G_CALLBACK(on_launch_activate), NULL);
            gtk_menu_shell_append(GTK_MENU_SHELL(sub), it);
        }
        g_list_free(sorted);
        g_hash_table_destroy(types);
        gtk_menu_item_set_submenu(GTK_MENU_ITEM(launch_mi), sub);
        gtk_menu_shell_append(GTK_MENU_SHELL(g_menu), launch_mi);
    }

    /* --- Running Instances: подменю наполнит xs_tray_add_plugin --- */
    {
        GtkWidget *run_mi = gtk_menu_item_new_with_label("Running Instances");
        GtkWidget *sub = gtk_menu_new();

        gtk_menu_item_set_submenu(GTK_MENU_ITEM(run_mi), sub);
        gtk_menu_shell_append(GTK_MENU_SHELL(g_menu), run_mi);
        for (gsize i = 0; i < xs_core_plugin_count(); i++) {
            XsPlugin *p = xs_core_plugin_at(i);
            GKeyFile *kf;
            char *sb;

            if (!p || !p->name)
                continue;
            /* гость рамки (started_by=plugin) не показывается:
             * его запуском управляет хозяин, не демон */
            kf = xs_core_plugin_conf(p->name);
            sb = kf ? g_key_file_get_string(kf, p->name,
                                            "started_by", NULL) : NULL;
            if (sb && strcmp(sb, "main_daemon") != 0) {
                g_free(sb);
                continue;
            }
            g_free(sb);
            xs_tray_add_plugin(p);
        }
    }

    GtkWidget *sep = gtk_separator_menu_item_new();
    GtkWidget *restart = gtk_menu_item_new_with_label("Restart Applets");
    GtkWidget *stopall = gtk_menu_item_new_with_label("Stop all Applets");
    GtkWidget *sep2 = gtk_separator_menu_item_new();
    GtkWidget *about = gtk_menu_item_new_with_label("About");
    GtkWidget *quit = gtk_menu_item_new_with_label("Quit");

    g_signal_connect(restart, "activate", G_CALLBACK(on_restart_activate),
                     NULL);
    g_signal_connect(stopall, "activate", G_CALLBACK(on_stop_all_activate),
                     NULL);
    g_signal_connect(about, "activate", G_CALLBACK(on_about_activate), NULL);
    g_signal_connect(quit, "activate", G_CALLBACK(on_quit_activate), NULL);
    gtk_menu_shell_append(GTK_MENU_SHELL(g_menu), sep);
    gtk_menu_shell_append(GTK_MENU_SHELL(g_menu), restart);
    gtk_menu_shell_append(GTK_MENU_SHELL(g_menu), stopall);
    gtk_menu_shell_append(GTK_MENU_SHELL(g_menu), sep2);
    gtk_menu_shell_append(GTK_MENU_SHELL(g_menu), about);
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