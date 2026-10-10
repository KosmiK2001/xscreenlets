#include "tray.h"
#include "common.h"

/* Tray intentionally uses the deprecated GtkStatusIcon API as required by the
 * specification; silence the corresponding -Wdeprecated-declarations warnings
 * for this translation unit so the build stays clean under -Wall -Wextra. */
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wdeprecated-declarations"

#include <gtk/gtk.h>
#include <glib.h>

#include "i18n.h"
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
static void on_applet_mgmt_activate(GtkMenuItem *mi, gpointer data)
{
    (void)mi;
    (void)data;
    xs_applet_manager_show();
}

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

/* Кошельки для донатов — общий список с About-страницей в common.c
 * (xs_core_about_page). */
static const struct { const char *coin; const char *addr; } tray_donates[] = {
    { "Bitcoin",  "bc1qfmr3ztpvsxjq0qpjd4xr4xujk5jq2jy5pntcgd" },
    { "Litecoin", "ltc1qqdnp7h4dfc5w0sj02uhpncl9qrgt57p583r3hp" },
    { "Solana",   "BDam16tXGmAqE3y8GncVCCzmiu1AxZwjYHQHGkyXNjEE" },
    { "Ethereum", "0xc6817e25b4d4283878aec8309b8df542a342a7f4" },
    { "Gridcoin", "SJcosJ2xaspR7GKjFi1WZ23AerhANVmjcf" },
};

/* Кастомный About вместо gtk_about_dialog: штатный диалог не умеет
 * добавлять свою секцию credits («Благодарности» в нём — только
 * authors/documenters/artists/translators, donate-секции нет), а валить
 * кошельки в License семантически неверно. Делаем свой диалог С ТЕМ ЖЕ
 * видом, что у gtk_about_dialog: переключаемые вкладки Credits/License,
 * донаты — во вкладке «Благодарности», в License — настоящая MIT. */
static void on_about_activate(GtkMenuItem *mi, gpointer data)
{
    (void)mi;
    (void)data;
    {
        GtkWidget *dlg;
        GtkWidget *vbox;
        GtkWidget *lbl;
        GtkWidget *nb;
        GtkWidget *sw;
        GtkWidget *cred;
        GtkWidget *dbox;
        size_t i;
        char *m;

        dlg = gtk_dialog_new_with_buttons(_("About Xscreenlets"), NULL,
                                          0,
                                          "Close", GTK_RESPONSE_CLOSE,
                                          NULL);
        gtk_window_set_default_size(GTK_WINDOW(dlg), 420, 380);
        gtk_container_set_border_width(GTK_CONTAINER(dlg), 10);
        vbox = gtk_box_new(GTK_ORIENTATION_VERTICAL, 6);
        gtk_container_add(GTK_CONTAINER(gtk_dialog_get_content_area(
                              GTK_DIALOG(dlg))), vbox);

        /* Шапка — как в gtk_about_dialog: имя/версия/описание/(c) */
        lbl = gtk_label_new(NULL);
        gtk_label_set_markup(GTK_LABEL(lbl),
                             "<b><span size=\"x-large\">Xscreenlets</span></b> "
                             "<span size=\"large\">0.0.3</span>\n"
                             "C/GTK3 replacement for python2 screenlets "
                             "(daemon + gmodule plugins)\n"
                             "<span size=\"small\">(c) kosmik2001 "
                             "&lt;kosmik2001@gmail.com&gt;</span>");
        gtk_widget_set_halign(lbl, GTK_ALIGN_CENTER);
        gtk_box_pack_start(GTK_BOX(vbox), lbl, FALSE, FALSE, 4);

        /* Переключаемые вкладки — как в штатном About */
        nb = gtk_notebook_new();
        gtk_box_pack_start(GTK_BOX(vbox), nb, TRUE, TRUE, 0);

        /* Вкладка «Благодарности»: donate-адреса, выделяемые */
        dbox = gtk_box_new(GTK_ORIENTATION_VERTICAL, 4);
        gtk_container_set_border_width(GTK_CONTAINER(dbox), 8);

        /* Почта до адресов: тоже выделяемая, чтобы можно было скопировать */
        {
            GtkWidget *mail = gtk_label_new("kosmik2001@gmail.com");

            gtk_label_set_selectable(GTK_LABEL(mail), TRUE);
            gtk_label_set_xalign(GTK_LABEL(mail), 0.0f);
            gtk_box_pack_start(GTK_BOX(dbox), mail, FALSE, FALSE, 2);
            gtk_box_pack_start(GTK_BOX(dbox), gtk_separator_new(
                                   GTK_ORIENTATION_HORIZONTAL),
                               FALSE, FALSE, 4);
        }
        for (i = 0; i < G_N_ELEMENTS(tray_donates); i++) {
            GtkWidget *row = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 6);
            GtkWidget *cl = gtk_label_new(NULL);
            GtkWidget *cv;
            char *e;

            e = g_markup_escape_text(tray_donates[i].coin, -1);
            m = g_strdup_printf("<b>%s:</b>", e);
            gtk_label_set_markup(GTK_LABEL(cl), m);
            g_free(m);
            g_free(e);
            cv = gtk_label_new(tray_donates[i].addr);
            gtk_label_set_selectable(GTK_LABEL(cv), TRUE);
            gtk_label_set_ellipsize(GTK_LABEL(cv), PANGO_ELLIPSIZE_END);
            gtk_label_set_max_width_chars(GTK_LABEL(cv), 40);
            gtk_label_set_xalign(GTK_LABEL(cv), 0.0f);
            gtk_box_pack_start(GTK_BOX(row), cl, FALSE, FALSE, 0);
            gtk_box_pack_start(GTK_BOX(row), cv, TRUE, TRUE, 0);
            gtk_box_pack_start(GTK_BOX(dbox), row, FALSE, FALSE, 2);
        }
        gtk_notebook_append_page(GTK_NOTEBOOK(nb), dbox,
                                 gtk_label_new(_("Thanks")));

        /* Вкладка «License»: настоящий текст MIT */
        cred = gtk_label_new(NULL);
        gtk_label_set_markup(GTK_LABEL(cred),
            "MIT License\n\n"
            "Copyright (c) 2026 kosmik2001\n\n"
            "Permission is hereby granted, free of charge, to any person "
            "obtaining a copy of this software and associated documentation "
            "files (the \"Software\"), to deal in the Software without "
            "restriction, including without limitation the rights to use, "
            "copy, modify, merge, publish, distribute, sublicense, and/or "
            "sell copies of the Software, and to permit persons to whom the "
            "Software is furnished to do so, subject to the following "
            "conditions:\n\n"
            "The above copyright notice and this permission notice shall be "
            "included in all copies or substantial portions of the "
            "Software.\n\n"
            "THE SOFTWARE IS PROVIDED \"AS IS\", WITHOUT WARRANTY OF ANY "
            "KIND, EXPRESS OR IMPLIED, INCLUDING BUT NOT LIMITED TO THE "
            "WARRANTIES OF MERCHANTABILITY, FITNESS FOR A PARTICULAR "
            "PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE AUTHORS OR "
            "COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER "
            "LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR "
            "OTHERWISE, ARISING FROM, OUT OF OR IN CONNECTION WITH THE "
            "SOFTWARE OR THE USE OR OTHER DEALINGS IN THE SOFTWARE.");
        gtk_label_set_selectable(GTK_LABEL(cred), TRUE);
        gtk_label_set_xalign(GTK_LABEL(cred), 0.0f);
        gtk_label_set_line_wrap(GTK_LABEL(cred), TRUE);
        sw = gtk_scrolled_window_new(NULL, NULL);
        gtk_scrolled_window_set_policy(GTK_SCROLLED_WINDOW(sw),
                                       GTK_POLICY_NEVER,
                                       GTK_POLICY_AUTOMATIC);
        gtk_container_set_border_width(GTK_CONTAINER(sw), 8);
        gtk_container_add(GTK_CONTAINER(sw), cred);
        gtk_notebook_append_page(GTK_NOTEBOOK(nb), sw,
                                 gtk_label_new(_("License")));

        gtk_widget_show_all(vbox);
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
    gboolean g_tray_icon_set = FALSE;

    g_tray = gtk_status_icon_new();
    if (!g_tray) {
        xs_log_impl("tray: failed to create GtkStatusIcon, continuing without tray");
        return;
    }
    /* Значок трея: сначала собственный из пакета
     * (/usr/share/icons/xscreenlets/screenlets-tray.png — его ставит
     * ebuild вместе со всеми иконками из icons/). Раньше здесь был
     * жёсткий путь /usr/share/icons/screenlets.svg, но этот файл
     * принадлежал python2-пакету screenlets и на машине без него
     * демон падал в fallback на имя "xscreenlets", которого в теме
     * нет — трей оставался пустым.
     *
     * .svg оставлен вторым: на части систем сохраняется старый значок,
     * и переходить на него молча не стоит. */
    {
        static const char *tray_icons[] = {
            "/usr/share/icons/xscreenlets/screenlets-tray.png",
            /* Обёртка того же значка: ebuild ставит иконки циклом
             * icons/*.svg, поэтому растр в пакет не попадает, а
             * положить его в git можно только в виде SVG. Правку
             * ebuild вносит пользователь, поэтому демон сначала
             * пробует .png, а если его нет — эту обёртку.
             * Когда ebuild начнёт ставить *.png, её можно убрать. */
            "/usr/share/icons/xscreenlets/screenlets-tray.svg",
            "/usr/share/icons/screenlets.svg",
            NULL
        };
        int i;

        for (i = 0; tray_icons[i] && !g_tray_icon_set; i++) {
            GdkPixbuf *pb;

            /* Проверять нужно не существование файла, а то, что GTK его
             * РЕАЛЬНО ДЕКОДИРУЕТ. g_file_test() проходит по битому
             * screenlets-tray.svg, gtk_status_icon_set_from_file() на нём
             * молча не показывает ничего, и цикл уходит на следующего
             * кандидата — в итоге в трее оказывался старый значок от
             * python2-пакета. Такое случилось с обёрткой: xmllint и
             * rsvg-convert такой файл принимают, а gdk-pixbuf — нет.
             *
             * Декодируем заранее и отбрасываем пиксель только как
             * ПРОВЕРКУ; сам значок ставим через set_from_file — на этой
             * машине set_from_pixbuf не создавал embed-окно трея вовсе. */
            pb = gdk_pixbuf_new_from_file(tray_icons[i], NULL);
            if (pb) {
                g_object_unref(pb);
                gtk_status_icon_set_from_file(g_tray, tray_icons[i]);
                g_tray_icon_set = TRUE;
            } else {
                g_warning("xscreenlets: трей-значок %s не читается, "
                          "пробуем следующий", tray_icons[i]);
            }
        }
        if (!g_tray_icon_set)
            gtk_status_icon_set_from_icon_name(g_tray, "xscreenlets");
    }
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
    /* Формат переводится, значения - нет: name это тип апплета,
     * lab это user_label из конфига, то есть то, что пользователь
     * сам вписал. Переводить их нельзя. */
    label = g_strdup_printf(_("%1$s: %2$s"), name, lab);
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

            /* Сравниваем с ПЕРЕВОДЁННЫМ заголовком: пункт создаётся
             * как _("Running Instances"), значит и искать надо
             * _("Running Instances"), а не английский литерал. С
             * литералом сравнение всегда ложное, и чекбоксы
             * экземпляров не появлялись бы в меню. */
            if (gtk_menu_item_get_label(it) &&
                g_strcmp0(gtk_menu_item_get_label(it),
                           _("Running Instances")) == 0) {
                run_mi = GTK_WIDGET(it);
                break;
            }
        }
        g_list_free(children);
    }
    if (!run_mi) {
        run_mi = gtk_menu_item_new_with_label(_("Running Instances"));
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
        if (lbl && g_strcmp0(lbl, _("Running Instances")) == 0) {
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

    /* --- Applet management: окно с тремя вкладками --- */
    {
        GtkWidget *am = gtk_menu_item_new_with_label(_("Applet management"));

        g_signal_connect(am, "activate",
                         G_CALLBACK(on_applet_mgmt_activate), NULL);
        gtk_menu_shell_append(GTK_MENU_SHELL(g_menu), am);
    }

    GtkWidget *sep = gtk_separator_menu_item_new();
    GtkWidget *restart = gtk_menu_item_new_with_label(_("Restart Applets"));
    GtkWidget *stopall = gtk_menu_item_new_with_label(_("Stop all Applets"));
    GtkWidget *sep2 = gtk_separator_menu_item_new();
    GtkWidget *about = gtk_menu_item_new_with_label(_("About"));
    GtkWidget *quit = gtk_menu_item_new_with_label(_("Quit"));

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