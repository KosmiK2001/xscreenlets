/* calendar.c — Xscreenlets плагин «calendar» (замена ClearCalendar v0.4).
 * Сетка месяца с локализованными днями, подсветка today, события из .ics,
 * листание месяцев колесом мыши. Рендер — PangoCairo на фоне темы. */
#include <gtk/gtk.h>
#include <glib.h>
#include <pango/pangocairo.h>
#include "xs_api.h"
#include "common.h"

#include <math.h>
#include <string.h>
#include <stdio.h>
#include <time.h>
#include <langinfo.h>
#include <locale.h>
#include <glib/gstdio.h>
#include <sys/stat.h>

#include "../core/i18n.h"
#define CAL_W 204   /* 102*2, как в оригинале */
#define CAL_H 210   /* 105*2 */
#define XS_PI_C 3.14159265358979323846

typedef struct {
	gdouble scale;
	char *theme;
	GKeyFile *kf;
	int x, y;
	/* iCalendar-группа */
	int first_day;          /* 0..6: день недели в левой колонке */
	char *first_weekday;    /* локализованное имя (ключ конфига) */
	char *icalpath;
	gboolean showevents;
	gdouble font_color[4];
	gdouble today_color[4];
	gdouble event_color[4];
	gdouble today_event_color[4];
	gdouble background_color[4];
	char *header_font;      /* «Месяц Год» */
	char *daynames_font;    /* строка дней недели */
	char *days_font;        /* числа дней */
	gdouble opacity;        /* прозрачность (0.1..1.0) */
	/* состояние просмотра */
	int month_shift;        /* 0 = текущий месяц */
	int cur_rows;           /* строк сетки видимого месяца (авто-высота) */
	/* события */
	GPtrArray *events;
} PrivData;

typedef struct {
	int year, month, day;   /* month: 1..12 */
	char *summary;
} CalEvent;

static void cal_event_free(gpointer data)
{
	CalEvent *e = data;

	if (!e)
		return;
	g_free(e->summary);
	g_free(e);
}

/* «r,g,b,a» → out[4]; нет/кривая строка → def */
static void cal_read_color(GKeyFile *kf, const char *sec, const char *key,
                           const gdouble def[4], gdouble out[4])
{
	char *s = xs_host_api()->conf_str(kf, sec, key, NULL);

	out[0] = def[0]; out[1] = def[1]; out[2] = def[2]; out[3] = def[3];
	if (s && sscanf(s, "%lf,%lf,%lf,%lf", &out[0], &out[1], &out[2],
	                &out[3]) != 4) {
		out[0] = def[0]; out[1] = def[1]; out[2] = def[2]; out[3] = def[3];
	}
	g_free(s);
}

/* ---------- видимый месяц ---------- */

typedef struct {
	int year, month;        /* month: 1..12 */
	int today_y, today_m, today_d;
	int days_in_month;
	int start_col;          /* колонка дня 1 с учётом first_day (0..6) */
	char header[64];
} CalMonth;

static void cal_visible_month(const PrivData *priv, CalMonth *cm)
{
	time_t t = time(NULL);
	struct tm tm_buf;
	struct tm *tm = localtime_r(&t, &tm_buf);
	int y = tm->tm_year + 1900;
	int m = tm->tm_mon + 1; /* 1..12 */
	struct tm first;
	time_t ft;
	static const int mdays[12] = {31, 28, 31, 30, 31, 30, 31, 31, 30, 31,
	                              30, 31};

	cm->today_y = y;
	cm->today_m = m;
	cm->today_d = tm->tm_mday;

	m += priv->month_shift;
	while (m > 12) { m -= 12; y++; }
	while (m < 1)  { m += 12; y--; }
	cm->year = y;
	cm->month = m;

	cm->days_in_month = mdays[m - 1];
	if (m == 2 && ((y % 4 == 0 && y % 100 != 0) || y % 400 == 0))
		cm->days_in_month = 29;

	/* день недели 1-го числа: 0=вс (tm_wday) → колонка с first_day */
	memset(&first, 0, sizeof(first));
	first.tm_year = y - 1900;
	first.tm_mon = m - 1;
	first.tm_mday = 1;
	ft = mktime(&first);
	localtime_r(&ft, &first);
	cm->start_col = (first.tm_wday + 7 - priv->first_day) % 7;

	strftime(cm->header, sizeof(cm->header), "%B %Y", &first);
}

static gboolean cal_is_today(const CalMonth *cm, const PrivData *priv,
                             int day)
{
	return priv->month_shift == 0 && day == cm->today_d &&
	       cm->year == cm->today_y && cm->month == cm->today_m;
}

/* Есть ли событие в этот день (с учётом простых RRULE) */
static gboolean cal_day_has_event(const PrivData *priv, const CalMonth *cm,
                                  int day)
{
	gsize i;

	if (!priv->events || !priv->showevents)
		return FALSE;
	for (i = 0; i < priv->events->len; i++) {
		const CalEvent *e = g_ptr_array_index(priv->events, i);

		if (e->month != cm->month)
			continue;
		if (e->year == cm->year && e->day == day)
			return TRUE;
		/* RRULE:FREQ=YEARLY — тот же день/месяц любого года */
		if (e->day == day && e->summary &&
		    e->summary[0] == '@') /* YEARLY помечен '@' префиксом */
			return TRUE;
	}
	return FALSE;
}

/* Список событий видимого месяца (для View events) */
static GString *cal_events_text(const PrivData *priv, const CalMonth *cm)
{
	GString *s = g_string_new("");
	gsize i;

	if (!priv->events)
		return s;
	for (i = 0; i < priv->events->len; i++) {
		const CalEvent *e = g_ptr_array_index(priv->events, i);
		gboolean yearly = e->summary && e->summary[0] == '@';

		if (e->month != cm->month)
			continue;
		if (e->year == cm->year || yearly) {
			if (s->len)
				g_string_append(s, "\n");
			g_string_append_printf(s, "%04d.%02d.%02d — %s",
			                       e->year, e->month, e->day,
			                       yearly ? e->summary + 1 : e->summary);
		}
	}
	if (s->len == 0)
		g_string_append(s, "No events this month");
	return s;
}

/* ---------- .ics-парсер (локальные файлы) ---------- */

/* DTSTART[:;...]YYYYMMDD[THHMMSS...] → дата */
static gboolean cal_parse_dtstart(const char *line, int *y, int *m, int *d)
{
	const char *p = strchr(line, ':');
	char digits[9];
	size_t n;

	if (!p)
		return FALSE;
	p++;
	n = strspn(p, "0123456789");
	if (n < 8)
		return FALSE;
	memcpy(digits, p, 8);
	digits[8] = '\0';
	*y = (digits[0] - '0') * 1000 + (digits[1] - '0') * 100 +
	     (digits[2] - '0') * 10 + (digits[3] - '0');
	*m = (digits[4] - '0') * 10 + (digits[5] - '0');
	*d = (digits[6] - '0') * 10 + (digits[7] - '0');
	return *m >= 1 && *m <= 12 && *d >= 1 && *d <= 31;
}

static void cal_ics_load(PrivData *priv)
{
	char *buf;
	gsize len;
	char **lines;
	gsize i, n;
	gboolean in_event = FALSE;
	int cur_y = 0, cur_m = 0, cur_d = 0;
	gboolean cur_ok = FALSE;
	gboolean cur_yearly = FALSE;
	char *cur_summary = NULL;
	GPtrArray *events;

	if (priv->events) {
		g_ptr_array_free(priv->events, TRUE);
		priv->events = NULL;
	}
	if (!priv->icalpath || !priv->icalpath[0]) {
		xs_host_api()->log("calendar: no icalpath set");
		return;
	}
	if (g_str_has_prefix(priv->icalpath, "http")) {
		xs_host_api()->log("calendar: http ics not supported "
		                   "(local file only): %s", priv->icalpath);
		return;
	}
	if (!g_file_get_contents(priv->icalpath, &buf, &len, NULL)) {
		xs_host_api()->log("calendar: cannot read %s", priv->icalpath);
		return;
	}
	lines = g_strsplit_set(buf, "\r\n", 0);
	g_free(buf);
	events = g_ptr_array_new_with_free_func(cal_event_free);
	n = g_strv_length(lines);
	for (i = 0; i < n; i++) {
		const char *l = lines[i];

		if (strncmp(l, "BEGIN:VEVENT", 12) == 0) {
			in_event = TRUE;
			cur_ok = FALSE;
			cur_yearly = FALSE;
			cur_y = cur_m = cur_d = 0;
			g_free(cur_summary);
			cur_summary = NULL;
		} else if (in_event && strncmp(l, "END:VEVENT", 10) == 0) {
			in_event = FALSE;
			if (cur_ok) {
				CalEvent *e = g_new0(CalEvent, 1);
				e->year = cur_y;
				e->month = cur_m;
				e->day = cur_d;
				e->summary = cur_summary
				    ? cur_summary : g_strdup("");
				if (cur_yearly)
					memmove(e->summary + 1, e->summary,
					        strlen(e->summary) + 1),
					e->summary[0] = '@';
				g_ptr_array_add(events, e);
			} else {
				g_free(cur_summary);
			}
			cur_summary = NULL;
		} else if (in_event) {
			int y, m, d;

			if (strncmp(l, "SUMMARY:", 8) == 0) {
				g_free(cur_summary);
				cur_summary = g_strdup(l + 8);
				g_strstrip(cur_summary);
			} else if (strncmp(l, "DTSTART", 7) == 0 &&
			           cal_parse_dtstart(l, &y, &m, &d)) {
				cur_y = y; cur_m = m; cur_d = d;
				cur_ok = TRUE;
			} else if (strncmp(l, "RRULE:", 6) == 0 &&
			           strstr(l + 6, "FREQ=YEARLY")) {
				cur_yearly = TRUE;
			}
			/* EXDATE и сложные RRULE не разворачиваем */
		}
	}
	g_strfreev(lines);
	priv->events = events;
	if (cur_summary)
		g_free(cur_summary);
	xs_host_api()->log("calendar: loaded %u events from %s",
	                   (unsigned)events->len, priv->icalpath);
}

/* ---------- отрисовка ---------- */

static void cal_color(cairo_t *cr, const gdouble c[4])
{
	cairo_set_source_rgba(cr, c[0], c[1], c[2], c[3]);
}

static void cal_rounded_rect(cairo_t *cr, double x, double y, double r,
                             double w, double h)
{
	cairo_new_sub_path(cr);
	cairo_arc(cr, x + w - r, y + r, r, -XS_PI_C / 2, 0);
	cairo_arc(cr, x + w - r, y + h - r, r, 0, XS_PI_C / 2);
	cairo_arc(cr, x + r, y + h - r, r, XS_PI_C / 2, XS_PI_C);
	cairo_arc(cr, x + r, y + r, r, XS_PI_C, 3 * XS_PI_C / 2);
	cairo_close_path(cr);
	cairo_fill(cr);
}

static void cal_draw(XsPlugin *p, cairo_t *cr, int w, int h)
{
	PrivData *priv = p->priv;
	CalMonth cm;
	PangoLayout *layout;
	PangoFontDescription *fd;
	PangoRectangle ext;
	PangoRectangle ink;
	char num[16];
	double k;
	double base_h;
	double ox, oy;
	int col, row, day;
	int i;
	char **day_names = NULL;
	gboolean theme_bg;

	if (!priv || !p->win)
		return;
	cal_visible_month(priv, &cm);
	priv->cur_rows = (cm.start_col + cm.days_in_month - 1) / 7 + 1;
	{
		int rows = priv->cur_rows;

		if (rows < 5)
			rows = 5;
		/* Канонический кадр: CAL_W × base_h юнитов, масштаб ОДИН по обеим
		 * осям (min) — пропорции не зависят от поведения WM при resize.
		 * Кадр центрируется в окне; вне его остаётся прозрачность. */
		base_h = 25 + rows * 12 + 4;
	}

	k = w / 102.0;                /* канон по ширине = 102 юнита (контент) */
	if (h / (double)base_h < k)
		k = h / (double)base_h;
	ox = (w - 102.0 * k) / 2.0;
	oy = (h - base_h * k) / 2.0;
	cairo_save(cr);
	cairo_translate(cr, ox, oy);
	cairo_scale(cr, k, k);

	/* фон: точный порядок оригинала (on_draw): ПОДЛОЖКА background_color
	 * строго 0,1,8,100,82 (rounded, высота 82 — НЕ до низа кадра!), затем
	 * native тема поверх; зона ниже темы остаётся прозрачной — как у
	 * питона (там снизу окна тоже нет подложки). */
	cairo_set_operator(cr, CAIRO_OPERATOR_OVER);
	theme_bg = xs_core_theme_has(p, "date-bg") || xs_core_theme_has(p, "back");
	cal_color(cr, priv->background_color);
	{
		int bg_h = (int)base_h - 2;

		if (bg_h > 82)
			bg_h = 82;   /* оригинал: draw_rounded_rectangle(0,1,8,100,82) */
		cal_rounded_rect(cr, 0, 1, 8, 100, bg_h);
	}
	if (theme_bg)
		xs_host_api()->theme_draw_native(p, cr, "date-bg", 0.0, 0.0);

	layout = pango_cairo_create_layout(cr);

	/* header «Месяц Год», справа */
	fd = pango_font_description_from_string(
		priv->header_font ? priv->header_font : "Tahoma Bold 9");
	pango_layout_set_font_description(layout, fd);
	pango_font_description_free(fd);
	pango_layout_set_text(layout, cm.header, -1);
	pango_layout_get_pixel_extents(layout, NULL, &ext);
	cal_color(cr, priv->font_color);
	cairo_move_to(cr, 97.0 - ext.width, 5.0);
	pango_cairo_show_layout(cr, layout);

	/* локализованные имена дней недели */
	day_names = g_new0(char *, 7);
	for (i = 0; i < 7; i++) {
		const char *full = nl_langinfo(DAY_1 + ((priv->first_day + i) % 7));
		day_names[i] = g_strdup_printf("%s", full);
		/* первые 3 буквы (по символам UTF-8) */
		{
			char *q = day_names[i];
			int cnt = 0;
			char *r = q;
			while (*r && cnt < 3) {
				r = g_utf8_next_char(r);
				cnt++;
			}
			*r = '\0';
			(void)q;
		}
	}

	fd = pango_font_description_from_string(
		priv->daynames_font ? priv->daynames_font : "Monospace Bold 6");
	pango_layout_set_font_description(layout, fd);
	pango_font_description_free(fd);
	cal_color(cr, priv->font_color);
	for (i = 0; i < 7; i++) {
		pango_layout_set_text(layout, day_names[i], -1);
		pango_layout_get_pixel_extents(layout, NULL, &ext);
		cairo_move_to(cr, 6 + i * 13 + (13 - ext.width) / 2.0, 15.0);
		pango_cairo_show_layout(cr, layout);
	}

	/* сетка дней */
	fd = pango_font_description_from_string(
		priv->days_font ? priv->days_font : "FreeSans 7");
	pango_layout_set_font_description(layout, fd);
	pango_font_description_free(fd);
	day = 1;
	row = 0;
	col = cm.start_col;
	while (day <= cm.days_in_month && row < 6) {
		double cx = 6 + col * 13;
		double cy = 25 + row * 12;
		gboolean has_ev = cal_day_has_event(priv, &cm, day);

		if (cal_is_today(&cm, priv, day)) {
			cal_color(cr, priv->today_color);
			cal_rounded_rect(cr, cx - 1.5, cy - 1.0, 2, 11, 10);
		} else if (has_ev) {
			cal_color(cr, priv->event_color);
			cal_rounded_rect(cr, cx - 1.5, cy - 1.0, 2, 11, 10);
		}
		snprintf(num, sizeof(num), "%d", day);
		pango_layout_set_text(layout, num, -1);
		pango_layout_get_pixel_extents(layout, &ink, &ext);
		cal_color(cr, priv->font_color);
		/* Центр плашки (cx+4, cy+4): позиция = центр − ink.width/2 −
		 * ink.x/y (ink-rect — фактические границы глифов). */
		cairo_move_to(cr,
		              cx + 4.0 - ink.width / 2.0 - ink.x,
		              cy + 4.0 - ink.height / 2.0 - ink.y);
		pango_cairo_show_layout(cr, layout);

		col++;
		if (col >= 7) { col = 0; row++; }
		day++;
	}

	g_object_unref(layout);
	for (i = 0; i < 7; i++)
		g_free(day_names[i]);
	g_free(day_names);
	cairo_restore(cr);
}

/* ---------- события мыши: колесо и средняя кнопка ---------- */

/* Живой ресайз под высоту сетки видимого месяца (устраняет пустоту снизу).
 * rows по формуле сетки; высота = заголовок(25) + rows*12 + паддинг 4. */
static void cal_fit_height(XsPlugin *p, PrivData *priv)
{
	int rows = priv->cur_rows;
	int base_h;
	double scale;

	if (rows < 5)
		rows = 5;
	base_h = 25 + rows * 12 + 4;
	/* scale — единый источник истины: конфиг (Properties/Size пишут туда
	 * напрямую, priv->scale при этом не обновляется — иначе тик откатит
	 * окно к старому размеру). */
	scale = xs_host_api()->conf_dbl(priv->kf, p->name, "scale", 1.0);
	if (scale < 0.2)
		scale = 0.2;
	else if (scale > 10.0)
		scale = 10.0;
	priv->scale = scale;
	if (p->win) {
		int cur_w, cur_h;
		int want_w = (int)(102 * scale);   /* канон X = 102 юнита */
		int want_h = (int)(base_h * scale);

		gtk_window_get_size(GTK_WINDOW(p->win), &cur_w, &cur_h);
		if (cur_h != want_h || cur_w != want_w)
			xs_host_api()->resize(p, want_w, want_h);
	}
}

static gboolean calendar_button(XsPlugin *p, GdkEventButton *ev)
{
	PrivData *priv = p->priv;

	if (!priv)
		return FALSE;
	if (ev->button == 2) { /* средняя — вернуться к текущему месяцу */
		priv->month_shift = 0;
		cal_fit_height(p, priv);
		if (p->win)
			gtk_widget_queue_draw(p->win);
		return TRUE;
	}
	return FALSE;
}

static gboolean calendar_scroll(XsPlugin *p, GdkEventScroll *ev)
{
	PrivData *priv = p->priv;

	if (!priv)
		return FALSE;
	if (ev->direction == GDK_SCROLL_UP)
		priv->month_shift++;
	else if (ev->direction == GDK_SCROLL_DOWN)
		priv->month_shift--;
	else
		return FALSE;
	if (priv->month_shift > 120)
		priv->month_shift = 120;
	if (priv->month_shift < -120)
		priv->month_shift = -120;
	cal_fit_height(p, priv);
	if (p->win)
		gtk_widget_queue_draw(p->win);
	return TRUE;
}

/* ---------- меню плагина ---------- */

static void cal_menu_cmd(XsPlugin *p, const char *cmd)
{
	PrivData *priv = p->priv;
	CalMonth cm;

	if (!priv)
		return;
	if (strcmp(cmd, "back_today") == 0) {
		priv->month_shift = 0;
		cal_fit_height(p, priv);
		if (p->win)
			gtk_widget_queue_draw(p->win);
	} else if (strcmp(cmd, "scale-applied") == 0) {
		/* scale изменён (Properties/Size): календарь сам подгоняет окно
		 * под канон 102×base_h (ядро больше не хардкодит 240*x) */
		cal_fit_height(p, priv);
		if (p->win)
			gtk_widget_queue_draw(p->win);
	} else if (strcmp(cmd, "toggle_events") == 0) {
		priv->showevents = !priv->showevents;
		g_key_file_set_boolean(priv->kf, p->name, "showevents",
		                       priv->showevents);
		xs_core_plugin_conf_flush(p->name);
		if (p->win)
			gtk_widget_queue_draw(p->win);
	} else if (strcmp(cmd, "update_events") == 0) {
		cal_ics_load(priv);
		if (p->win)
			gtk_widget_queue_draw(p->win);
	} else if (strcmp(cmd, "view_events") == 0) {
		GString *s;
		GtkWidget *dlg;

		cal_visible_month(priv, &cm);
		s = cal_events_text(priv, &cm);
		dlg = gtk_message_dialog_new(p->win ? GTK_WINDOW(p->win) : NULL,
		                             GTK_DIALOG_DESTROY_WITH_PARENT,
		                             GTK_MESSAGE_INFO,
		                             GTK_BUTTONS_CLOSE, "%s",
		                             s->str);
		gtk_window_set_title(GTK_WINDOW(dlg), cm.header);
		g_signal_connect(dlg, "response",
		                 G_CALLBACK(gtk_widget_destroy), NULL);
		gtk_widget_show(dlg);
		g_string_free(s, TRUE);
	} else if (g_str_has_prefix(cmd, "theme:")) {
		g_free(priv->theme);
		priv->theme = g_strdup(cmd + 6);
		g_key_file_set_string(priv->kf, p->name, "theme", priv->theme);
		xs_core_plugin_conf_flush(p->name);
		{
			char *dir = xs_core_find_theme(
			    p->type ? p->type : p->name, priv->theme);
			if (dir)
				xs_host_api()->theme_load(p, dir);
			g_free(dir);
		}
		if (p->win)
			gtk_widget_queue_draw(p->win);
	}
}

static void cal_theme_toggled(GtkCheckMenuItem *mi, gpointer data);

static void cal_menu_item(GtkWidget *menu, XsPlugin *p, const char *label,
                          const char *cmd)
{
	GtkWidget *mi = gtk_menu_item_new_with_label(_(label));

	g_object_set_data_full(G_OBJECT(mi), "xs-cmd",
	                       g_strdup_printf("p:%s", cmd), g_free);
	g_signal_connect(mi, "activate", G_CALLBACK(xs_core_menu_activate), p);
	gtk_menu_shell_append(GTK_MENU_SHELL(menu), mi);
}

static void cal_menu_add_themes(const char *dir, GPtrArray *names)
{
	GDir *d;
	const char *fn;

	if (!dir)
		return;
	d = g_dir_open(dir, 0, NULL);
	if (!d)
		return;
	while ((fn = g_dir_read_name(d))) {
		char *path = g_build_filename(dir, fn, NULL);
		struct stat st;
		gboolean dup = FALSE;
		gsize i;

		if (stat(path, &st) != 0 || !S_ISDIR(st.st_mode) || fn[0] == '.') {
			g_free(path);
			continue;
		}
		for (i = 0; i < names->len; i++)
			if (strcmp(g_ptr_array_index(names, i), fn) == 0)
				dup = TRUE;
		if (!dup)
			g_ptr_array_add(names, g_strdup(fn));
		g_free(path);
	}
	g_dir_close(d);
}

static int cal_cmp_names(gconstpointer a, gconstpointer b)
{
	return strcmp(*(const char *const *)a, *(const char *const *)b);
}

static void cal_menu(XsPlugin *p, GtkMenu *m)
{
	PrivData *priv;
	GtkWidget *sub, *item, *mi;
	char *user_dir;
	GPtrArray *names;
	gsize i;

	if (!p || !m || !p->priv)
		return;
	priv = p->priv;

	cal_menu_item(GTK_WIDGET(m), p, "View events", "view_events");
	cal_menu_item(GTK_WIDGET(m), p, "Update events", "update_events");
	cal_menu_item(GTK_WIDGET(m), p, "Toggle events", "toggle_events");
	cal_menu_item(GTK_WIDGET(m), p, "Back to today", "back_today");
	xs_core_add_separator(GTK_WIDGET(m));

	/* Theme-подменю (как у clock) */
	user_dir = g_build_filename(g_get_user_config_dir(), "xscreenlets",
	                            "themes", p->type ? p->type : p->name, NULL);
	names = g_ptr_array_new_with_free_func(g_free);
	cal_menu_add_themes(user_dir, names);
	cal_menu_add_themes("/usr/share/screenlets/ClearCalendar/themes", names);
	g_free(user_dir);
	if (names->len > 0) {
		g_ptr_array_sort(names, cal_cmp_names);
		sub = gtk_menu_new();
		for (i = 0; i < names->len; i++) {
			const char *name = g_ptr_array_index(names, i);
			mi = gtk_check_menu_item_new_with_label(name);
			gtk_check_menu_item_set_active(GTK_CHECK_MENU_ITEM(mi),
			                               priv->theme &&
			                               strcmp(priv->theme, name) == 0);
			g_object_set_data_full(G_OBJECT(mi), "xs-theme",
			                       g_strdup(name), g_free);
			g_signal_connect(mi, "toggled",
			                 G_CALLBACK(cal_theme_toggled), p);
			gtk_menu_shell_append(GTK_MENU_SHELL(sub), mi);
		}
		item = gtk_menu_item_new_with_label(_("Theme"));
		gtk_menu_item_set_submenu(GTK_MENU_ITEM(item), sub);
		gtk_menu_shell_append(GTK_MENU_SHELL(m), item);
	}
	g_ptr_array_unref(names);
}

static void cal_theme_toggled(GtkCheckMenuItem *mi, gpointer data)
{
	XsPlugin *p = data;
	char *name;

	if (!p || !p->priv || !gtk_check_menu_item_get_active(mi))
		return;
	name = g_strdup(g_object_get_data(G_OBJECT(mi), "xs-theme"));
	cal_menu_cmd(p, name);
	g_free(name);
}

/* ---------- Properties-группа iCalendar ---------- */

static void cal_font_set(GtkFontButton *btn, gpointer data);

static void cal_bool_toggled(GtkToggleButton *btn, gpointer data)
{
	XsPlugin *p = data;
	PrivData *priv;
	const char *key = g_object_get_data(G_OBJECT(btn), "xs-key");
	gboolean active = gtk_toggle_button_get_active(btn);

	if (!p || !p->priv || !key)
		return;
	priv = p->priv;
	g_key_file_set_boolean(priv->kf, p->name, key, active);
	if (strcmp(key, "showevents") == 0)
		priv->showevents = active;
	xs_core_plugin_conf_flush(p->name);
	if (p->win)
		gtk_widget_queue_draw(p->win);
}

static void cal_entry_changed(GtkEditable *e, gpointer data)
{
	XsPlugin *p = data;
	PrivData *priv;
	const char *key = g_object_get_data(G_OBJECT(e), "xs-key");
	const char *text = gtk_entry_get_text(GTK_ENTRY(e));

	if (!p || !p->priv || !key)
		return;
	priv = p->priv;
	if (strcmp(key, "icalpath") == 0) {
		g_free(priv->icalpath);
		priv->icalpath = g_strdup(text);
		g_key_file_set_string(priv->kf, p->name, "icalpath",
		                      text ? text : "");
		xs_core_plugin_conf_flush(p->name);
		cal_ics_load(priv);
		if (p->win)
			gtk_widget_queue_draw(p->win);
	}
}

static void cal_combo_changed(GtkComboBox *cb, gpointer data)
{
	XsPlugin *p = data;
	PrivData *priv;
	char *text;

	if (!p || !p->priv)
		return;
	priv = p->priv;
	text = gtk_combo_box_text_get_active_text(GTK_COMBO_BOX_TEXT(cb));
	if (text) {
		g_free(priv->first_weekday);
		priv->first_weekday = g_strdup(text);
		g_key_file_set_string(priv->kf, p->name, "first_weekday",
		                      text);
		{
			gsize i;

			for (i = 0; i < 7; i++) {
				if (g_utf8_collate_key(text, -1) ==
				    g_utf8_collate_key(nl_langinfo(DAY_1 + i), -1))
					priv->first_day = (int)i;
			}
		}
		xs_core_plugin_conf_flush(p->name);
		if (p->win)
			gtk_widget_queue_draw(p->win);
	}
	g_free(text);
}

static void cal_color_set(GtkColorButton *btn, gpointer data)
{
	XsPlugin *p = data;
	PrivData *priv;
	const char *key = g_object_get_data(G_OBJECT(btn), "xs-key");
	GdkRGBA c;

	if (!p || !p->priv || !key)
		return;
	priv = p->priv;
	gtk_color_chooser_get_rgba(GTK_COLOR_CHOOSER(btn), &c);
	if (strcmp(key, "font_color") == 0) {
		priv->font_color[0] = c.red; priv->font_color[1] = c.green;
		priv->font_color[2] = c.blue; priv->font_color[3] = c.alpha;
	} else if (strcmp(key, "today_color") == 0) {
		priv->today_color[0] = c.red; priv->today_color[1] = c.green;
		priv->today_color[2] = c.blue; priv->today_color[3] = c.alpha;
	} else if (strcmp(key, "event_color") == 0) {
		priv->event_color[0] = c.red; priv->event_color[1] = c.green;
		priv->event_color[2] = c.blue; priv->event_color[3] = c.alpha;
	} else if (strcmp(key, "today_event_color") == 0) {
		priv->today_event_color[0] = c.red;
		priv->today_event_color[1] = c.green;
		priv->today_event_color[2] = c.blue;
		priv->today_event_color[3] = c.alpha;
	} else if (strcmp(key, "background_color") == 0) {
		priv->background_color[0] = c.red;
		priv->background_color[1] = c.green;
		priv->background_color[2] = c.blue;
		priv->background_color[3] = c.alpha;
	}
	{
		char *s = g_strdup_printf("%g,%g,%g,%g", c.red, c.green, c.blue,
		                          c.alpha);
		g_key_file_set_string(priv->kf, p->name, key, s);
		g_free(s);
	}
	xs_core_plugin_conf_flush(p->name);
	if (p->win)
		gtk_widget_queue_draw(p->win);
}

/* Добавить строку цвета из priv->xxx_color */
static GtkWidget *cal_add_color(GtkWidget *page, XsPlugin *p,
                                const char *label, const char *desc,
                                const char *key, const gdouble c[4])
{
	GtkWidget *w = xs_prop_add_color(GTK_BOX(page), label, desc,
	                                 c[0], c[1], c[2], c[3]);

	g_object_set_data_full(G_OBJECT(w), "xs-key", g_strdup(key), g_free);
	g_signal_connect(w, "color-set", G_CALLBACK(cal_color_set), p);
	return w;
}

static void cal_properties(XsPlugin *p, GtkNotebook *nb)
{
	PrivData *priv;
	GtkWidget *page;
	GtkWidget *w;
	GtkWidget *page_box;
	char *choices[8];
	int i;

	if (!p || !p->priv)
		return;
	priv = p->priv;

	page = gtk_box_new(GTK_ORIENTATION_VERTICAL, 3);
	gtk_container_set_border_width(GTK_CONTAINER(page), 10);
	page_box = page;
	{
		GtkWidget *lbl = gtk_label_new(_("Calendar specific options"));
		gtk_widget_set_halign(lbl, GTK_ALIGN_START);
		gtk_box_pack_start(GTK_BOX(page_box), lbl, FALSE, FALSE, 7);
		gtk_box_pack_start(GTK_BOX(page_box),
		                   gtk_separator_new(GTK_ORIENTATION_HORIZONTAL),
		                   FALSE, FALSE, 5);
	}

	/* first_weekday: ComboBox из локализованных имён дней */
	w = gtk_combo_box_text_new();
	for (i = 0; i < 7; i++) {
		choices[i] = g_strdup(nl_langinfo(DAY_1 + i));
		gtk_combo_box_text_append_text(GTK_COMBO_BOX_TEXT(w), choices[i]);
	}
	choices[7] = NULL;
	for (i = 0; i < 7; i++) {
		if (priv->first_weekday &&
		    g_strcmp0(priv->first_weekday, choices[i]) == 0)
			gtk_combo_box_set_active(GTK_COMBO_BOX(w), i);
	}
	xs_prop_add_row(GTK_BOX(page_box), "First Weekday",
	           "The day to be shown in the leftmost column", w);
	g_signal_connect(w, "changed", G_CALLBACK(cal_combo_changed), p);

	w = xs_prop_add_string(GTK_BOX(page_box), "iCalendar ics file path",
	                       "The full path where the .ics file is located "
	                       "(local file) ...", priv->icalpath);
	g_object_set_data_full(G_OBJECT(w), "xs-key", g_strdup("icalpath"),
	                       g_free);
	g_signal_connect(w, "changed", G_CALLBACK(cal_entry_changed), p);

	w = xs_prop_add_bool(GTK_BOX(page_box), "Show iCalendar events",
	                     "Show iCalendar events", priv->showevents);
	g_object_set_data_full(G_OBJECT(w), "xs-key", g_strdup("showevents"),
	                       g_free);
	g_signal_connect(w, "toggled", G_CALLBACK(cal_bool_toggled), p);

	cal_add_color(page_box, p, "Text color", "font_color",
	              "font_color", priv->font_color);
	cal_add_color(page_box, p, "Today color", "today_color",
	              "today_color", priv->today_color);
	cal_add_color(page_box, p, "Event day color", "event_color",
	              "event_color", priv->event_color);
	cal_add_color(page_box, p, "Today event color",
	              "today_event_color", "today_event_color",
	              priv->today_event_color);
	cal_add_color(page_box, p, "Back color (default theme)",
	              "only works with default theme", "background_color",
	              priv->background_color);

	/* Шрифты (заголовок/дни недели/числа) */
	{
		GtkWidget *w;

		w = xs_prop_add_font(GTK_BOX(page_box), "Header font",
		                     "Font of the «Month Year» header",
		                     priv->header_font);
		g_object_set_data_full(G_OBJECT(w), "xs-key",
		                       g_strdup("header_font"), g_free);
		g_signal_connect(w, "font-set", G_CALLBACK(cal_font_set), p);

		w = xs_prop_add_font(GTK_BOX(page_box), "Day names font",
		                     "Font of the weekday-names row",
		                     priv->daynames_font);
		g_object_set_data_full(G_OBJECT(w), "xs-key",
		                       g_strdup("daynames_font"), g_free);
		g_signal_connect(w, "font-set", G_CALLBACK(cal_font_set), p);

		w = xs_prop_add_font(GTK_BOX(page_box), "Days font",
		                     "Font of the day numbers",
		                     priv->days_font);
		g_object_set_data_full(G_OBJECT(w), "xs-key",
		                       g_strdup("days_font"), g_free);
		g_signal_connect(w, "font-set", G_CALLBACK(cal_font_set), p);
	}

	for (i = 0; i < 7; i++)
		g_free(choices[i]);
	gtk_widget_show_all(page);
	gtk_notebook_append_page(nb, page, gtk_label_new(_("iCalendar")));
}

/* ---------- Themes-страница (4 колонки) ---------- */

static void cal_theme_conf_field(const char *dir, const char *field,
                                 char **out)
{
	GKeyFile *kf;
	char *path;
	char *val;

	*out = NULL;
	path = g_build_filename(dir, "theme.conf", NULL);
	kf = g_key_file_new();
	if (g_key_file_load_from_file(kf, path, G_KEY_FILE_NONE, NULL))
		val = g_key_file_get_string(kf, "Theme", field, NULL);
	else
		val = NULL;
	g_key_file_free(kf);
	g_free(path);
	*out = val;
}

static void cal_fill_themes(XsPlugin *p, GtkListStore *store)
{
	const char *search_dirs[2] = {NULL, "/usr/share/screenlets/ClearCalendar/themes"};
	GtkTreeIter it;
	gsize di;

	if (!p || !p->priv || !store)
		return;
	search_dirs[0] = g_build_filename(g_get_user_config_dir(), "xscreenlets",
	                                  "themes", p->type ? p->type : p->name, NULL);
	for (di = 0; di < G_N_ELEMENTS(search_dirs); di++) {
		GDir *d = g_dir_open(search_dirs[di], 0, NULL);
		const char *fn;

		if (!d)
			continue;
		while ((fn = g_dir_read_name(d))) {
			char *dir_path;
			struct stat st;
			char *info = NULL, *author = NULL, *version = NULL;

			if (fn[0] == '.')
				continue;
			dir_path = g_build_filename(search_dirs[di], fn, NULL);
			if (stat(dir_path, &st) != 0 || !S_ISDIR(st.st_mode)) {
				g_free(dir_path);
				continue;
			}
			cal_theme_conf_field(dir_path, "info", &info);
			cal_theme_conf_field(dir_path, "author", &author);
			cal_theme_conf_field(dir_path, "version", &version);
			g_free(dir_path);
			gtk_list_store_append(store, &it);
			gtk_list_store_set(store, &it, 0, fn, 1, info, 2, author,
			                   3, version, -1);
			g_free(info);
			g_free(author);
			g_free(version);
		}
		g_dir_close(d);
	}
	g_free((gpointer)search_dirs[0]);
}

/* ---------- init / tick / shutdown / desc ---------- */

static int calendar_init(XsPlugin *p, GKeyFile *kf)
{
	static const gdouble fc[4] = {1, 1, 1, 0.8};
	static const gdouble tc[4] = {1, 0, 0, 0.8};
	static const gdouble ec[4] = {0, 1, 0, 0.8};
	static const gdouble tec[4] = {0, 0, 1, 0.8};
	static const gdouble bc[4] = {0, 0, 0, 0.8};
	PrivData *priv;
	gsize i;

	priv = g_new0(PrivData, 1);
	p->priv = priv;
	priv->kf = kf;
	priv->events = g_ptr_array_new_with_free_func(cal_event_free);

	priv->scale = xs_host_api()->conf_dbl(kf, p->name, "scale", 1.0);
	if (priv->scale < 0.2)
		priv->scale = 0.2;
	else if (priv->scale > 10.0)
		priv->scale = 10.0;
	priv->theme = xs_host_api()->conf_str(kf, p->name, "theme", "default");
	priv->icalpath = xs_host_api()->conf_str(kf, p->name, "icalpath",
	                                         "/usr/share/screenlets/ClearCalendar/calendar.ics");
	priv->showevents = g_key_file_get_boolean(kf, p->name, "showevents",
	                                          NULL);
	cal_read_color(kf, p->name, "font_color", fc, priv->font_color);
	cal_read_color(kf, p->name, "today_color", tc, priv->today_color);
	cal_read_color(kf, p->name, "event_color", ec, priv->event_color);
	cal_read_color(kf, p->name, "today_event_color", tec, priv->today_event_color);
	cal_read_color(kf, p->name, "background_color", bc, priv->background_color);
	priv->header_font = xs_host_api()->conf_str(kf, p->name,
	                                            "header_font",
	                                            "Tahoma Bold 6");
	priv->daynames_font = xs_host_api()->conf_str(kf, p->name,
	                                              "daynames_font",
	                                              "Monospace Bold 4");
	priv->days_font = xs_host_api()->conf_str(kf, p->name, "days_font",
	                                          "FreeSans 6");
	priv->opacity = xs_host_api()->conf_dbl(kf, p->name, "opacity", 1.0);
	if (priv->opacity < 0.1)
		priv->opacity = 0.1;
	else if (priv->opacity > 1.0)
		priv->opacity = 1.0;

	/* first_weekday: имя дня (ru-локаль) → индекс */
	setlocale(LC_ALL, "");
	priv->first_weekday = xs_host_api()->conf_str(kf, p->name,
	                                              "first_weekday",
	                                              nl_langinfo(DAY_1 + 1));
	priv->first_day = 1; /* по умолчанию понедельник */
	for (i = 0; i < 7; i++) {
		if (g_strcmp0(priv->first_weekday, nl_langinfo(DAY_1 + i)) == 0)
			priv->first_day = (int)i;
	}

	priv->x = xs_host_api()->conf_int(kf, p->name, "x", 80);
	priv->y = xs_host_api()->conf_int(kf, p->name, "y", 80);

	p->win = xs_host_api()->make_window(p, priv->x, priv->y,
	                                    (int)(102 * priv->scale),
	                                    (int)(CAL_H * priv->scale));
	if (!p->win) {
		p->host->log("calendar: failed to create window");
		g_free(priv->theme);
		g_free(priv->icalpath);
		g_free(priv->first_weekday);
		g_ptr_array_free(priv->events, TRUE);
		g_free(priv);
		p->priv = NULL;
		return -1;
	}

	/* Прозрачность (рендером через ядро — как у clock) */
	xs_host_api()->set_opacity(p, priv->opacity);

	/* Тема: единый поиск — $XDG_CONFIG_HOME, legacy ~/.xscreenlets,
	 * системный каталог (-DXS_THEME_DIR/--themedir). Прежде здесь был
	 * жёстко зашит /usr/share/screenlets/ClearCalendar, которого в
	 * системе нет, из-за чего системные темы не находились никогда. */
	{
		char *dir = xs_core_find_theme(p->type ? p->type : p->name,
		                               priv->theme);
		if (!dir || !xs_host_api()->theme_load(p, dir))
			p->host->log("calendar: theme %s: using plain "
			             "background", priv->theme);
		g_free(dir);
	}

	cal_ics_load(priv);
	{
		CalMonth cm;

		cal_visible_month(priv, &cm);
		priv->cur_rows = (cm.start_col + cm.days_in_month - 1) / 7 + 1;
	}
	cal_fit_height(p, priv); /* высота окна по строкам месяца сразу */
	xs_host_api()->set_tick(p, 10000); /* update_interval=10 как в ориг. */
	return 0;
}

static void calendar_shutdown(XsPlugin *p)
{
	PrivData *priv = p->priv;

	if (!priv)
		return;
	if (priv->events)
		g_ptr_array_free(priv->events, TRUE);
	g_free(priv->theme);
	g_free(priv->icalpath);
	g_free(priv->first_weekday);
	g_free(priv->header_font);
	g_free(priv->daynames_font);
	g_free(priv->days_font);
	g_free(priv);
	p->priv = NULL;
}

/* ---------- коллбеки шрифтов Properties ---------- */

static void cal_font_set(GtkFontButton *btn, gpointer data)
{
	XsPlugin *p = data;
	PrivData *priv;
	const char *key = g_object_get_data(G_OBJECT(btn), "xs-key");
	const char *fname = gtk_font_chooser_get_font(GTK_FONT_CHOOSER(btn));

	if (!p || !p->priv || !key || !fname)
		return;
	priv = p->priv;
	if (strcmp(key, "header_font") == 0) {
		g_free(priv->header_font);
		priv->header_font = g_strdup(fname);
	} else if (strcmp(key, "daynames_font") == 0) {
		g_free(priv->daynames_font);
		priv->daynames_font = g_strdup(fname);
	} else if (strcmp(key, "days_font") == 0) {
		g_free(priv->days_font);
		priv->days_font = g_strdup(fname);
	}
	g_key_file_set_string(priv->kf, p->name, key, fname);
	xs_core_plugin_conf_flush(p->name);
	if (p->win)
		gtk_widget_queue_draw(p->win);
}

static guint calendar_tick(XsPlugin *p)
{
	/* Окно подтягивается к каноническому кадру (204×base_h юнитов) на
	 * каждом тике: если WM при старте/смене месяца не применил resize —
	 * через 10 с размер сойдётся, «картинка меньше окна» исчезает. */
	if (p && p->priv && p->win)
		cal_fit_height(p, p->priv);
	return 10000; /* раз в 10 с, как update_interval оригинала */
}

/* Plugin descriptor */
static XsPluginOps cal_ops = {
	.init = calendar_init,
	.draw = cal_draw,
	.tick = calendar_tick,
	.button = calendar_button,
	.motion = NULL,
	.shutdown = calendar_shutdown,
	.menu = cal_menu,
	.menu_cmd = cal_menu_cmd,
	.properties = cal_properties,
	.fill_themes = cal_fill_themes,
	.scroll = calendar_scroll
};

static XsPluginDesc cal_desc = {
	.name = "calendar",
	.api_version = XS_API_VERSION,
	.ops = &cal_ops,
	.desc = "A simple multilingual iCalendar Screenlet with month "
	        "preview, you can scroll through other months too and view "
	        "monthly events.",
	.author = "kosmik2001 <kosmik2001@gmail.com>"
	          "robgig1088",
	.version = "0.4"
};

XsPluginDesc *xs_plugin_desc(void)
{
	return &cal_desc;
}

