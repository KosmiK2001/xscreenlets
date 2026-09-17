/* frame_launcher.c — плагин-хост в стиле Vista Sidebar Gadgets:
 * рамка (SVG-тема) с полупрозрачным фоном, внутри — другие апплеты
 * (гости), чьи окна репарентятся в content-зону (X обрезает выход за
 * границы). Тема: ~/.config/xscreenlets/themes/frame_launcher/<name>/
 * (frame.svg, backdrop.svg) или /usr/share/screenlets/FrameLauncher.
 * Гости: конфиги .plugins/<имя>.conf, ключи guests_1..N (имена инстансов).
 * Вложенность: гость-рамка сам может хостить — циклы проверяет демон. */
#include <gtk/gtk.h>
#include <glib.h>
#include <librsvg/rsvg.h>
#include <string.h>
#include "xs_api.h"
#include "common.h"

typedef struct {
	GKeyFile *kf;
	char *theme;             /* имя темы */
	double opacity;          /* прозрачность рамки */
	double bg_opacity;       /* прозрачность фона content-зоны */
	int x, y;
	int width, height;       /* внешний размер */
	int shadow;              /* зона затенения px (2-4) */
	int frame_l, frame_r, frame_t, frame_b; /* толщина рамки (тема) */
	int guest_count;
	char **guests;           /* имена гостей (guests_1..N) */
	gboolean started;        /* гости запущены (после map окна) */
} PrivData;

static void fl_read_theme_frame(PrivData *priv, const char *dir)
{
	GKeyFile *kf;
	char *path;

	if (!dir)
		return;
	path = g_build_filename(dir, "theme.conf", NULL);
	kf = g_key_file_new();
	if (g_key_file_load_from_file(kf, path, G_KEY_FILE_NONE, NULL)) {
		priv->frame_l = g_key_file_get_integer(kf, "frame_launcher",
		                                       "frame_left", NULL);
		priv->frame_r = g_key_file_get_integer(kf, "frame_launcher",
		                                       "frame_right", NULL);
		priv->frame_t = g_key_file_get_integer(kf, "frame_launcher",
		                                       "frame_top", NULL);
		priv->frame_b = g_key_file_get_integer(kf, "frame_launcher",
		                                       "frame_bottom", NULL);
	}
	g_key_file_free(kf);
	g_free(path);
}

static void fl_load_theme(XsPlugin *p, PrivData *priv)
{
	const char *type = p->type ? p->type : p->name;
	char *dir;

	dir = g_build_filename(g_get_user_config_dir(), "xscreenlets",
	                       "themes", type, priv->theme, NULL);
	if (!xs_host_api()->theme_load(p, dir)) {
		g_free(dir);
		dir = g_build_filename("/usr/share/screenlets",
		                       "FrameLauncher", "themes",
		                       priv->theme, NULL);
		if (!xs_host_api()->theme_load(p, dir)) {
			xs_host_api()->log("frame_launcher: theme '%s' not found, built-in frame",
			                   priv->theme);
			g_free(dir);
			return;
		}
	}
	fl_read_theme_frame(priv, dir);
	g_free(dir);
}

/* рассчитать content-зону: внешний размер − рамка − затенение */
static void fl_content_rect(PrivData *priv, int *cx, int *cy,
                            int *cw, int *ch)
{
	int sh = priv->shadow < 0 ? 0 : priv->shadow;

	*cx = priv->frame_l + sh;
	*cy = priv->frame_t + sh;
	*cw = priv->width - priv->frame_l - priv->frame_r - 2 * sh;
	*ch = priv->height - priv->frame_t - priv->frame_b - 2 * sh;
	if (*cw < 8)
		*cw = 8;
	if (*ch < 8)
		*ch = 8;
}

static void fl_start_guests(XsPlugin *p, PrivData *priv);

/* idle-обёртка запуска гостей (окно хоста уже отображено) */
static gboolean fl_start_guests_idle(gpointer data)
{
	XsPlugin *p = data;
	PrivData *priv = p ? p->priv : NULL;

	if (!priv)
		return G_SOURCE_REMOVE;
	fl_start_guests(p, priv);
	return G_SOURCE_REMOVE;
}

static void fl_start_guests(XsPlugin *p, PrivData *priv)
{
	if (priv->started)
		return;
	priv->started = TRUE;
	for (int i = 0; i < priv->guest_count; i++) {
		if (!priv->guests[i] || !priv->guests[i][0])
			continue;
		xs_host_api()->log("frame_launcher %s: starting guest '%s'",
		                   p->name, priv->guests[i]);
		if (!xs_host_api()->start_guest(p, priv->guests[i]))
			xs_host_api()->log("frame_launcher %s: guest '%s' failed",
			                   p->name, priv->guests[i]);
	}
}

static void fl_position_guests(XsPlugin *p, PrivData *priv)
{
	/* гости репарентятся с (0,0) внутри content-зоны; их позиции x/y
	 * из их конфигов интерпретируются относительно рамки — двигаем
	 * content-окно уже не нужно, гость сам знает свои x/y. Пока
	 * просто отмечаем: смещение гостей задаёт демон при репаренте. */
	(void)p;
	(void)priv;
}

/* ---------- ops ---------- */

static int fl_init(XsPlugin *p, GKeyFile *kf)
{
	PrivData *priv = g_new0(PrivData, 1);

	p->priv = priv;
	priv->kf = kf;
	priv->theme = xs_host_api()->conf_str(kf, p->name, "theme", "vista");
	priv->opacity = xs_host_api()->conf_dbl(kf, p->name, "opacity", 0.9);
	if (priv->opacity < 0.1)
		priv->opacity = 0.1;
	else if (priv->opacity > 1.0)
		priv->opacity = 1.0;
	priv->bg_opacity = xs_host_api()->conf_dbl(kf, p->name,
	                                           "bg_opacity", 0.35);
	if (priv->bg_opacity < 0.0)
		priv->bg_opacity = 0.0;
	else if (priv->bg_opacity > 1.0)
		priv->bg_opacity = 1.0;
	priv->x = xs_host_api()->conf_int(kf, p->name, "x", 80);
	priv->y = xs_host_api()->conf_int(kf, p->name, "y", 80);
	priv->width = xs_host_api()->conf_int(kf, p->name, "width", 300);
	priv->height = xs_host_api()->conf_int(kf, p->name, "height", 400);
	priv->shadow = xs_host_api()->conf_int(kf, p->name, "shadow", 3);
	if (priv->shadow < 0)
		priv->shadow = 0;
	else if (priv->shadow > 16)
		priv->shadow = 16;
	/* рамка по умолчанию (если тема не переопределила) */
	if (!priv->frame_l) priv->frame_l = 12;
	if (!priv->frame_r) priv->frame_r = 12;
	if (!priv->frame_t) priv->frame_t = 12;
	if (!priv->frame_b) priv->frame_b = 12;
	/* гости: guests_1..N */
	{
		int i;

		for (i = 0; i < 32; i++) {
			char *key = g_strdup_printf("guests_%d", i + 1);
			char *v = xs_host_api()->conf_str(kf, p->name, key, "");

			g_free(key);
			if (!v[0]) {
				g_free(v);
				break;
			}
			priv->guests = g_realloc(priv->guests,
			                         (i + 1) * sizeof(char *));
			priv->guests[i] = v;
		}
		priv->guest_count = i;
	}
	xs_host_api()->log("frame_launcher %s: guests=%d", p->name,
	                   priv->guest_count);
	/* started_by по умолчанию main_daemon */
	{
		char *sb = xs_host_api()->conf_str(kf, p->name,
		                                   "started_by", "");

		if (!sb[0])
			xs_host_api()->conf_set_str(kf, p->name,
			                            "started_by",
			                            "main_daemon");
		g_free(sb);
	}

	fl_load_theme(p, priv);

	p->win = xs_host_api()->make_window(p, priv->x, priv->y,
	                                    priv->width, priv->height);
	if (!p->win) {
		xs_host_api()->log("frame_launcher: failed to create window");
		g_free(priv->theme);
		g_free(priv);
		p->priv = NULL;
		return -1;
	}
	xs_host_api()->set_opacity(p, priv->opacity);
	/* Content-контейнер для гостей: невидимый GtkFixed на всю
	 * content-зону; демона репарентит X-окна гостей сюда (X режет
	 * выход за родителя). Помечаем "xs-content". */
	{
		GtkWidget *fixed = gtk_fixed_new();
		int cx, cy, cw, ch;

		fl_content_rect(priv, &cx, &cy, &cw, &ch);
		gtk_fixed_put(GTK_FIXED(fixed), gtk_label_new(NULL), 0, 0);
		gtk_widget_set_size_request(fixed, cw, ch);
		g_object_set_data(G_OBJECT(p->win), "xs-content", fixed);
		/* оффсет content-зоны для репарента (fixed без X-окна) */
		{
			gint *xy = g_new(gint, 2);

			xy[0] = cx;
			xy[1] = cy;
			g_object_set_data_full(G_OBJECT(p->win),
			                       "xs-content-xy", xy, g_free);
		}
	}
	/* гости запускаем после показа окна (нужен X window для репарента) */
	g_idle_add_full(G_PRIORITY_LOW, fl_start_guests_idle, p, NULL);
	return 0;
}

static void fl_draw(XsPlugin *p, cairo_t *cr, int w, int h)
{
	PrivData *priv = p->priv;
	int cx, cy, cw, ch;

	if (!priv)
		return;
	cairo_set_operator(cr, CAIRO_OPERATOR_OVER);
	/* фон content-зоны (backdrop), полупрозрачный */
	fl_content_rect(priv, &cx, &cy, &cw, &ch);
	if (priv->bg_opacity > 0.0) {
		cairo_save(cr);
		cairo_set_source_rgba(cr, 0.06, 0.06, 0.08, priv->bg_opacity);
		cairo_rectangle(cr, cx, cy, cw, ch);
		cairo_fill(cr);
		cairo_restore(cr);
	}
	/* рамка из темы (элемент frame, растянут на всё окно) */
	if (xs_core_theme_has(p, "frame")) {
		xs_host_api()->theme_draw_full(p, cr, "frame", 0, 0, w, h);
	} else {
		/* встроенная рамка: скруглённый прямоугольник */
		double radius = 10.0;

		cairo_save(cr);
		cairo_move_to(cr, radius, 0);
		cairo_line_to(cr, w - radius, 0);
		cairo_curve_to(cr, w - 4, 0, w, 4, w, radius);
		cairo_line_to(cr, w, h - radius);
		cairo_curve_to(cr, w, h - 4, w - 4, h, w - radius, h);
		cairo_line_to(cr, radius, h);
		cairo_curve_to(cr, 4, h, 0, h - 4, 0, h - radius);
		cairo_line_to(cr, 0, radius);
		cairo_curve_to(cr, 0, 4, 4, 0, radius, 0);
		cairo_close_path(cr);
		cairo_set_source_rgba(cr, 0.95, 0.95, 1.0, 0.25);
		cairo_set_line_width(cr, 2.0);
		cairo_stroke(cr);
		cairo_restore(cr);
	}
}

static guint fl_tick(XsPlugin *p)
{
	(void)p;
	return 0; /* статичный плагин: таймер не нужен */
}

static void fl_shutdown(XsPlugin *p)
{
	PrivData *priv = p->priv;

	if (priv) {
		/* гости — самостоятельные инстансы; их окна репарентились в
		 * content-окно, которое умрёт вместе с хостом. Останавливаем. */
		for (int i = 0; i < priv->guest_count; i++)
			xs_host_api()->stop_guest(p, priv->guests[i]);
		for (int i = 0; i < priv->guest_count; i++)
			g_free(priv->guests[i]);
		g_free(priv->guests);
		g_free(priv->theme);
		g_free(priv);
		p->priv = NULL;
	}
}

/* ---------- Properties: вкладка Frame ---------- */

static void fl_spin_changed(GtkSpinButton *spin, gpointer data)
{
	XsPlugin *p = data;
	PrivData *priv;
	const char *key = g_object_get_data(G_OBJECT(spin), "xs-key");
	int v;

	if (!p || !p->priv || !key)
		return;
	priv = p->priv;
	v = gtk_spin_button_get_value_as_int(spin);
	if (strcmp(key, "width") == 0)
		priv->width = v;
	else if (strcmp(key, "height") == 0)
		priv->height = v;
	else if (strcmp(key, "shadow") == 0)
		priv->shadow = v;
	xs_host_api()->conf_set_int(priv->kf, p->name, key, v);
	xs_core_plugin_conf_flush(p->name);
	/* живое применение размера/затенения */
	if (strcmp(key, "width") == 0 || strcmp(key, "height") == 0)
		xs_host_api()->resize(p, priv->width, priv->height);
	else
		gtk_widget_queue_draw(p->win);
}

static void fl_entry_changed(GtkEditable *e, gpointer data)
{
	XsPlugin *p = data;
	PrivData *priv;
	const char *text;

	if (!p || !p->priv)
		return;
	priv = p->priv;
	text = gtk_entry_get_text(GTK_ENTRY(e));
	g_free(priv->theme);
	priv->theme = g_strdup(text && text[0] ? text : "vista");
	xs_host_api()->conf_set_str(priv->kf, p->name, "theme",
	                            priv->theme);
	xs_core_plugin_conf_flush(p->name);
}

static void fl_guests_changed(GtkTextBuffer *buf, gpointer data)
{
	XsPlugin *p = data;
	PrivData *priv;
	GtkTextIter s, e;
	char *text, **lines;
	gsize n;

	if (!p || !p->priv)
		return;
	priv = p->priv;
	gtk_text_buffer_get_bounds(buf, &s, &e);
	text = gtk_text_buffer_get_text(buf, &s, &e, FALSE);
	lines = g_strsplit(text, "\n", -1);
	g_free(text);
	/* перезаписать guests_1..N; старые ключи очистить */
	for (gsize i = 0; ; i++) {
		char *key = g_strdup_printf("guests_%zu", i + 1);

		if (!lines[i] || !lines[i][0]) {
			g_free(key);
			break;
		}
		/* вырезать пробелы по краям */
		char *t = g_strstrip(g_strdup(lines[i]));
		xs_host_api()->conf_set_str(priv->kf, p->name, key, t);
		g_free(t);
		g_free(key);
	}
	g_strfreev(lines);
	xs_core_plugin_conf_flush(p->name);
}

static void fl_properties(XsPlugin *p, GtkNotebook *nb)
{
	PrivData *priv;
	GtkWidget *page, *lbl, *w;
	GtkAdjustment *adj;

	if (!p || !p->priv)
		return;
	priv = p->priv;

	page = gtk_box_new(GTK_ORIENTATION_VERTICAL, 3);
	gtk_container_set_border_width(GTK_CONTAINER(page), 10);
	lbl = gtk_label_new("Frame Launcher: рамка-хост для других апплетов "
	                    "(в стиле Vista Sidebar).");
	gtk_widget_set_halign(lbl, GTK_ALIGN_START);
	gtk_box_pack_start(GTK_BOX(page), lbl, FALSE, FALSE, 7);
	gtk_box_pack_start(GTK_BOX(page),
	                   gtk_separator_new(GTK_ORIENTATION_HORIZONTAL),
	                   FALSE, FALSE, 5);
	gtk_notebook_append_page(nb, page, gtk_label_new("Frame"));

#define FL_SPIN(key, label, desc, min, max)                              \
	do {                                                             \
		adj = gtk_adjustment_new(                                \
		    xs_host_api()->conf_int(priv->kf, p->name, key, 0),  \
		    min, max, 1, 10, 0);                                 \
		w = xs_prop_add_row(GTK_BOX(page), label, desc,          \
		                    gtk_spin_button_new(adj, 1, 0));     \
		g_object_set_data_full(G_OBJECT(w), "xs-key",            \
		                       g_strdup(key), g_free);           \
		g_signal_connect(w, "value-changed",                     \
		                 G_CALLBACK(fl_spin_changed), p);        \
	} while (0)

	FL_SPIN("width", "Width", "Внешняя ширина рамки", 60, 4000);
	FL_SPIN("height", "Height", "Внешняя высота рамки", 60, 4000);
	FL_SPIN("shadow", "Shadow", "Зона затенения (px), 2-4 типично", 0, 16);

#undef FL_SPIN

	w = xs_prop_add_string(GTK_BOX(page), "Theme",
	                       "Имя темы (каталог в themes/frame_launcher)",
	                       priv->theme);
	g_signal_connect(w, "changed", G_CALLBACK(fl_entry_changed), p);

	/* Гости: многострочное поле, имя инстанса на строку */
	{
		GtkWidget *tv = gtk_text_view_new();
		GtkTextBuffer *buf = gtk_text_view_get_buffer(
		    GTK_TEXT_VIEW(tv));
		GtkWidget *sw = gtk_scrolled_window_new(NULL, NULL);
		GString *gs = g_string_new("");

		gtk_text_view_set_monospace(GTK_TEXT_VIEW(tv), TRUE);
		gtk_widget_set_size_request(sw, -1, 90);
		gtk_container_add(GTK_CONTAINER(sw), tv);
		for (int i = 0; i < priv->guest_count; i++)
			g_string_append_printf(gs, "%s\n", priv->guests[i]);
		gtk_text_buffer_set_text(buf, gs->str, -1);
		g_string_free(gs, TRUE);
		g_signal_connect(buf, "changed",
		                 G_CALLBACK(fl_guests_changed), p);
		xs_prop_add_row(GTK_BOX(page), "Guests (по одному на строку)",
		                "Имена инстансов из .plugins (конфигов); "
		                "гость-рамка может хостить своих гостей",
		                sw);
	}

	gtk_widget_show_all(page);
}

static XsPluginOps ops = {
	.init = fl_init,
	.draw = fl_draw,
	.tick = fl_tick,
	.button = NULL,
	.motion = NULL,
	.shutdown = fl_shutdown,
	.menu = NULL,
	.menu_cmd = NULL,
	.properties = fl_properties,
	.fill_themes = NULL
};

static XsPluginDesc desc = {
	.name = "frame_launcher",
	.api_version = XS_API_VERSION,
	.ops = &ops,
	.desc = "Vista-style frame that hosts other screenlets. Guests are "
	        "clipped to the frame's content area and always render "
	        "below the frame. Configure guests via guests_1..N keys.",
	.author = "kosmik2001",
	.version = "0.1"
};

XsPluginDesc *xs_plugin_desc(void)
{
	return &desc;
}
