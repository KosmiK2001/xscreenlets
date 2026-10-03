/* ==== Applet Management: окно с тремя вкладками ====
 *
 * 1. «Applets»    — значки всех загруженных типов: Запустить,
 *                   Сбросить настройки (удалить конфиги инстансов типа)
 * 2. «Running»    — дерево запущенных с вложенностью гостей (guests_N)
 * 3. «Configs»    — все конфиги: автозапуск (symlink в plugins_on,
 *                   только для корневых), удалить, редактировать
 *
 * Вызывается пунктом меню «Applet management» (tray.c).
 */
#include "common.h"
#include <gtk/gtk.h>

#include "i18n.h"
static GtkWidget *am_window = NULL;
static GtkNotebook *am_notebook = NULL;

static void am_refresh_running_tree(void);

/* ============ вспомогательные ============ */

static char **am_list_conf_names(int *count)
{
    GDir *dir;
    char **out;
    int n = 0, cap = 16;
    const char *fn;
    const char *confdir = xs_core_plugins_dir();

    *count = 0;
    if (!confdir)
        return g_new0(char *, 1);
    dir = g_dir_open(confdir, 0, NULL);
    if (!dir)
        return g_new0(char *, 1);
    out = g_new(char *, cap);
    while ((fn = g_dir_read_name(dir)) != NULL) {
        if (!g_str_has_suffix(fn, ".conf"))
            continue;
        if (n == cap - 1) {
            cap *= 2;
            out = g_realloc(out, cap * sizeof(char *));
        }
        out[n++] = g_strndup(fn, strlen(fn) - 5);
    }
    g_dir_close(dir);
    out[n] = NULL;
    *count = n;
    return out;
}

/* конфиг: значение ключа (без создания кэша) */
static char *am_conf_get(const char *inst, const char *key)
{
    GKeyFile *kf = xs_core_plugin_conf(inst);

    return kf ? g_key_file_get_string(kf, inst, key, NULL) : NULL;
}

static int am_conf_get_int(const char *inst, const char *key, int d)
{
    GKeyFile *kf = xs_core_plugin_conf(inst);
    GError *err = NULL;
    int v;

    if (!kf)
        return d;
    v = g_key_file_get_integer(kf, inst, key, &err);
    if (err) {
        g_error_free(err);
        return d;
    }
    return v;
}

/* есть ли у инстанса symlink автозапуска */
static gboolean am_autostart_enabled(const char *inst)
{
    char *nm = g_strdup_printf("%s.conf", inst);
    char *lnk = g_build_filename(xs_core_onoff_dir(), nm, NULL);
    gboolean on = g_file_test(lnk, G_FILE_TEST_IS_SYMLINK) &&
                  g_file_test(lnk, G_FILE_TEST_EXISTS);

    g_free(nm);

    g_free(lnk);
    return on;
}

static void am_autostart_set(const char *inst, gboolean on)
{
    char *nm = g_strdup_printf("%s.conf", inst);
    char *lnk = g_build_filename(xs_core_onoff_dir(), nm, NULL);

    unlink(lnk);
    if (on) {
        const char *odir = xs_core_onoff_dir();
        char *dir = g_path_get_dirname(odir);
        char *tn = g_strdup_printf("%s.conf", inst);
        char *target = g_build_filename(dir, ".plugins", tn, NULL);

        if (symlink(target, lnk) != 0)
            g_message("am: symlink %s failed", lnk);
        g_free(target);
        g_free(tn);
        g_free(dir);
    }
    g_free(nm);
    g_free(lnk);
}

/* иконка типа: СВОИ icons/ рядом с плагинами (не зависим от
 * python-версии), затем fallback /usr/share/screenlets/<Type>/ */
static GdkPixbuf *am_type_icon(const char *type, int size)
{
    GdkPixbuf *pb = NULL;
    const char *exts[] = { "svg", "png", NULL };
    int j;
    const char *pd = xs_core_plugdir();

    if (pd && type) {
        char *icons_dir = g_build_filename(pd, "..", "icons", NULL);


        for (j = 0; exts[j] && !pb; j++) {
            char *fname = g_strdup_printf("%s.%s", type, exts[j]);
            char *path = g_build_filename(icons_dir, fname, NULL);
            GError *err = NULL;

            g_free(fname);

            pb = gdk_pixbuf_new_from_file_at_scale(
                path, size, size, TRUE, &err);
            if (err)
                g_error_free(err);
            g_free(path);
        }
        g_free(icons_dir);
    }
    {
        /* Системный каталог иконок пакета: /usr/share/icons/xscreenlets/
         * (плагины живут в /usr/libexec/xscreenlets, поэтому относительный
         * ../icons из plugdir в системной установке не попадает в цель).
         /* Затем legacy-каталог python-версии. Каталога /usr/share/screenlets в
                  * системе нет, и все типы имеют свою иконку в icons/, поэтому
                  * второго поиска не делаем: он молча вернул бы значок-заглушку. */
        static const char *sys_dirs[] = {
            "/usr/share/icons/xscreenlets", NULL
        };
        int i;

        for (i = 0; sys_dirs[i] && !pb; i++) {
            for (j = 0; exts[j] && !pb; j++) {
                char *fname = g_strdup_printf("%s.%s", type, exts[j]);
                char *path = g_build_filename(sys_dirs[i], fname, NULL);
                GError *err = NULL;

                g_free(fname);
                pb = gdk_pixbuf_new_from_file_at_scale(
                    path, size, size, TRUE, &err);
                if (err)
                    g_error_free(err);
                g_free(path);
            }
        }
    }
    if (!pb) {
        xs_log_impl("am: icon fallback for type '%s'",
                    type ? type : "(null)");
        pb = gtk_icon_theme_load_icon(gtk_icon_theme_get_default(),
                                      "application-x-executable",
                                      size, 0, NULL);
    }
    return pb;
}

/* текст-рендерер с заданным размером шрифта */
static GtkCellRenderer *am_text_renderer(double pts)
{
    GtkCellRenderer *r = gtk_cell_renderer_text_new();

    g_object_set(r, "size-points", pts, NULL);
    return r;
}

/* ============ Вкладка 1: Applets (значки типов) ============ */

static void am_launch_type(GtkButton *b, gpointer data)
{
    const char *type = g_object_get_data(G_OBJECT(b), "am-type");

    (void)data;

    xs_core_add_instance(type);
    /* обновить вкладку Running */
    am_refresh_running_tree();
}

/* сброс настроек: удалить ВСЕ конфиги инстансов этого типа? Нет —
 * в оригинале reset сбрасывает настройки апплета на дефолт. У нас
 * у типа может быть много инстансов; сбрасываем настройки
 * ВЫДЕЛЕННОГО инстанса — но на вкладке 1 типы, не инстансы.
 * Практичный смысл: удалить конфиги всех инстансов типа и
 * перезапустить их (создадутся с дефолтами). */
static void am_reset_type(GtkButton *b, gpointer data)
{
    const char *type = g_object_get_data(G_OBJECT(b), "am-type");

    (void)data;
    GtkWidget *dlg;
    int resp;
    int n, i;
    char **names = am_list_conf_names(&n);
    GPtrArray *victims = g_ptr_array_new_with_free_func(g_free);

    for (i = 0; i < n; i++) {
        char *t = am_conf_get(names[i], "xs_type");

        if (t && strcmp(t, type) == 0)
            g_ptr_array_add(victims, g_strdup(names[i]));
        g_free(t);
    }
    g_strfreev(names);

    dlg = gtk_message_dialog_new(GTK_WINDOW(am_window),
                                 GTK_DIALOG_MODAL,
                                 GTK_MESSAGE_QUESTION,
                                 GTK_BUTTONS_OK_CANCEL,
                                 _("Reset the settings of all instances "
                                   "of type %s (%d in total)?\n"
                                   "Their configs will be deleted and the "
                                   "instances restarted with default "
                                   "settings."),
                                 type, victims->len);
    gtk_window_set_title(GTK_WINDOW(dlg), _("Reset settings"));
    resp = gtk_dialog_run(GTK_DIALOG(dlg));
    gtk_widget_destroy(dlg);
    if (resp != GTK_RESPONSE_OK) {
        g_ptr_array_free(victims, TRUE);
        return;
    }
    for (i = 0; i < (int)victims->len; i++) {
        const char *name = g_ptr_array_index(victims, i);
        XsPlugin *p = xs_core_find_instance(name);

        if (p)
            xs_core_delete_instance_full(p, TRUE);
        else {
            char *path = g_strdup_printf(
                "%s/%s.conf", xs_core_plugins_dir(), name);

            unlink(path);
            g_free(path);
            am_autostart_set(name, FALSE);
        }
    }
    g_ptr_array_free(victims, TRUE);
    xs_core_save_instances();
}

static GtkWidget *am_build_types_page(void)
{
    GtkWidget *scroll, *grid, *box, *img, *lbl, *vbox;
    GtkWidget *btn_run, *btn_reset;
    int n, i;
    char **types = xs_core_list_plugin_types();

    scroll = gtk_scrolled_window_new(NULL, NULL);
    gtk_scrolled_window_set_policy(GTK_SCROLLED_WINDOW(scroll),
                                   GTK_POLICY_NEVER,
                                   GTK_POLICY_AUTOMATIC);
    grid = gtk_grid_new();
    gtk_grid_set_row_spacing(GTK_GRID(grid), 12);
    gtk_grid_set_column_spacing(GTK_GRID(grid), 12);
    gtk_container_set_border_width(GTK_CONTAINER(grid), 12);
    gtk_container_add(GTK_CONTAINER(scroll), grid);

    for (n = 0; types && types[n]; n++) {
        ;
    }
    /* сортировка по имени */
    for (i = 0; types && types[i]; i++) {
        int j;

        for (j = i + 1; types[j]; j++) {
            if (strcmp(types[i], types[j]) > 0) {
                char *tmp = types[i];

                types[i] = types[j];
                types[j] = tmp;
            }
        }
    }
    i = 0;
    for (n = 0; types && types[n]; n++) {
        const char *type = types[n];
        GdkPixbuf *pb = am_type_icon(type, 48);

        box = gtk_box_new(GTK_ORIENTATION_VERTICAL, 4);
        gtk_widget_set_halign(box, GTK_ALIGN_CENTER);
        img = gtk_image_new_from_pixbuf(pb);
        if (pb)
            g_object_unref(pb);
        gtk_box_pack_start(GTK_BOX(box), img, FALSE, FALSE, 0);
        lbl = gtk_label_new(type);
        gtk_box_pack_start(GTK_BOX(box), lbl, FALSE, FALSE, 0);

        btn_run = gtk_button_new_with_label(_("Start"));
        g_object_set_data_full(G_OBJECT(btn_run), "am-type",
                               g_strdup(type), g_free);
        g_signal_connect(btn_run, "clicked",
                         G_CALLBACK(am_launch_type), NULL);
        gtk_box_pack_start(GTK_BOX(box), btn_run, FALSE, FALSE, 0);

        btn_reset = gtk_button_new_with_label(_("Reset settings"));
        g_object_set_data_full(G_OBJECT(btn_reset), "am-type",
                               g_strdup(type), g_free);
        g_signal_connect(btn_reset, "clicked",
                         G_CALLBACK(am_reset_type), NULL);
        gtk_box_pack_start(GTK_BOX(box), btn_reset, FALSE, FALSE, 0);

        vbox = gtk_box_new(GTK_ORIENTATION_VERTICAL, 0);
        gtk_box_pack_start(GTK_BOX(vbox), box, TRUE, TRUE, 0);
        gtk_grid_attach(GTK_GRID(grid), vbox, i % 5, i / 5, 1, 1);
        i++;
    }
    g_strfreev(types);
    return scroll;
}

/* ============ Вкладка 2: Running (дерево вложенности) ============ */

static GtkTreeStore *am_run_store = NULL;

static void am_fill_running(GtkTreeIter *parent, const char *host_name);

/* добавить гостя name в дерево (если инстанс жив) */
static void am_add_running_child(GtkTreeIter *parent,
                                 const char *name)
{
    GtkTreeIter it;
    XsPlugin *p = xs_core_find_instance(name);
    GdkPixbuf *pb;
    char *t;

    if (!p || !p->win)
        return;
    t = am_conf_get(name, "user_label");
    pb = am_type_icon(xs_core_plugin_type(p), 16);
    gtk_tree_store_append(am_run_store, &it, parent);
    gtk_tree_store_set(am_run_store, &it,
                       0, pb,
                       1, xs_core_plugin_type(p),
                       2, t && t[0] ? t : name,
                       -1);
    if (pb)
        g_object_unref(pb);
    g_free(t);
    /* гости этого гостя (вложенность) */
    am_fill_running(&it, name);
}

static void am_fill_running(GtkTreeIter *parent, const char *host_name)
{
    GKeyFile *kf = xs_core_plugin_conf(host_name);
    int i;

    if (!kf)
        return;
    for (i = 1; i <= 64; i++) {
        char *key = g_strdup_printf("guests_%d", i);
        char *v = g_key_file_get_string(kf, host_name, key, NULL);
        char *off;

        g_free(key);
        if (!v || !v[0]) {
            g_free(v);
            break;
        }
        {
            char *okey = g_strdup_printf("guests_%d_off", i);

            off = g_key_file_get_string(kf, host_name, okey, NULL);
            g_free(okey);
        }
        if (!off || !off[0])
            am_add_running_child(parent, v);
        g_free(off);
        g_free(v);
    }
}

void am_refresh_running_tree(void)
{
    GtkTreeIter it;
    int n, i;
    char **roots;

    if (!am_run_store)
        return;
    gtk_tree_store_clear(am_run_store);
    roots = xs_core_list_running_daemon_instances(&n);
    for (i = 0; i < n; i++) {
        GdkPixbuf *pb;
        char *t = am_conf_get(roots[i], "user_label");

        XsPlugin *rp = xs_core_find_instance(roots[i]);

        pb = am_type_icon(xs_core_plugin_type(rp), 16);
        gtk_tree_store_append(am_run_store, &it, NULL);
        gtk_tree_store_set(am_run_store, &it,
                           0, pb,
                           1, xs_core_plugin_type(rp),
                           2, t && t[0] ? t : roots[i],
                           -1);
        if (pb)
            g_object_unref(pb);
        g_free(t);
        am_fill_running(&it, roots[i]);
    }
    g_strfreev(roots);
}

static GtkWidget *am_build_running_page(void)
{
    GtkWidget *scroll, *tree;
    GtkCellRenderer *r;
    GtkTreeViewColumn *c;

    scroll = gtk_scrolled_window_new(NULL, NULL);
    gtk_scrolled_window_set_policy(GTK_SCROLLED_WINDOW(scroll),
                                   GTK_POLICY_AUTOMATIC,
                                   GTK_POLICY_AUTOMATIC);
    gtk_widget_set_vexpand(scroll, TRUE);
    am_run_store = gtk_tree_store_new(3, GDK_TYPE_PIXBUF,
                                      G_TYPE_STRING, G_TYPE_STRING);
    tree = gtk_tree_view_new_with_model(
        GTK_TREE_MODEL(am_run_store));
    gtk_tree_view_set_headers_visible(GTK_TREE_VIEW(tree), TRUE);
    gtk_style_context_add_class(
        gtk_widget_get_style_context(tree), "am-tree");

    c = gtk_tree_view_column_new();
    gtk_tree_view_column_set_title(c, "Applet");
    r = gtk_cell_renderer_pixbuf_new();
    gtk_tree_view_column_pack_start(c, r, FALSE);
    gtk_tree_view_column_set_attributes(c, r, "pixbuf", 0, NULL);
    r = am_text_renderer(14.0);
    gtk_tree_view_column_pack_start(c, r, TRUE);
    gtk_tree_view_column_set_attributes(c, r, "text", 1, NULL);
    gtk_tree_view_append_column(GTK_TREE_VIEW(tree), c);

    r = am_text_renderer(14.0);
    c = gtk_tree_view_column_new_with_attributes("Instance", r,
                                                 "text", 2, NULL);
    gtk_tree_view_append_column(GTK_TREE_VIEW(tree), c);

    gtk_container_add(GTK_CONTAINER(scroll), tree);
    am_refresh_running_tree();
    return scroll;
}

/* ============ Вкладка 3: Configs ============ */

static GtkTreeStore *am_conf_store = NULL;

enum {
    AM_CONF_NAME = 0,
    AM_CONF_TYPE,
    AM_CONF_STARTED,
    AM_CONF_AUTOSTART,
    AM_CONF_LABEL,
    AM_CONF_N
};

static gboolean am_is_guest(const char *inst)
{
    char *sb = am_conf_get(inst, "started_by");
    gboolean guest = sb && strcmp(sb, "plugin") == 0;

    g_free(sb);
    return guest;
}

static char *am_conf_host_of(const char *inst);
static gboolean am_conf_find_row(const char *inst, GtkTreeIter *out);

/* добавить конфиг inst в дерево; если он гость — найти host и
 * повесить ребёнком (иначе — корень) */
static void am_conf_add_row(const char *inst)
{
    GtkTreeIter it, *parent = NULL;
    char *host = NULL;
    char *type = am_conf_get(inst, "xs_type");
    char *sb = am_conf_get(inst, "started_by");
    gboolean is_guest = sb && strcmp(sb, "plugin") == 0;
    gboolean autostart = am_autostart_enabled(inst);
    GtkTreeIter hit;
    char *lab = am_conf_get(inst, "user_label");

    g_free(sb);
    /* гость: ищем строку хоста в дереве (хост уже добавлен —
     * корни добавляем первыми, hosts_before_guests) */
    if (is_guest) {
        host = am_conf_host_of(inst);
        if (host && am_conf_find_row(host, &hit))
            parent = &hit;
    }
    gtk_tree_store_append(am_conf_store, &it, parent);
    gtk_tree_store_set(am_conf_store, &it,
                       AM_CONF_NAME, inst,
                       AM_CONF_TYPE, type ? type : "?",
                       AM_CONF_STARTED,
                           is_guest ? "plugin" : "main_daemon",
                       AM_CONF_AUTOSTART, autostart,
                       AM_CONF_LABEL, lab && lab[0] ? lab : inst,
                       -1);
    xs_log_impl("am: conf %s autostart=%d guest=%d",
                inst, autostart, is_guest);
    g_free(type);
    g_free(lab);
    g_free(host);
}

/* хост инстанса (по guests_N всех frame-конфигов) */
char *am_conf_host_of(const char *inst)
{
    int n, i;
    char **names = am_list_conf_names(&n);
    char *found = NULL;

    for (i = 0; i < n && !found; i++) {
        GKeyFile *kf = xs_core_plugin_conf(names[i]);
        int j;

        if (!kf)
            continue;
        {
            gchar **groups = g_key_file_get_groups(kf, NULL);

            if (!groups || !groups[0]) {
                g_strfreev(groups);
                continue;
            }
            for (j = 1; j <= 64 && !found; j++) {
                char *k = g_strdup_printf("guests_%d", j);
                char *v = g_key_file_get_string(kf, groups[0],
                                                k, NULL);
                gboolean has = v && v[0];

                if (has && strcmp(v, inst) == 0)
                    found = g_strdup(names[i]);
                g_free(v);
                g_free(k);
                if (!has)
                    break;
            }
            g_strfreev(groups);
        }
    }
    g_strfreev(names);
    return found;
}

/* найти уже добавленную строку хоста в дереве */
gboolean am_conf_find_row(const char *inst, GtkTreeIter *out)
{
    GtkTreeIter it;
    gboolean ok = gtk_tree_model_get_iter_first(
        GTK_TREE_MODEL(am_conf_store), &it);

    while (ok) {
        char *nm = NULL;

        gtk_tree_model_get(GTK_TREE_MODEL(am_conf_store), &it,
                           AM_CONF_NAME, &nm, -1);
        if (nm && strcmp(nm, inst) == 0) {
            *out = it;
            g_free(nm);
            return TRUE;
        }
        g_free(nm);
        /* спуск в детей */
        {
            GtkTreeIter ch;
            gboolean has = gtk_tree_model_iter_children(
                GTK_TREE_MODEL(am_conf_store), &ch, &it);

            while (has) {
                char *cn = NULL;

                gtk_tree_model_get(GTK_TREE_MODEL(am_conf_store),
                                   &ch, AM_CONF_NAME, &cn, -1);
                if (cn && strcmp(cn, inst) == 0) {
                    *out = ch;
                    g_free(cn);
                    return TRUE;
                }
                g_free(cn);
                has = gtk_tree_model_iter_next(
                    GTK_TREE_MODEL(am_conf_store), &ch);
            }
        }
        ok = gtk_tree_model_iter_next(
            GTK_TREE_MODEL(am_conf_store), &it);
    }
    return FALSE;
}

static void am_refresh_conf_list(void)
{
    int n, i;
    char **names;

    if (!am_conf_store)
        return;
    gtk_tree_store_clear(am_conf_store);
    names = am_list_conf_names(&n);
    /* ПРОХОД 1: все НЕ-гости (корни), затем ПРОХОД 2: гости
     * (их хосты уже в дереве) */
    for (i = 0; i < n; i++) {
        char *sb = am_conf_get(names[i], "started_by");

        if (!sb || strcmp(sb, "main_daemon") == 0)
            am_conf_add_row(names[i]);
        g_free(sb);
    }
    for (i = 0; i < n; i++) {
        char *sb = am_conf_get(names[i], "started_by");

        if (sb && strcmp(sb, "plugin") == 0)
            am_conf_add_row(names[i]);
        g_free(sb);
    }
    g_strfreev(names);
}

static void am_autostart_toggled(GtkCellRendererToggle *cell,
                                 gchar *path, gpointer data)
{
    GtkTreeIter it;
    gchar *name;

    (void)cell;
    (void)data;
    if (!gtk_tree_model_get_iter_from_string(
            GTK_TREE_MODEL(am_conf_store), &it, path))
        return;
    gtk_tree_model_get(GTK_TREE_MODEL(am_conf_store), &it,
                       AM_CONF_NAME, &name, -1);
    if (name) {
        gboolean cur = am_autostart_enabled(name);
        gboolean is_guest = am_is_guest(name);

        if (is_guest) {
            GtkWidget *dlg = gtk_message_dialog_new(
                GTK_WINDOW(am_window), GTK_DIALOG_MODAL,
                GTK_MESSAGE_INFO, GTK_BUTTONS_OK,
                _("\u00ab%s\u00bb is a guest instance: its autostart is "
                  "managed by its frame host."),
                name);
            gtk_dialog_run(GTK_DIALOG(dlg));
            gtk_widget_destroy(dlg);
        } else {
            am_autostart_set(name, !cur);
            gtk_tree_store_set(am_conf_store, &it,
                               AM_CONF_AUTOSTART, !cur, -1);
        }
        g_free(name);
    }
}

static void am_conf_delete(GtkButton *b, gpointer data)
{
    GtkTreeSelection *sel;
    GtkTreeIter it;
    gchar *name;
    GtkWidget *dlg;
    int resp;

    (void)b;
    (void)data;
    sel = gtk_tree_view_get_selection(
        g_object_get_data(G_OBJECT(am_notebook), "am-conf-tree"));
    if (!gtk_tree_selection_get_selected(sel, NULL, &it))
        return;
    gtk_tree_model_get(GTK_TREE_MODEL(am_conf_store), &it,
                       AM_CONF_NAME, &name, -1);
    if (!name)
        return;
    dlg = gtk_message_dialog_new(
        GTK_WINDOW(am_window), GTK_DIALOG_MODAL,
        GTK_MESSAGE_QUESTION, GTK_BUTTONS_OK_CANCEL,
        _("Delete config \u00ab%s\u00bb?\nThe instance will be stopped "
          "and deleted."),
        name);
    resp = gtk_dialog_run(GTK_DIALOG(dlg));
    gtk_widget_destroy(dlg);
    if (resp == GTK_RESPONSE_OK) {
        XsPlugin *p = xs_core_find_instance(name);

        if (p)
            xs_core_delete_instance_full(p, TRUE);
        else {
            char *path = g_strdup_printf(
                "%s/%s.conf", xs_core_plugins_dir(), name);

            unlink(path);
            g_free(path);
            am_autostart_set(name, FALSE);
        }
        am_refresh_conf_list();
        am_refresh_running_tree();
    }
    g_free(name);
}

static void am_conf_edit(GtkButton *b, gpointer data)
{
    GtkTreeSelection *sel;
    GtkTreeIter it;
    gchar *name;

    (void)b;
    (void)data;
    sel = gtk_tree_view_get_selection(
        g_object_get_data(G_OBJECT(am_notebook), "am-conf-tree"));
    if (!gtk_tree_selection_get_selected(sel, NULL, &it))
        return;
    gtk_tree_model_get(GTK_TREE_MODEL(am_conf_store), &it,
                       AM_CONF_NAME, &name, -1);
    if (!name)
        return;
    {
        GtkWidget *dlg = gtk_dialog_new_with_buttons(
            name, GTK_WINDOW(am_window), GTK_DIALOG_MODAL,
            "_Save", GTK_RESPONSE_OK,
            "_Cancel", GTK_RESPONSE_CANCEL, NULL);
        GtkWidget *sw = gtk_scrolled_window_new(NULL, NULL);
        GtkWidget *tv = gtk_text_view_new();
        char *path = g_strdup_printf("%s/%s.conf",
                                     xs_core_plugins_dir(), name);
        gchar *text = NULL;

        if (g_file_get_contents(path, &text, NULL, NULL)) {
            GtkTextBuffer *buf =
                gtk_text_view_get_buffer(GTK_TEXT_VIEW(tv));

            gtk_text_buffer_set_text(buf, text ? text : "", -1);
            g_free(text);
        }
        gtk_text_view_set_monospace(GTK_TEXT_VIEW(tv), TRUE);
        gtk_container_add(GTK_CONTAINER(sw), tv);
        gtk_box_pack_start(GTK_BOX(gtk_dialog_get_content_area(
                               GTK_DIALOG(dlg))),
                           sw, TRUE, TRUE, 0);
        gtk_window_set_default_size(GTK_WINDOW(dlg), 520, 420);
        gtk_widget_show_all(dlg);
        if (gtk_dialog_run(GTK_DIALOG(dlg)) == GTK_RESPONSE_OK) {
            GtkTextBuffer *buf =
                gtk_text_view_get_buffer(GTK_TEXT_VIEW(tv));
            GtkTextIter s, e;
            gchar *newtext;

            gtk_text_buffer_get_bounds(buf, &s, &e);
            newtext = gtk_text_buffer_get_text(buf, &s, &e,
                                               FALSE);
            /* сбросить кэш, чтобы демон перечитал файл */
            xs_core_drop_conf_cache(name);
            g_file_set_contents(path, newtext ? newtext : "", -1,
                                NULL);
            g_free(newtext);
        }
        gtk_widget_destroy(dlg);
        g_free(path);
    }
    g_free(name);
}

static GtkWidget *am_build_configs_page(void)
{
    GtkWidget *scroll, *tree, *hb, *btn;
    GtkCellRenderer *r;
    GtkTreeViewColumn *c;

    scroll = gtk_scrolled_window_new(NULL, NULL);
    gtk_scrolled_window_set_policy(GTK_SCROLLED_WINDOW(scroll),
                                   GTK_POLICY_AUTOMATIC,
                                   GTK_POLICY_AUTOMATIC);
    am_conf_store = gtk_tree_store_new(AM_CONF_N, G_TYPE_STRING,
                                       G_TYPE_STRING, G_TYPE_STRING,
                                       G_TYPE_BOOLEAN, G_TYPE_STRING);
    tree = gtk_tree_view_new_with_model(
        GTK_TREE_MODEL(am_conf_store));
    gtk_tree_view_set_headers_visible(GTK_TREE_VIEW(tree), TRUE);
    gtk_style_context_add_class(
        gtk_widget_get_style_context(tree), "am-tree");

    c = gtk_tree_view_column_new_with_attributes("Instance",
        am_text_renderer(14.0), "text", AM_CONF_NAME, NULL);
    gtk_tree_view_column_set_expand(c, TRUE);
    gtk_tree_view_append_column(GTK_TREE_VIEW(tree), c);
    c = gtk_tree_view_column_new_with_attributes("Type",
        am_text_renderer(14.0), "text", AM_CONF_TYPE, NULL);
    gtk_tree_view_append_column(GTK_TREE_VIEW(tree), c);
    c = gtk_tree_view_column_new_with_attributes("Started by",
        am_text_renderer(14.0), "text", AM_CONF_STARTED, NULL);
    gtk_tree_view_append_column(GTK_TREE_VIEW(tree), c);
    r = gtk_cell_renderer_toggle_new();
    g_object_set(r, "activatable", TRUE, NULL);
    g_signal_connect(r, "toggled", G_CALLBACK(am_autostart_toggled),
                     NULL);
    c = gtk_tree_view_column_new_with_attributes("Autostart", r,
        "active", AM_CONF_AUTOSTART,
        "activatable", AM_CONF_AUTOSTART, NULL);
    gtk_tree_view_append_column(GTK_TREE_VIEW(tree), c);

    gtk_container_add(GTK_CONTAINER(scroll), tree);

    hb = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 6);
    gtk_container_set_border_width(GTK_CONTAINER(hb), 6);
    gtk_style_context_add_class(
        gtk_widget_get_style_context(hb), "am-toolbar");
    btn = gtk_button_new_with_label(_("Delete config"));
    g_signal_connect(btn, "clicked", G_CALLBACK(am_conf_delete),
                     NULL);
    gtk_box_pack_start(GTK_BOX(hb), btn, FALSE, FALSE, 0);
    btn = gtk_button_new_with_label(_("Edit"));
    g_signal_connect(btn, "clicked", G_CALLBACK(am_conf_edit), NULL);
    gtk_box_pack_start(GTK_BOX(hb), btn, FALSE, FALSE, 0);

    {
        GtkWidget *vbox = gtk_box_new(GTK_ORIENTATION_VERTICAL, 0);

        gtk_box_pack_start(GTK_BOX(vbox), scroll, TRUE, TRUE, 0);
        gtk_box_pack_start(GTK_BOX(vbox), hb, FALSE, FALSE, 0);
        g_object_set_data_full(G_OBJECT(am_notebook),
                               "am-conf-tree",
                               g_object_ref(tree),
                               (GDestroyNotify)g_object_unref);
        am_refresh_conf_list();
        return vbox;
    }
}

/* ============ окно и меню ============ */

static void am_window_destroy(GtkWidget *w, gpointer data)
{
    (void)w;
    (void)data;
    am_window = NULL;
    am_notebook = NULL;
    am_run_store = NULL;
    am_conf_store = NULL;
}

void xs_applet_manager_show(void)
{
    GtkWidget *nb;
    static GtkCssProvider *css = NULL;

    if (!css) {
        css = gtk_css_provider_new();
        gtk_css_provider_load_from_data(
            css,
            ".am-tree, .am-tree * { font-size: 14px; }"
            ".am-toolbar button { font-size: 13px; padding: 4px 12px; }",
            -1, NULL);
        gtk_style_context_add_provider_for_screen(
            gdk_screen_get_default(),
            GTK_STYLE_PROVIDER(css),
            GTK_STYLE_PROVIDER_PRIORITY_APPLICATION);
    }
    if (am_window) {
        gtk_window_present(GTK_WINDOW(am_window));
        return;
    }
    am_window = gtk_window_new(GTK_WINDOW_TOPLEVEL);
    gtk_window_set_title(GTK_WINDOW(am_window),
                         _("Applet management"));
    gtk_window_set_default_size(GTK_WINDOW(am_window), 820, 500);
    g_signal_connect(am_window, "destroy",
                     G_CALLBACK(am_window_destroy), NULL);

    nb = gtk_notebook_new();
    am_notebook = GTK_NOTEBOOK(nb);
    gtk_notebook_append_page(GTK_NOTEBOOK(nb),
                             am_build_types_page(),
                             gtk_label_new(_("Applets")));
    gtk_notebook_append_page(GTK_NOTEBOOK(nb),
                             am_build_running_page(),
                             gtk_label_new(_("Running")));
    gtk_notebook_append_page(GTK_NOTEBOOK(nb),
                             am_build_configs_page(),
                             gtk_label_new(_("Configs")));
    gtk_container_add(GTK_CONTAINER(am_window), nb);
    gtk_widget_show_all(am_window);
}
