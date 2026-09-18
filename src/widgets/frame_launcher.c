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
		char *offkey, *offv;

		if (!priv->guests[i] || !priv->guests[i][0])
			continue;
		/* гость выключен галочкой — не запускаем */
		offkey = g_strdup_printf("guests_%d_off", i + 1);
		offv = xs_host_api()->conf_str(priv->kf, p->name, offkey,
		                               "");
		g_free(offkey);
		if (offv && offv[0]) {
			g_free(offv);
			continue;
		}
		g_free(offv);
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

	/* Тема загружается ПОСЛЕ make_window (theme_load требует state).
	 * Толщины рамки читаются из theme.conf там же. */

	p->win = xs_host_api()->make_window(p, priv->x, priv->y,
	                                    priv->width, priv->height);
	fl_load_theme(p, priv);
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

/* 9-slice отрисовка рамки из темы: углы — в натуральном размере,
 * стороны — тайлами/растяжкой между углами, центр (backdrop) —
 * растяжкой на content-зону. Элементы темы:
 *   frame-tl, frame-tr, frame-bl, frame-br  — углы (native size)
 *   frame-top, frame-bottom, frame-left, frame-right — стороны
 *   backdrop — фон content-зоны
 * Если в теме есть единый frame.svg — растягиваем его целиком
 * (простые темы без 9-slice). */
static void fl_draw_frame_nine_slice(XsPlugin *p, PrivData *priv,
                                     cairo_t *cr, int w, int h)
{
	int L = priv->frame_l, R = priv->frame_r;
	int T = priv->frame_t, B = priv->frame_b;

	/* backdrop: растяжкой на content-зону; прозрачность — bg_opacity
	 * юзера (через временную группу, т.к. theme_draw_full рисует
	 * напрямую) */
	if (xs_core_theme_has(p, "backdrop") && priv->bg_opacity > 0.0) {
		int cx, cy, cw, ch;
		cairo_surface_t *tmp;
		cairo_t *tcr;

		fl_content_rect(priv, &cx, &cy, &cw, &ch);
		tmp = cairo_image_surface_create(CAIRO_FORMAT_ARGB32,
		                                 cw > 0 ? cw : 1,
		                                 ch > 0 ? ch : 1);
		tcr = cairo_create(tmp);
		xs_host_api()->theme_draw_full(p, tcr, "backdrop",
		                               0, 0, cw, ch);
		cairo_destroy(tcr);
		cairo_save(cr);
		cairo_set_source_surface(cr, tmp, cx, cy);
		cairo_paint_with_alpha(cr, priv->bg_opacity);
		cairo_restore(cr);
		cairo_surface_destroy(tmp);
	}
	/* углы */
	if (xs_core_theme_has(p, "frame-tl"))
		xs_host_api()->theme_draw_native(p, cr, "frame-tl", 0, 0);
	if (xs_core_theme_has(p, "frame-tr"))
		xs_host_api()->theme_draw_native(p, cr, "frame-tr",
		                                 w - R, 0);
	if (xs_core_theme_has(p, "frame-bl"))
		xs_host_api()->theme_draw_native(p, cr, "frame-bl",
		                                 0, h - B);
	if (xs_core_theme_has(p, "frame-br"))
		xs_host_api()->theme_draw_native(p, cr, "frame-br",
		                                 w - R, h - B);
	/* стороны: растяжка между углами */
	if (xs_core_theme_has(p, "frame-top") && w - L - R > 0)
		xs_host_api()->theme_draw_full(p, cr, "frame-top",
		                               L, 0, w - L - R, T);
	if (xs_core_theme_has(p, "frame-bottom") && w - L - R > 0)
		xs_host_api()->theme_draw_full(p, cr, "frame-bottom",
		                               L, h - B, w - L - R, B);
	if (xs_core_theme_has(p, "frame-left") && h - T - B > 0)
		xs_host_api()->theme_draw_full(p, cr, "frame-left",
		                               0, T, L, h - T - B);
	if (xs_core_theme_has(p, "frame-right") && h - T - B > 0)
		xs_host_api()->theme_draw_full(p, cr, "frame-right",
		                               w - R, T, R, h - T - B);
}

static void fl_draw(XsPlugin *p, cairo_t *cr, int w, int h)
{
	PrivData *priv = p->priv;
	int cx, cy, cw, ch;
	gboolean nine = xs_core_theme_has(p, "frame-tl") ||
	                xs_core_theme_has(p, "frame-top");

	if (!priv)
		return;
	cairo_set_operator(cr, CAIRO_OPERATOR_OVER);
	fl_content_rect(priv, &cx, &cy, &cw, &ch);
	if (nine) {
		/* backdrop рисуется внутри 9-slice первым слоем */
		fl_draw_frame_nine_slice(p, priv, cr, w, h);
		return;
	}
	/* фон content-зоны (backdrop), полупрозрачный */
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

/* ---------- меню и кнопки добавления гостей ---------- */

/* Список типов доступных плагинов (из host API). Реализация через
 * коллбек в common.c — там виден g_loaded_modules. */
extern char **xs_core_list_plugin_types(void);
extern int xs_core_type_count(void);

/* «Add Applet»: создать НОВЫЙ инстанс выбранного типа сразу как гостя
 * этой рамки (не через трей-автостарт). */
static void fl_add_applet_response(GtkDialog *dlg, int res, gpointer data)
{
	XsPlugin *p = data;
	PrivData *priv;
	GtkComboBox *combo;
	char *type;
	XsPlugin *g;
	int i;

	if (res != GTK_RESPONSE_ACCEPT || !p || !p->priv) {
		gtk_widget_destroy(GTK_WIDGET(dlg));
		return;
	}
	priv = p->priv;
	combo = g_object_get_data(G_OBJECT(dlg), "xs-combo");
	type = gtk_combo_box_text_get_active_text(GTK_COMBO_BOX_TEXT(combo));
	gtk_widget_destroy(GTK_WIDGET(dlg));
	if (!type || !type[0]) {
		g_free(type);
		return;
	}
	/* создать инстанс с UUID-именем и тут же включить в рамку */
	g = xs_host_api()->start_guest_new(p, type);
	if (g) {
		/* дописать в guests_N */
		i = priv->guest_count;
		while (i < 64) {
			char *key = g_strdup_printf("guests_%d", i + 1);
			char *v = xs_host_api()->conf_str(priv->kf, p->name,
			                                  key, "");

			g_free(key);
			if (v[0]) {
				g_free(v);
				i++;
				continue;
			}
			g_free(v);
			break;
		}
		if (i < 64) {
			char *key = g_strdup_printf("guests_%d", i + 1);

			xs_host_api()->conf_set_str(priv->kf, p->name, key,
			                            g->name);
			g_free(key);
			priv->guests = g_realloc(priv->guests,
			                         (i + 1) * sizeof(char *));
			priv->guests[i] = g_strdup(g->name);
			priv->guest_count = i + 1;
			xs_core_plugin_conf_flush(p->name);
		}
	}
	g_free(type);
}

static void fl_add_applet_clicked(GtkButton *btn, gpointer data)
{
	XsPlugin *p = data;
	GtkWidget *dlg, *box, *combo;
	char **types;
	int n, i;

	(void)btn;
	if (!p || !p->priv)
		return;
	dlg = gtk_dialog_new_with_buttons("Add Applet to Frame",
	                                  GTK_WINDOW(gtk_widget_get_toplevel(
	                                      p->win)),
	                                  GTK_DIALOG_MODAL, "Добавить",
	                                  GTK_RESPONSE_ACCEPT, "Отмена",
	                                  GTK_RESPONSE_CANCEL, NULL);
	box = gtk_dialog_get_content_area(GTK_DIALOG(dlg));
	gtk_box_pack_start(GTK_BOX(box),
	                   gtk_label_new("Тип апплета для нового гостя:"),
	                   FALSE, FALSE, 6);
	combo = gtk_combo_box_text_new();
	types = xs_core_list_plugin_types();
	n = xs_core_type_count();
	for (i = 0; i < n; i++)
		gtk_combo_box_text_append_text(GTK_COMBO_BOX_TEXT(combo),
		                               types[i]);
	gtk_combo_box_set_active(GTK_COMBO_BOX(combo), 0);
	g_object_set_data(G_OBJECT(dlg), "xs-combo", combo);
	gtk_box_pack_start(GTK_BOX(box), combo, FALSE, FALSE, 6);
	gtk_widget_show_all(dlg);
	g_signal_connect(dlg, "response",
	                 G_CALLBACK(fl_add_applet_response), p);
}

/* «Add Running»: забрать уже запущенный main_daemon-инстанс в рамку. */
static void fl_add_running_response(GtkDialog *dlg, int res, gpointer data)
{
	XsPlugin *p = data;
	PrivData *priv;
	GtkComboBox *combo;
	char *name;
	int i;

	if (res != GTK_RESPONSE_ACCEPT || !p || !p->priv) {
		gtk_widget_destroy(GTK_WIDGET(dlg));
		return;
	}
	priv = p->priv;
	combo = g_object_get_data(G_OBJECT(dlg), "xs-combo");
	name = gtk_combo_box_text_get_active_text(GTK_COMBO_BOX_TEXT(combo));
	gtk_widget_destroy(GTK_WIDGET(dlg));
	if (!name || !name[0]) {
		g_free(name);
		return;
	}
	/* start_guest сам: репарентит, started_by=plugin, убирает symlink */
	if (xs_host_api()->start_guest(p, name)) {
		i = priv->guest_count;
		while (i < 64) {
			char *key = g_strdup_printf("guests_%d", i + 1);
			char *v = xs_host_api()->conf_str(priv->kf, p->name,
			                                  key, "");

			g_free(key);
			if (v[0]) {
				g_free(v);
				i++;
				continue;
			}
			g_free(v);
			break;
		}
		if (i < 64) {
			char *key = g_strdup_printf("guests_%d", i + 1);

			xs_host_api()->conf_set_str(priv->kf, p->name, key,
			                            name);
			g_free(key);
			priv->guests = g_realloc(priv->guests,
			                         (i + 1) * sizeof(char *));
			priv->guests[i] = g_strdup(name);
			priv->guest_count = i + 1;
			xs_core_plugin_conf_flush(p->name);
		}
	}
	g_free(name);
}

extern char **xs_core_list_running_daemon_instances(int *count);

static void fl_add_running_clicked(GtkButton *btn, gpointer data)
{
	XsPlugin *p = data;
	GtkWidget *dlg, *box, *combo;
	char **names;
	int n, i;

	(void)btn;
	if (!p || !p->priv)
		return;
	dlg = gtk_dialog_new_with_buttons("Add Running Applet to Frame",
	                                  GTK_WINDOW(gtk_widget_get_toplevel(
	                                      p->win)),
	                                  GTK_DIALOG_MODAL, "Забрать в Frame",
	                                  GTK_RESPONSE_ACCEPT, "Отмена",
	                                  GTK_RESPONSE_CANCEL, NULL);
	box = gtk_dialog_get_content_area(GTK_DIALOG(dlg));
	gtk_box_pack_start(GTK_BOX(box),
	                   gtk_label_new("Запущенный апплет (main_daemon):"),
	                   FALSE, FALSE, 6);
	combo = gtk_combo_box_text_new();
	names = xs_core_list_running_daemon_instances(&n);
	for (i = 0; i < n; i++)
		gtk_combo_box_text_append_text(GTK_COMBO_BOX_TEXT(combo),
		                               names[i]);
	gtk_combo_box_set_active(GTK_COMBO_BOX(combo),
	                         n > 0 ? 0 : -1);
	g_object_set_data(G_OBJECT(dlg), "xs-combo", combo);
	gtk_box_pack_start(GTK_BOX(box), combo, FALSE, FALSE, 6);
	gtk_widget_show_all(dlg);
	g_signal_connect(dlg, "response",
	                 G_CALLBACK(fl_add_running_response), p);
}

/* контекст таблицы гостей (для обновления галочек из коллбеков) */
static GtkListStore *fl_guests_store = NULL;
static XsPlugin *fl_guests_plugin = NULL;

/* Галочка гостя: FALSE → остановить гостя (guests_N_off=1),
 * TRUE → запустить заново (ключ снять). */
static void fl_guest_toggled(GtkCellRendererToggle *cell, gchar *path,
                             gpointer data)
{
	XsPlugin *p = data;
	PrivData *priv;
	GtkTreeIter it;
	int idx;
	char *offkey, *cur, *gkey, *gname;

	(void)cell;
	if (!p || !p->priv || !fl_guests_store)
		return;
	priv = p->priv;
	idx = atoi(path); /* 0-based; конфиг: guests_<idx+1> */
	if (idx < 0 || idx >= priv->guest_count)
		return;
	offkey = g_strdup_printf("guests_%d_off", idx + 1);
	cur = xs_host_api()->conf_str(priv->kf, p->name, offkey, "");
	g_free(offkey);
	{
		gboolean now_off = (cur && cur[0]);

		g_free(cur);
		gkey = g_strdup_printf("guests_%d", idx + 1);
		gname = xs_host_api()->conf_str(priv->kf, p->name, gkey,
		                                "");
		g_free(gkey);
		if (now_off) {
			/* включить: снять off, запустить гостя */
			char *offkey2 = g_strdup_printf("guests_%d_off",
			                                idx + 1);

			xs_host_api()->conf_set_str(priv->kf, p->name,
			                            offkey2, "");
			g_free(offkey2);
			xs_core_plugin_conf_flush(p->name);
			if (gname[0])
				xs_host_api()->start_guest(p, gname);
		} else {
			/* выключить: off=1, остановить гостя */
			char *offkey2 = g_strdup_printf("guests_%d_off",
			                                idx + 1);

			xs_host_api()->conf_set_str(priv->kf, p->name,
			                            offkey2, "1");
			g_free(offkey2);
			xs_core_plugin_conf_flush(p->name);
			if (gname[0])
				xs_host_api()->stop_guest(p, gname);
		}
		g_free(gname);
	}
	/* обновить галочку в модели (toggled не меняет active сам) */
	if (gtk_tree_model_get_iter_from_string(
	        GTK_TREE_MODEL(fl_guests_store), &it, path)) {
		gboolean newval;

		offkey = g_strdup_printf("guests_%d_off", idx + 1);
		cur = xs_host_api()->conf_str(priv->kf, p->name, offkey,
		                              "");
		newval = !(cur && cur[0]);
		g_free(cur);
		g_free(offkey);
		gtk_list_store_set(fl_guests_store, &it, 0, newval, -1);
	}
}

/* контекст рестарта: имя гостя + хост */
typedef struct {
	char *name;
	XsPlugin *host;
} FlRestart;

static gboolean fl_restart_start_cb(gpointer data)
{
	FlRestart *r = data;

	if (r->host && r->host->priv && r->name && r->name[0])
		xs_host_api()->start_guest(r->host, r->name);
	g_free(r->name);
	g_free(r);
	return G_SOURCE_REMOVE;
}

/* Клик по кнопке рестарта (третий столбец): остановить и через 350 мс
 * запустить гостя заново. */
static gboolean fl_guest_restart_clicked(GtkWidget *tree,
                                         GdkEventButton *ev,
                                         gpointer data)
{
	XsPlugin *p = data;
	PrivData *priv;
	GtkTreePath *tp;
	GtkTreeIter it;
	GtkTreeModel *model;
	gchar *name = NULL;
	gint col = -1;
	gboolean is_on_button = FALSE;
	FlRestart *r;

	if (!p || !p->priv || ev->type != GDK_BUTTON_PRESS)
		return FALSE;
	xs_host_api()->log("fl restart-click: btn=%u at %.0f,%.0f",
	                   ev->button, ev->x, ev->y);
	if (ev->button != 1)
		return FALSE;
	priv = p->priv;
	gtk_tree_view_get_path_at_pos(GTK_TREE_VIEW(tree),
	                              (gint)ev->x, (gint)ev->y, &tp, NULL,
	                              &col, NULL);
	if (!tp)
		return FALSE;
	/* Определяем КОЛОНКУ по X: col из get_path_at_pos — это offset
	 * ВНУТРИ колонки, не её номер. Кнопка = 3-я колонка (индекс 2),
	 * её ширина 24px прижата вправо. */
	{
		gint wx = (gint)ev->x;
		GtkTreeViewColumn *c_btn = gtk_tree_view_get_column(
		    GTK_TREE_VIEW(tree), 2);
		gint btn_x_start = 0;
		GList *cols, *l;
		gint btn_width = 24;

		cols = gtk_tree_view_get_columns(GTK_TREE_VIEW(tree));
		for (l = cols; l; l = l->next) {
			GtkTreeViewColumn *c = l->data;

			if (c == c_btn)
				break;
			btn_x_start += gtk_tree_view_column_get_width(c);
		}
		g_list_free(cols);
		if (c_btn)
			btn_width = gtk_tree_view_column_get_width(c_btn);
		is_on_button = (wx >= btn_x_start && wx < btn_x_start + btn_width);
	}
	model = gtk_tree_view_get_model(GTK_TREE_VIEW(tree));
	if (!gtk_tree_model_get_iter(model, &it, tp)) {
		gtk_tree_path_free(tp);
		return FALSE;
	}
	gtk_tree_model_get(model, &it, 2, &name, -1);
	gtk_tree_path_free(tp);
	if (!name || !name[0]) {
		g_free(name);
		return FALSE;
	}
	if (!is_on_button) {
		xs_host_api()->log("fl restart-click: мимо кнопки (x-offset=%d)",
		                   col);
		g_free(name);
		return FALSE;
	}
	xs_host_api()->log("frame_launcher %s: restart guest '%s'",
	                   p->name, name);
	xs_host_api()->stop_guest(p, name);
	r = g_new0(FlRestart, 1);
	r->name = g_strdup(name);
	r->host = p;
	g_timeout_add(1000, fl_restart_start_cb, r);
	g_free(name);
	return TRUE;
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

	/* Кнопки добавления гостей */
	{
		GtkWidget *hb = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 6);
		GtkWidget *b1 = gtk_button_new_with_label("Add Applet");
		GtkWidget *b2 = gtk_button_new_with_label("Add Running");

		g_signal_connect(b1, "clicked",
		                 G_CALLBACK(fl_add_applet_clicked), p);
		g_signal_connect(b2, "clicked",
		                 G_CALLBACK(fl_add_running_clicked), p);
		gtk_widget_set_tooltip_text(
		    b1, "Создать НОВЫЙ апплет выбранного типа сразу внутри "
		        "рамки (в автостарт демона не попадает)");
		gtk_widget_set_tooltip_text(
		    b2, "Забрать уже запущенный апплет: он остановится в "
		        "демоне, будет перенесён в рамку и убран из "
		        "автостарта");
		gtk_box_pack_start(GTK_BOX(hb), b1, TRUE, TRUE, 0);
		gtk_box_pack_start(GTK_BOX(hb), b2, TRUE, TRUE, 0);
		gtk_box_pack_start(GTK_BOX(page), hb, FALSE, FALSE, 4);
	}

	/* Гости: таблица [checkbox | имя | restart] */
	{
		GtkWidget *sw = gtk_scrolled_window_new(NULL, NULL);
		GtkListStore *store = gtk_list_store_new(
		    3, G_TYPE_BOOLEAN, G_TYPE_STRING, G_TYPE_STRING);
		GtkWidget *tree;
		GtkCellRenderer *chk, *txt, *btn;
		GtkTreeViewColumn *c0, *c1, *c2;
		GtkWidget *hb;

		gtk_scrolled_window_set_policy(
		    GTK_SCROLLED_WINDOW(sw), GTK_POLICY_NEVER,
		    GTK_POLICY_AUTOMATIC);
		gtk_widget_set_size_request(sw, -1, 220);
		tree = gtk_tree_view_new_with_model(
		    GTK_TREE_MODEL(store));
		gtk_tree_view_set_headers_visible(GTK_TREE_VIEW(tree), FALSE);

		chk = gtk_cell_renderer_toggle_new();
		c0 = gtk_tree_view_column_new_with_attributes(
		    NULL, chk, "active", 0, NULL);
		gtk_tree_view_column_set_sizing(c0,
		                                GTK_TREE_VIEW_COLUMN_FIXED);
		gtk_tree_view_column_set_fixed_width(c0, 28);
		g_object_set(chk, "activatable", TRUE, NULL);
		gtk_tree_view_append_column(GTK_TREE_VIEW(tree), c0);

		txt = gtk_cell_renderer_text_new();
		c1 = gtk_tree_view_column_new_with_attributes(
		    NULL, txt, "text", 1, NULL);
		gtk_tree_view_column_set_expand(c1, TRUE);
		gtk_tree_view_append_column(GTK_TREE_VIEW(tree), c1);

		btn = gtk_cell_renderer_pixbuf_new();
		{
			GdkPixbuf *ico = gtk_icon_theme_load_icon(
			    gtk_icon_theme_get_default(),
			    "view-refresh", 16,
			    GTK_ICON_LOOKUP_USE_BUILTIN, NULL);

			g_object_set(btn, "pixbuf", ico, "mode",
			             GTK_CELL_RENDERER_MODE_ACTIVATABLE,
			             NULL);
			if (ico)
				g_object_unref(ico);
		}
		c2 = gtk_tree_view_column_new_with_attributes(
		    NULL, btn, NULL);
		gtk_tree_view_column_set_sizing(c2,
		                                GTK_TREE_VIEW_COLUMN_FIXED);
		gtk_tree_view_column_set_fixed_width(c2, 24);
		gtk_tree_view_append_column(GTK_TREE_VIEW(tree), c2);

		for (int i = 0; i < priv->guest_count; i++) {
			GtkTreeIter it;
			gboolean enabled = TRUE;
			char *key = g_strdup_printf("guests_%d_off", i + 1);
			char *v;

			v = xs_host_api()->conf_str(priv->kf, p->name, key,
			                            "");
			g_free(key);
			if (v && v[0])
				enabled = FALSE;
			g_free(v);
			gtk_list_store_append(store, &it);
			gtk_list_store_set(store, &it, 0, enabled,
			                   1, priv->guests[i],
			                   2, priv->guests[i], -1);
		}
		gtk_container_add(GTK_CONTAINER(sw), tree);
		fl_guests_store = store;
		fl_guests_plugin = p;

		/* toggled: guests_N_off=1 → гостя остановить; =0 → запустить */
		g_signal_connect(chk, "toggled",
		                 G_CALLBACK(fl_guest_toggled), p);
		g_signal_connect(tree, "button-press-event",
		                 G_CALLBACK(fl_guest_restart_clicked), p);

		hb = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 6);
		(void)hb;
		xs_prop_add_row(GTK_BOX(page), "Guests",
		                "Гости рамки: галочка = запущен; кнопка "
		                "справа — рестарт гостя", sw);
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
