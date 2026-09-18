/* launcher.c — Xscreenlets плагин «launcher» (замена LauncherScreenlet v0.7).
 * Иконка (SVG/PNG) на рабочем столе; клик запускает shell-команду.
 * Drag&drop .desktop-файла настраивает action/icon/label (как в оригинале).
 * Окно = размер иконки * scale; label — tooltip окна. */
#include <gtk/gtk.h>
#include <glib.h>
#include <glib/gstdio.h>
#include <librsvg/rsvg.h>
#include <gdk-pixbuf/gdk-pixbuf.h>
#include <string.h>
#include <stdlib.h>
#include "xs_api.h"
#include "common.h"

typedef struct {
	GKeyFile *kf;
	double scale;
	double opacity;
	int x, y;
	char *action;           /* shell-команда */
	char *icon_path;        /* файл иконки (svg/png/...) */
	char *label;            /* tooltip/подпись */
	/* загруженная иконка: либо svg, либо pixbuf */
	RsvgHandle *svg;
	GdkPixbuf *pixbuf;
	int icon_w, icon_h;
} PrivData;

/* ---------- иконка ---------- */

static void launcher_clear_icon(PrivData *priv)
{
	g_clear_object(&priv->svg);
	g_clear_object(&priv->pixbuf);
	priv->icon_w = 32;
	priv->icon_h = 32;
}

static gboolean launcher_load_icon(PrivData *priv, const char *path)
{
	GError *err = NULL;

	launcher_clear_icon(priv);
	if (!path || !path[0])
		return FALSE;

	if (g_str_has_suffix(path, ".svg") || g_str_has_suffix(path, ".SVG")) {
		priv->svg = rsvg_handle_new_from_file(path, &err);
		if (priv->svg) {
			gdouble out_w = 0, out_h = 0;

			if (rsvg_handle_get_intrinsic_size_in_pixels(priv->svg,
			                                             &out_w, &out_h) &&
			    out_w > 0 && out_h > 0) {
				priv->icon_w = (int)out_w;
				priv->icon_h = (int)out_h;
			} else {
				/* Нет width/height в px (только viewBox):
				 * natural-размер = размер viewBox, иначе
				 * между запусками icon_w «прыгает» и
				 * масштаб слетает. */
				RsvgDimensionData dim;

				rsvg_handle_get_dimensions(priv->svg, &dim);
				if (dim.width > 0 && dim.height > 0) {
					priv->icon_w = dim.width;
					priv->icon_h = dim.height;
				}
			}
		}
	} else {
		priv->pixbuf = gdk_pixbuf_new_from_file(path, &err);
		if (priv->pixbuf) {
			priv->icon_w = gdk_pixbuf_get_width(priv->pixbuf);
			priv->icon_h = gdk_pixbuf_get_height(priv->pixbuf);
		}
	}
	if (!priv->svg && !priv->pixbuf) {
		if (err)
			g_error_free(err);
		return FALSE;
	}
	return TRUE;
}

/* дефолт: иконка из каталога оригинального плагина */
static char *launcher_default_icon(void)
{
	static const char *cands[] = {
		"/usr/share/screenlets/Launcher/default-icon.svg",
		"/usr/share/screenlets/Launcher/icon.svg",
		NULL
	};
	size_t i;

	for (i = 0; cands[i]; i++)
		if (g_file_test(cands[i], G_FILE_TEST_EXISTS))
			return g_strdup(cands[i]);
	return g_strdup("");
}

/* ---------- запуск ---------- */

static void launcher_launch(PrivData *priv)
{
	GError *err = NULL;

	if (!priv->action || !priv->action[0])
		return;
	if (!g_spawn_command_line_async(priv->action, &err)) {
		xs_host_api()->log("launcher: failed to run '%s': %s",
		                   priv->action, err ? err->message : "?");
		if (err)
			g_error_free(err);
	}
}

/* ---------- конфиг ---------- */

static void launcher_apply_icon(XsPlugin *p, PrivData *priv)
{
	int w = (int)(priv->icon_w * priv->scale);
	int h = (int)(priv->icon_h * priv->scale);

	if (w < 8) w = 8;
	if (h < 8) h = 8;
	/* запомнить ИТОГОВЫЙ размер окна: при рестарте окно должно
	 * встать ровно таким, каким юзер его оставил (natural-размер
	 * SVG между запусками может считаться по-разному) */
	g_key_file_set_integer(priv->kf, p->name, "win_w", w);
	g_key_file_set_integer(priv->kf, p->name, "win_h", h);
	xs_core_plugin_conf_flush(p->name);
	if (p->win)
		xs_host_api()->resize(p, w, h);
	if (p->win)
		gtk_widget_set_tooltip_text(p->win,
		                            priv->label && priv->label[0]
		                            ? priv->label : NULL);
}

/* ---------- .desktop ---------- */

static char *launcher_icon_from_gtk_theme(const char *name)
{
	GtkIconTheme *theme = gtk_icon_theme_get_default();
	GtkIconInfo *info;
	char *fn = NULL;

	if (!name || !name[0])
		return NULL;
	if (name[0] == '/')
		return g_strdup(name);
	info = gtk_icon_theme_lookup_icon(theme, name, 48, 0);
	if (info) {
		fn = g_strdup(gtk_icon_info_get_filename(info));
		g_object_unref(info);
	}
	return fn;
}

/* распарсить .desktop: Exec → action, Icon → icon_path, Name → label */
static void launcher_from_desktop_file(XsPlugin *p, const char *filename)
{
	PrivData *priv = p->priv;
	char *path = (char *)filename;
	GKeyFile *dkf;
	gchar *exec = NULL, *icon = NULL, *name = NULL;
	gboolean changed = FALSE;

	if (g_str_has_prefix(path, "file://"))
		path += 7;
	dkf = g_key_file_new();
	if (!g_key_file_load_from_file(dkf, path, G_KEY_FILE_NONE, NULL)) {
		g_key_file_free(dkf);
		return;
	}
	exec = g_key_file_get_string(dkf, "Desktop Entry", "Exec", NULL);
	icon = g_key_file_get_string(dkf, "Desktop Entry", "Icon", NULL);
	name = g_key_file_get_string(dkf, "Desktop Entry", "Name", NULL);
	g_key_file_free(dkf);

	if (exec && exec[0]) {
		g_free(priv->action);
		priv->action = g_strdup(exec);
		g_key_file_set_string(priv->kf, p->name, "action",
		                      priv->action);
		changed = TRUE;
	}
	if (icon && icon[0]) {
		char *fn = launcher_icon_from_gtk_theme(icon);
		if (fn && fn[0]) {
			if (launcher_load_icon(priv, fn)) {
				g_free(priv->icon_path);
				priv->icon_path = fn;
				g_key_file_set_string(priv->kf, p->name,
				                      "icon", priv->icon_path);
				changed = TRUE;
			} else {
				g_free(fn);
			}
		} else {
			g_free(fn);
		}
	}
	if (name && name[0]) {
		g_free(priv->label);
		priv->label = g_strdup(name);
		g_key_file_set_string(priv->kf, p->name, "label",
		                      priv->label);
		changed = TRUE;
	}
	g_free(exec);
	g_free(icon);
	g_free(name);
	if (changed) {
		xs_core_plugin_conf_flush(p->name);
		launcher_apply_icon(p, priv);
		if (p->win)
			gtk_widget_queue_draw(p->win);
	}
}

/* ---------- drag&drop ---------- */

static void launcher_drop_received(GtkWidget *w, GdkDragContext *ctx,
                                   gint x, gint y, GtkSelectionData *sel,
                                   guint info, guint time, gpointer data)
{
	XsPlugin *p = data;
	gchar **uris;

	(void)w; (void)x; (void)y; (void)info;
	uris = gtk_selection_data_get_uris(sel);
	if (uris && uris[0]) {
		if (g_str_has_suffix(uris[0], ".desktop"))
			launcher_from_desktop_file(p, uris[0]);
	} else {
		const guchar *txt = gtk_selection_data_get_text(sel);
		if (txt) {
			char *s = g_strdup((const char *)txt);
			char *nl = strchr(s, '\n');
			if (nl)
				*nl = '\0';
			g_strstrip(s);
			if (g_str_has_suffix(s, ".desktop"))
				launcher_from_desktop_file(p, s);
			g_free(s);
		}
	}
	if (uris)
		g_strfreev(uris);
	gtk_drag_finish(ctx, TRUE, FALSE, time);
}

static void launcher_setup_dnd(XsPlugin *p)
{
	GtkWidget *area;
	XsWinState *state;
	GtkTargetEntry targets[] = {
		{ "text/uri-list", 0, 0 },
		{ "text/plain",    0, 1 }
	};

	if (!p->win)
		return;
	state = g_object_get_data(G_OBJECT(p->win), "xs-state");
	area = state ? state->area : p->win;
	gtk_drag_dest_set(area, GTK_DEST_DEFAULT_ALL, targets,
	                  G_N_ELEMENTS(targets), GDK_ACTION_COPY);
	g_signal_connect(area, "drag-data-received",
	                 G_CALLBACK(launcher_drop_received), p);
}

/* ---------- ops ---------- */

static int launcher_init(XsPlugin *p, GKeyFile *kf)
{
	PrivData *priv = g_new0(PrivData, 1);

	p->priv = priv;
	priv->kf = kf;
	priv->scale = xs_host_api()->conf_dbl(kf, p->name, "scale", 1.0);
	if (priv->scale < 0.2)
		priv->scale = 0.2;
	else if (priv->scale > 10.0)
		priv->scale = 10.0;
	priv->opacity = xs_host_api()->conf_dbl(kf, p->name, "opacity", 1.0);
	if (priv->opacity < 0.1)
		priv->opacity = 0.1;
	else if (priv->opacity > 1.0)
		priv->opacity = 1.0;
	priv->action = xs_host_api()->conf_str(kf, p->name, "action", "");
	priv->label = xs_host_api()->conf_str(kf, p->name, "label", "");
	priv->icon_path = xs_host_api()->conf_str(kf, p->name, "icon", "");
	if (!priv->icon_path[0]) {
		g_free(priv->icon_path);
		priv->icon_path = launcher_default_icon();
		g_key_file_set_string(kf, p->name, "icon", priv->icon_path);
	}
	if (!launcher_load_icon(priv, priv->icon_path)) {
		char *di = launcher_default_icon();
		if (strcmp(di, priv->icon_path) != 0 && di[0] &&
		    launcher_load_icon(priv, di)) {
			g_free(priv->icon_path);
			priv->icon_path = di;
			g_key_file_set_string(kf, p->name, "icon", di);
		} else {
			g_free(di);
		}
	}
	priv->x = xs_host_api()->conf_int(kf, p->name, "x", 80);
	priv->y = xs_host_api()->conf_int(kf, p->name, "y", 80);

	/* NATURAL-размер иконки сохранён в конфиге (SVG без width/height
	 * в px возвращает разный intrinsic размер между запусками —
	 * из-за этого после рестарта лаунчер становился микроскопическим).
	 * Конфиг главнее: 1-й запуск пишет, последующие читают. */
	{
		int saved_w = xs_host_api()->conf_int(kf, p->name,
		                                      "icon_w", 0);
		int saved_h = xs_host_api()->conf_int(kf, p->name,
		                                      "icon_h", 0);

		if (saved_w > 0 && saved_h > 0) {
			priv->icon_w = saved_w;
			priv->icon_h = saved_h;
		}
		xs_host_api()->conf_set_int(kf, p->name, "icon_w",
		                            priv->icon_w);
		xs_host_api()->conf_set_int(kf, p->name, "icon_h",
		                            priv->icon_h);
	}
	/* ИТОГОВЫЙ размер окна из конфига (win_w/win_h пишет
	 * launcher_apply_icon при каждой смене scale/иконки): при
	 * рестарте окно ровно такое, каким юзер его оставил. */
	{
		int ww = xs_host_api()->conf_int(kf, p->name, "win_w", 0);
		int wh = xs_host_api()->conf_int(kf, p->name, "win_h", 0);
		int w = (int)(priv->icon_w * priv->scale);
		int h = (int)(priv->icon_h * priv->scale);

		if (w < 8) w = 8;
		if (h < 8) h = 8;
		if (ww >= 8 && wh >= 8) {
			w = ww;
			h = wh;
		}
		p->win = xs_host_api()->make_window(p, priv->x, priv->y,
		                                    w, h);
	}
	if (!p->win) {
		p->host->log("launcher: failed to create window");
		launcher_clear_icon(priv);
		g_free(priv->action);
		g_free(priv->icon_path);
		g_free(priv->label);
		g_free(priv);
		p->priv = NULL;
		return -1;
	}
	xs_host_api()->set_opacity(p, priv->opacity);
	if (priv->label && priv->label[0])
		gtk_widget_set_tooltip_text(p->win, priv->label);
	launcher_setup_dnd(p);
	return 0;
}

static void launcher_draw(XsPlugin *p, cairo_t *cr, int w, int h)
{
	PrivData *priv = p->priv;
	cairo_surface_t *host_bg;

	if (!priv)
		return;
	cairo_set_operator(cr, CAIRO_OPERATOR_OVER);
	/* гость рамки: первым слоем — фон рамки под нами (иначе
	 * прозрачные пиксели показали бы рабочий стол) */
	host_bg = xs_host_api()->get_host_backdrop
	              ? xs_host_api()->get_host_backdrop(p) : NULL;
	if (host_bg) {
		cairo_save(cr);
		cairo_set_source_surface(cr, host_bg, 0, 0);
		cairo_set_operator(cr, CAIRO_OPERATOR_SOURCE);
		cairo_paint(cr);
		cairo_restore(cr);
		cairo_set_operator(cr, CAIRO_OPERATOR_OVER);
	}
	if (priv->pixbuf) {
		gdk_cairo_set_source_pixbuf(cr, priv->pixbuf, 0, 0);
		cairo_save(cr);
		cairo_scale(cr,
		            (double)w / gdk_pixbuf_get_width(priv->pixbuf),
		            (double)h / gdk_pixbuf_get_height(priv->pixbuf));
		cairo_paint(cr);
		cairo_restore(cr);
	} else if (priv->svg) {
		/* ВПИСЫВАТЬ весь viewBox в окно: render_document без
		 * явного scale у SVG без width/height рисует в natural
		 * размере (виден только уголок). Явный scale по осям. */
		double nw = priv->icon_w > 0 ? priv->icon_w : w;
		double nh = priv->icon_h > 0 ? priv->icon_h : h;
		RsvgRectangle viewport = { 0, 0, nw, nh };

		cairo_save(cr);
		cairo_scale(cr, (double)w / nw, (double)h / nh);
		rsvg_handle_render_document(priv->svg, cr, &viewport, NULL);
		cairo_restore(cr);
	}
}

static gboolean launcher_button(XsPlugin *p, GdkEventButton *ev)
{
	PrivData *priv = p->priv;

	if (!priv || ev->type != GDK_BUTTON_PRESS)
		return FALSE;
	if (ev->button == 1) {
		launcher_launch(priv);
		return TRUE;
	}
	return FALSE;
}

static void launcher_shutdown(XsPlugin *p)
{
	PrivData *priv = p->priv;

	if (priv) {
		launcher_clear_icon(priv);
		g_free(priv->action);
		g_free(priv->icon_path);
		g_free(priv->label);
		g_free(priv);
		p->priv = NULL;
	}
}

static void launcher_menu_cmd(XsPlugin *p, const char *cmd)
{
	PrivData *priv = p->priv;

	if (!priv)
		return;
	if (strcmp(cmd, "scale-applied") == 0) {
		GKeyFile *kf = xs_core_plugin_conf(p->name);
		double s = xs_host_api()->conf_dbl(kf, p->name, "scale", 1.0);

		if (s < 0.2) s = 0.2; else if (s > 10.0) s = 10.0;
		priv->scale = s;
		launcher_apply_icon(p, priv);
	}
}

/* ---------- Properties: группа Starter ---------- */

static void launcher_entry_changed(GtkEditable *e, gpointer data)
{
	XsPlugin *p = data;
	PrivData *priv;
	const char *key = g_object_get_data(G_OBJECT(e), "xs-key");
	const char *text;

	if (!p || !p->priv || !key)
		return;
	priv = p->priv;
	text = gtk_entry_get_text(GTK_ENTRY(e));
	if (strcmp(key, "action") == 0) {
		g_free(priv->action);
		priv->action = g_strdup(text);
	} else if (strcmp(key, "label") == 0) {
		g_free(priv->label);
		priv->label = g_strdup(text);
		if (p->win)
			gtk_widget_set_tooltip_text(p->win,
			                            priv->label[0]
			                            ? priv->label : NULL);
	}
	g_key_file_set_string(priv->kf, p->name, key, text ? text : "");
	xs_core_plugin_conf_flush(p->name);
}

static void launcher_icon_set(GtkFileChooserButton *btn, gpointer data)
{
	XsPlugin *p = data;
	PrivData *priv;
	char *fn;

	if (!p || !p->priv)
		return;
	priv = p->priv;
	fn = gtk_file_chooser_get_filename(GTK_FILE_CHOOSER(btn));
	if (!fn || !launcher_load_icon(priv, fn)) {
		g_free(fn);
		return;
	}
	g_free(priv->icon_path);
	priv->icon_path = fn;
	g_key_file_set_string(priv->kf, p->name, "icon", priv->icon_path);
	g_key_file_set_integer(priv->kf, p->name, "icon_w", priv->icon_w);
	g_key_file_set_integer(priv->kf, p->name, "icon_h", priv->icon_h);
	xs_core_plugin_conf_flush(p->name);
	launcher_apply_icon(p, priv);
	if (p->win)
		gtk_widget_queue_draw(p->win);
}

static void launcher_properties(XsPlugin *p, GtkNotebook *nb)
{
	PrivData *priv;
	GtkWidget *page, *lbl, *w, *fc;
	GtkFileFilter *filter;

	if (!p || !p->priv)
		return;
	priv = p->priv;

	page = gtk_box_new(GTK_ORIENTATION_VERTICAL, 3);
	gtk_container_set_border_width(GTK_CONTAINER(page), 10);
	lbl = gtk_label_new("Some options related to the Launcher-Screenlet.");
	gtk_widget_set_halign(lbl, GTK_ALIGN_START);
	gtk_box_pack_start(GTK_BOX(page), lbl, FALSE, FALSE, 7);
	gtk_box_pack_start(GTK_BOX(page),
	                   gtk_separator_new(GTK_ORIENTATION_HORIZONTAL),
	                   FALSE, FALSE, 5);
	gtk_notebook_append_page(nb, page, gtk_label_new("Starter"));

	w = xs_prop_add_string(GTK_BOX(page), "Tooltip/Label",
	                       "A string that will be displayed as tooltip ...",
	                       priv->label);
	g_object_set_data_full(G_OBJECT(w), "xs-key",
	                       g_strdup("label"), g_free);
	g_signal_connect(w, "changed", G_CALLBACK(launcher_entry_changed), p);

	w = xs_prop_add_string(GTK_BOX(page), "Command",
	                       "Shell-command to be executed when icon is "
	                       "clicked ...",
	                       priv->action);
	g_object_set_data_full(G_OBJECT(w), "xs-key",
	                       g_strdup("action"), g_free);
	g_signal_connect(w, "changed", G_CALLBACK(launcher_entry_changed), p);

	/* Icon-Filename: выбор файла изображения */
	fc = gtk_file_chooser_button_new("Icon-Filename",
	                                 GTK_FILE_CHOOSER_ACTION_OPEN);
	filter = gtk_file_filter_new();
	gtk_file_filter_set_name(filter, "Images");
	gtk_file_filter_add_mime_type(filter, "image/svg+xml");
	gtk_file_filter_add_mime_type(filter, "image/png");
	gtk_file_filter_add_mime_type(filter, "image/jpeg");
	gtk_file_filter_add_mime_type(filter, "image/gif");
	gtk_file_filter_add_mime_type(filter, "image/x-xpixmap");
	gtk_file_chooser_add_filter(GTK_FILE_CHOOSER(fc), filter);
	if (priv->icon_path && priv->icon_path[0])
		gtk_file_chooser_set_filename(GTK_FILE_CHOOSER(fc),
		                              priv->icon_path);
	w = xs_prop_add_row(GTK_BOX(page), "Icon-Filename",
	                    "The image to display on this Launcher ...", fc);
	(void)w;
	g_signal_connect(fc, "file-set", G_CALLBACK(launcher_icon_set), p);

	gtk_widget_show_all(page);
}

static XsPluginOps ops = {
	.init = launcher_init,
	.draw = launcher_draw,
	.tick = NULL,
	.button = launcher_button,
	.motion = NULL,
	.shutdown = launcher_shutdown,
	.menu = NULL,
	.menu_cmd = launcher_menu_cmd,
	.properties = launcher_properties,
	.fill_themes = NULL
};

static XsPluginDesc desc = {
	.name = "launcher",
	.api_version = XS_API_VERSION,
	.ops = &ops,
	.desc = "A Screenlet that executes any kind of application or "
	        "shell-command when clicked. You can simply drag&drop an icon "
	        "from your mainmenu or panel on the Launcher's window to "
	        "initialize it for the given app.",
	.author = "RYX (aka Rico Pfaus)",
	.version = "0.7"
};

XsPluginDesc *xs_plugin_desc(void)
{
	return &desc;
}
