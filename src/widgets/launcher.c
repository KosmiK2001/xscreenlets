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
	int end_size_w, end_size_h; /* end_size: конечный размер px */
	int win_w, win_h;       /* итоговый размер окна (память) */
	char *scale_mode;       /* "scale" | "end_size" */
	gboolean hovered;       /* курсор над лаунчером (подсветка) */
	double click_glow;      /* 0..1 затухающая вспышка клика */
	guint glow_id;          /* таймер затухания вспышки */
} PrivData;

/* затухание вспышки клика: 40 мс шаг, ~0.4 сек */
static gboolean launcher_glow_tick(gpointer data)
{
	XsPlugin *p = data;
	PrivData *priv = p ? p->priv : NULL;

	if (!priv)
		return G_SOURCE_REMOVE;
	priv->click_glow -= 0.1;
	if (priv->click_glow <= 0.0) {
		priv->click_glow = 0.0;
		priv->glow_id = 0;
	} else if (p->win) {
		gtk_widget_queue_draw(p->win);
	}
	return G_SOURCE_REMOVE;
}

static void launcher_enter(XsPlugin *p)
{
	PrivData *priv = p ? p->priv : NULL;

	if (!priv)
		return;
	priv->hovered = TRUE;
	if (p->win)
		gtk_widget_queue_draw(p->win);
}

static void launcher_leave(XsPlugin *p)
{
	PrivData *priv = p ? p->priv : NULL;

	if (!priv)
		return;
	priv->hovered = FALSE;
	if (p->win)
		gtk_widget_queue_draw(p->win);
}

/* Новая логика картинки-отрисовка:
 *  - при ЗАПУСКЕ изображение НЕ грузится вовсе (быстрый старт,
 *    ноль памяти на картинки);
 *  - буфер (pixbuf нужного размера) строится ТОЛЬКО на время
 *    отрисовки кадра: файл читается → ресайз до целевого окна →
 *    нарисовано → буфер сброшен (кэш на время одного draw);
 *  - перечитывание при манипуляциях в Properties — тем же путём
 *    (каждый draw перечитывает), плюс предпросмотр в диалоге.
 *  - end_size = WxH: конечный размер изображения В ПИКСЕЛЯХ,
 *    масштаб = end_size / natural. Если end_size не задан —
 *    работает scale (с сотыми долями: шаг 0.01). */
static cairo_surface_t *launcher_render_buffer(PrivData *priv,
                                               int target_w,
                                               int target_h)
{
	GdkPixbuf *full = NULL;
	GdkPixbuf *scaled = NULL;
	cairo_surface_t *surf = NULL;
	GError *err = NULL;

	if (!priv->icon_path || !priv->icon_path[0])
		return NULL;
	if (target_w < 1) target_w = 1;
	if (target_h < 1) target_h = 1;

	if (g_str_has_suffix(priv->icon_path, ".svg") ||
	    g_str_has_suffix(priv->icon_path, ".SVG")) {
		RsvgHandle *svg = rsvg_handle_new_from_file(
		    priv->icon_path, &err);
		double nat_w = target_w, nat_h = target_h;
		RsvgDimensionData dim = {0};
		cairo_surface_t *surf;
		cairo_t *tcr;
		RsvgRectangle vp;

		if (!svg) {
			if (err)
				g_error_free(err);
			return NULL;
		}
		/* natural для расчёта масштаба end_size/scale */
		rsvg_handle_get_dimensions(svg, &dim);
		if (dim.width > 0 && dim.height > 0) {
			nat_w = dim.width;
			nat_h = dim.height;
		}
		/* итог = (end_size по оси, иначе natural) × scale */
		target_w = (int)((priv->end_size_w > 0
		                      ? priv->end_size_w : nat_w) *
		                 priv->scale);
		target_h = (int)((priv->end_size_h > 0
		                      ? priv->end_size_h : nat_h) *
		                 priv->scale);
		if (target_w < 1) target_w = 1;
		if (target_h < 1) target_h = 1;
		surf = cairo_image_surface_create(CAIRO_FORMAT_ARGB32,
		                                  target_w, target_h);
		tcr = cairo_create(surf);
		vp.x = 0; vp.y = 0;
		vp.width = target_w; vp.height = target_h;
		rsvg_handle_render_document(svg, tcr, &vp, NULL);
		cairo_destroy(tcr);
		g_object_unref(svg);
		return surf;
	} else {
		/* растр: загрузка full → scale */
		full = gdk_pixbuf_new_from_file(priv->icon_path, &err);
		if (!full) {
			if (err)
				g_error_free(err);
			return NULL;
		}
		/* итог = (end_size по оси, иначе natural) × scale */
		target_w = (int)((priv->end_size_w > 0
		                      ? priv->end_size_w
		                      : gdk_pixbuf_get_width(full)) *
		                 priv->scale);
		target_h = (int)((priv->end_size_h > 0
		                      ? priv->end_size_h
		                      : gdk_pixbuf_get_height(full)) *
		                 priv->scale);
		if (target_w < 1) target_w = 1;
		if (target_h < 1) target_h = 1;
		{
			cairo_surface_t *s = cairo_image_surface_create(
			    CAIRO_FORMAT_ARGB32, target_w, target_h);
			cairo_t *tcr = cairo_create(s);

			if (scaled) {
				gdk_cairo_set_source_pixbuf(tcr, scaled,
				                            0, 0);
				cairo_paint(tcr);
				g_object_unref(scaled);
			} else {
				/* fallback: растянуть full без scale_simple
				 * (NULL от него — известная странность на
				 * мелких размерах) */
				gdk_cairo_set_source_pixbuf(tcr, full, 0,
				                            0);
				cairo_scale(tcr,
				            (double)target_w /
				                gdk_pixbuf_get_width(full),
				            (double)target_h /
				                gdk_pixbuf_get_height(full));
				cairo_paint(tcr);
			}
			cairo_destroy(tcr);
			g_object_unref(scaled);
			g_object_unref(full);
			return s;
		}
		g_object_unref(full);
	}
	return NULL;
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

/* Итоговый размер изображения (px): end_size имеет приоритет,
 * иначе natural × scale. Natural берётся из файла (только для
 * расчёта — файл читается в launcher_render_buffer на draw). */
static void launcher_target_size(PrivData *priv, int *tw, int *th)
{
	double nw = 0, nh = 0;
	GdkPixbuf *probe;
	GError *err = NULL;

	if (priv->icon_path &&
	    (g_str_has_suffix(priv->icon_path, ".svg") ||
	     g_str_has_suffix(priv->icon_path, ".SVG"))) {
		RsvgHandle *svg = rsvg_handle_new_from_file(
		    priv->icon_path, &err);
		RsvgDimensionData dim = {0};

		if (svg) {
			rsvg_handle_get_dimensions(svg, &dim);
			nw = dim.width;
			nh = dim.height;
			g_object_unref(svg);
		}
	} else if (priv->icon_path) {
		probe = gdk_pixbuf_new_from_file(priv->icon_path, &err);
		if (probe) {
			nw = gdk_pixbuf_get_width(probe);
			nh = gdk_pixbuf_get_height(probe);
			g_object_unref(probe);
		}
	}
	/* РЕЖИМ масштабирования:
	 *  scale    — окно = natural × scale (end_size игнорируется)
	 *  end_size — окно = end_size (асимметричный; scale не влияет) */
	if (priv->scale_mode && strcmp(priv->scale_mode, "end_size") == 0) {
		*tw = priv->end_size_w > 0 ? priv->end_size_w : 64;
		*th = priv->end_size_h > 0 ? priv->end_size_h : 64;
	} else {
		*tw = (int)(nw * priv->scale);
		*th = (int)(nh * priv->scale);
	}
	if (*tw < 8) *tw = 8;
	if (*th < 8) *th = 8;
}

static void launcher_apply_icon(XsPlugin *p, PrivData *priv)
{
	int w, h;

	launcher_target_size(priv, &w, &h);
	/* запомнить ИТОГОВЫЙ размер окна: при рестарте окно должно
	 * встать ровно таким, каким юзер его оставил */
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
			g_free(priv->icon_path);
			priv->icon_path = fn;
			g_key_file_set_string(priv->kf, p->name,
			                      "icon", priv->icon_path);
			changed = TRUE;
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
	if (priv->scale < 0.01)
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
	/* end_size: конечный размер изображения в px (опция; "0" = выкл) */
	priv->end_size_w = xs_host_api()->conf_int(kf, p->name,
	                                           "end_size_w", 0);
	priv->end_size_h = xs_host_api()->conf_int(kf, p->name,
	                                           "end_size_h", 0);
	priv->scale_mode = xs_host_api()->conf_str(kf, p->name,
	                                           "scale_mode", "scale");
	priv->x = xs_host_api()->conf_int(kf, p->name, "x", 80);
	priv->y = xs_host_api()->conf_int(kf, p->name, "y", 80);

	/* ИТОГОВЫЙ размер окна из конфига (win_w/win_h пишет
	 * launcher_apply_icon при каждой смене scale/иконки): при
	 * рестарте окно ровно такое, каким юзер его оставил.
	 * Файл на старте НЕ читается — картинка строится в draw. */
	{
		int w, h;

		/* окно = natural×scale (или end_size): никаких legacy
		 * win_w/h — при смене иконки/масштаба размер честный */
		launcher_target_size(priv, &w, &h);
		priv->win_w = w;
		priv->win_h = h;
		p->win = xs_host_api()->make_window(p, priv->x, priv->y,
		                                    w, h);
	}
	if (!p->win) {
		p->host->log("launcher: failed to create window");
		g_free(priv->action);
		g_free(priv->icon_path);
		g_free(priv->label);
		g_free(priv->scale_mode);
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
	/* Буфер строится на время отрисовки и сразу сбрасывается:
	 * файл перечитывается на каждом draw (дёшево: раз в сек/сек10) */
	{
		cairo_surface_t *buf = launcher_render_buffer(priv, w, h);

		if (buf) {
			cairo_set_source_surface(cr, buf, 0, 0);
			cairo_paint(cr);
			/* hover/glow — МАСКОЙ по альфе иконки:
			 * подсвечивается сама иконка, не квадрат окна */
			if (priv->hovered) {
				cairo_set_source_rgb(cr, 1.0, 1.0, 1.0);
				cairo_push_group(cr);
				cairo_set_source_rgba(cr, 1.0, 1.0, 1.0,
				                      0.35);
				cairo_paint(cr);
				cairo_pop_group_to_source(cr);
				cairo_mask_surface(cr, buf, 0, 0);
			}
			if (priv->click_glow > 0.0) {
				cairo_push_group(cr);
				cairo_set_source_rgba(cr, 1.0, 0.85, 0.3,
				                      0.7 * priv->click_glow);
				cairo_paint(cr);
				cairo_pop_group_to_source(cr);
				cairo_mask_surface(cr, buf, 0, 0);
			}
			cairo_surface_destroy(buf);
		}
	}
}

static void launcher_start_glow(XsPlugin *p)
{
	PrivData *priv = p ? p->priv : NULL;

	if (!priv)
		return;
	priv->click_glow = 1.0;
	if (!priv->glow_id)
		priv->glow_id = g_timeout_add(40, launcher_glow_tick, p);
	if (p->win)
		gtk_widget_queue_draw(p->win);
}

static gboolean launcher_button(XsPlugin *p, GdkEventButton *ev)
{
	PrivData *priv = p->priv;

	if (!priv || ev->type != GDK_BUTTON_PRESS)
		return FALSE;
	if (ev->button == 1) {
		launcher_start_glow(p); /* визуал: апплет что-то запустил */
		launcher_launch(priv);
		return TRUE;
	}
	return FALSE;
}

static void launcher_shutdown(XsPlugin *p)
{
	PrivData *priv = p->priv;

	if (priv) {
		if (priv->glow_id) {
			g_source_remove(priv->glow_id);
			priv->glow_id = 0;
		}
		g_free(priv->action);
		g_free(priv->icon_path);
		g_free(priv->label);
		g_free(priv->scale_mode);
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

		/* сотые доли: нижний порог 0.01 */
		if (s < 0.01) s = 0.01; else if (s > 10.0) s = 10.0;
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
	if (!fn || !fn[0]) {
		g_free(fn);
		return;
	}
	g_free(priv->icon_path);
	priv->icon_path = fn;
	g_key_file_set_string(priv->kf, p->name, "icon", priv->icon_path);
	xs_core_plugin_conf_flush(p->name);
	launcher_apply_icon(p, priv);
	if (p->win)
		gtk_widget_queue_draw(p->win);
}

/* Режим масштабирования: scale | end_size — смена + apply */
static void fl_mode_changed(GtkComboBox *combo, gpointer data)
{
	XsPlugin *p = data;
	PrivData *priv;
	char *sel;

	if (!p || !p->priv)
		return;
	priv = p->priv;
	sel = gtk_combo_box_text_get_active_text(GTK_COMBO_BOX_TEXT(combo));
	if (!sel || !sel[0]) {
		g_free(sel);
		return;
	}
	g_free(priv->scale_mode);
	priv->scale_mode = g_strdup(sel);
	xs_host_api()->conf_set_str(priv->kf, p->name, "scale_mode",
	                            priv->scale_mode);
	xs_core_plugin_conf_flush(p->name);
	launcher_apply_icon(p, priv);
	if (p->win)
		gtk_widget_queue_draw(p->win);
	g_free(sel);
}

/* End size: жёсткий конечный размер картинки (px). 0 = по Scale. */
static void fl_end_size_changed(GtkSpinButton *spin, gpointer data)
{
	XsPlugin *p = data;
	PrivData *priv;
	const char *key = g_object_get_data(G_OBJECT(spin), "xs-key");
	int v;

	if (!p || !p->priv || !key)
		return;
	priv = p->priv;
	v = gtk_spin_button_get_value_as_int(spin);
	xs_host_api()->conf_set_int(priv->kf, p->name, key, v);
	if (strcmp(key, "end_size_w") == 0)
		priv->end_size_w = v;
	else
		priv->end_size_h = v;
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

	/* Режим масштабирования: от Scale или от End size */
	{
		static const char *const modes[] = {
		    "scale", "end_size", NULL
		};
		const char *cur = priv->scale_mode ? priv->scale_mode
		                                   : "scale";
		GtkWidget *w = xs_prop_add_choices(
		    GTK_BOX(page), "Resize mode",
		    "scale = natural × Scale (End size игнорируется); "
		    "end_size = жёсткий размер End size (Scale не влияет)",
		    modes, cur);
		GtkWidget *lbl_w, *lbl_h;
		GtkAdjustment *adj_w, *adj_h;
		GtkWidget *ew, *eh, *hbx;

		g_signal_connect(w, "changed",
		                 G_CALLBACK(fl_mode_changed), p);

		/* End size: спиннеры (активны в любом режиме, но применяются
		 * только в end_size) */
		adj_w = gtk_adjustment_new(priv->end_size_w, 0, 4096, 1, 16,
		                           0);
		adj_h = gtk_adjustment_new(priv->end_size_h, 0, 4096, 1, 16,
		                           0);
		ew = gtk_spin_button_new(adj_w, 1, 0);
		eh = gtk_spin_button_new(adj_h, 1, 0);
		gtk_widget_set_tooltip_text(
		    ew, "Конечная ширина картинки в px (mode=end_size)");
		gtk_widget_set_tooltip_text(
		    eh, "Конечная высота картинки в px (mode=end_size)");
		lbl_w = gtk_label_new("End width");
		lbl_h = gtk_label_new("End height");
		gtk_grid_attach(GTK_GRID(page), lbl_w, 0, 100, 1, 1);
		gtk_grid_attach(GTK_GRID(page), ew, 1, 100, 1, 1);
		gtk_grid_attach(GTK_GRID(page), lbl_h, 0, 101, 1, 1);
		gtk_grid_attach(GTK_GRID(page), eh, 1, 101, 1, 1);
		g_object_set_data_full(G_OBJECT(ew), "xs-key",
		                       g_strdup("end_size_w"), g_free);
		g_object_set_data_full(G_OBJECT(eh), "xs-key",
		                       g_strdup("end_size_h"), g_free);
		g_signal_connect(ew, "value-changed",
		                 G_CALLBACK(fl_end_size_changed), p);
		g_signal_connect(eh, "value-changed",
		                 G_CALLBACK(fl_end_size_changed), p);
		(void)hbx;
	}

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
	.fill_themes = NULL,
	.enter = launcher_enter,
	.leave = launcher_leave
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
