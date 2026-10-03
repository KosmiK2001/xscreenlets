#include <gtk/gtk.h>
#include <glib.h>
#include <pango/pangocairo.h>
#include "xs_api.h"
#include "common.h"
#include <math.h>
#include <string.h>
#include <time.h>
#include <glib.h>
#include <stdio.h>
#include <dirent.h>
#include <sys/stat.h>

#define XS_PI 3.141592653589793238462643383279502884
#include <cairo.h>
#include <librsvg/rsvg.h>

#include "../core/i18n.h"
typedef struct {
	gdouble scale;
	gdouble opacity;
	gboolean show_seconds;
	gboolean h24;
	char *theme;
	int x;
	int y;
	GKeyFile *kf; /* конфиг плагина, сохранён при init */
	/* группа Clock */
	char *timezone;        /* "" = локальное время */
	gdouble time_offset;   /* часы, -12..+12 */
	/* группа Face */
	char *face_text;
	char *face_text_font;
	gdouble face_color[4]; /* r,g,b,a 0..1 */
	int face_text_x;
	int face_text_y;
	gboolean show_date;
	char *date_format;
	/* группа Alarm */
	gboolean alarm_activated;
	int alarm_h, alarm_m, alarm_s;
	int alarm_length;      /* миганий до авто-стопа */
	gboolean run_command;
	char *alarm_command;
	gboolean alarm_fired;  /* будильник уже сработал (раз в сутки) */
	int alarm_until_s;     /* конец мигания (сек от полуночи) */
	gboolean alarm_fired_day; /* день последнего срабатывания (tm_yday) */
	cairo_surface_t *base_cache;
	cairo_surface_t *overlay_cache;
	int cache_width;
	int cache_height;
} PrivData;

/* Читает булево значение в формате строк; "true"/"1"/"yes" = TRUE */
static gboolean clock_conf_bool(GKeyFile *kf, const char *sec,
                                const char *key, gboolean def)
{
	char *s = xs_host_api()->conf_str(kf, sec, key,
	                                  def ? "true" : "false");
	gboolean v = g_ascii_strcasecmp(s, "true") == 0 ||
	             strcmp(s, "1") == 0 ||
	             g_ascii_strcasecmp(s, "yes") == 0;
	g_free(s);
	return v;
}

/*
 * Read theme.conf (GKeyFile, section [clock]) from the theme directory.
 * The geometry values are accepted but intentionally unused by this plugin —
 * they are only validated so that a malformed file does not break theme loading.
 */
static void clock_read_geometry(GKeyFile *kf, const char *section)
{
	(void)g_key_file_get_integer(kf, section, "x", NULL);
	(void)g_key_file_get_integer(kf, section, "y", NULL);
	(void)g_key_file_get_integer(kf, section, "width", NULL);
	(void)g_key_file_get_integer(kf, section, "height", NULL);
}

static void clock_read_theme_conf(const char *dir)
{
	GKeyFile *kf;
	GError *error = NULL;
	char *path;

	if (!dir)
		return;
	path = g_build_filename(dir, "theme.conf", NULL);
	kf = g_key_file_new();
	if (!g_key_file_load_from_file(kf, path, G_KEY_FILE_NONE, &error)) {
		if (error &&
		    !(error->domain == G_FILE_ERROR &&
		      error->code == G_FILE_ERROR_NOENT)) {
			xs_host_api()->log("clock: theme.conf %s: %s", path, error->message);
		}
		g_error_free(error);
		g_key_file_free(kf);
		g_free(path);
		return;
	}
	/* Geometry is derived from the plugin config (x/y/scale) and the SVG
	 * elements themselves; theme.conf geometry values are intentionally unused. */
	clock_read_geometry(kf, "clock");
	clock_read_geometry(kf, "geometry");
	g_key_file_free(kf);
	g_free(path);
}

static int clock_init(XsPlugin *p, GKeyFile *kf)
{
	PrivData *priv = g_new0(PrivData, 1);
	p->priv = priv;
	priv->kf = kf; /* сохраняем указатель на kf (живёт весь цикл плагина) */

	/* Read config */
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
	{
		char *show_seconds = xs_host_api()->conf_str(kf, p->name, "show_seconds", "true");
		priv->show_seconds = g_ascii_strcasecmp(show_seconds, "false") != 0 &&
		                     g_ascii_strcasecmp(show_seconds, "0") != 0 &&
		                     g_ascii_strcasecmp(show_seconds, "no") != 0;
		g_free(show_seconds);
	}
	priv->theme = xs_host_api()->conf_str(kf, p->name, "theme", "cairo-clock");

	/* Миграция старых ключей (до v3 меню-версии): h24 → hour_format,
	 * show_seconds → show_seconds_hand */
	if (g_key_file_has_key(kf, p->name, "h24", NULL) &&
	    !g_key_file_has_key(kf, p->name, "hour_format", NULL)) {
		gboolean old = clock_conf_bool(kf, p->name, "h24", FALSE);
		g_key_file_set_string(kf, p->name, "hour_format", old ? "24" : "12");
		g_key_file_remove_key(kf, p->name, "h24", NULL);
	}
	if (g_key_file_has_key(kf, p->name, "show_seconds", NULL) &&
	    !g_key_file_has_key(kf, p->name, "show_seconds_hand", NULL)) {
		gboolean old = clock_conf_bool(kf, p->name, "show_seconds", TRUE);
		g_key_file_set_boolean(kf, p->name, "show_seconds_hand", old);
		g_key_file_remove_key(kf, p->name, "show_seconds", NULL);
	}

	{
		char *hf = xs_host_api()->conf_str(kf, p->name, "hour_format", "12");
		priv->h24 = strcmp(hf, "24") == 0;
		g_free(hf);
	}
	priv->show_seconds = clock_conf_bool(kf, p->name, "show_seconds_hand", TRUE);

	/* группа Clock */
	priv->timezone = xs_host_api()->conf_str(kf, p->name, "timezone", "");
	priv->time_offset = xs_host_api()->conf_dbl(kf, p->name, "time_offset", 0.0);
	if (priv->time_offset < -12.0)
		priv->time_offset = -12.0;
	else if (priv->time_offset > 12.0)
		priv->time_offset = 12.0;

	/* группа Face */
	priv->face_text = xs_host_api()->conf_str(kf, p->name, "face_text", "");
	priv->face_text_font = xs_host_api()->conf_str(kf, p->name,
	                                               "face_text_font",
	                                               "Sans Medium 5");
	{
		char *cs = xs_host_api()->conf_str(kf, p->name,
		                                   "face_text_color", NULL);
		if (cs && sscanf(cs, "%lf,%lf,%lf,%lf", &priv->face_color[0],
		                 &priv->face_color[1], &priv->face_color[2],
		                 &priv->face_color[3]) != 4) {
			/* кривая строка — дефолт: чёрный непрозрачный */
			priv->face_color[0] = 0.0; priv->face_color[1] = 0.0;
			priv->face_color[2] = 0.0; priv->face_color[3] = 1.0;
		} else if (!cs) {
			priv->face_color[0] = 0.0; priv->face_color[1] = 0.0;
			priv->face_color[2] = 0.0; priv->face_color[3] = 1.0;
		}
		g_free(cs);
	}
	priv->face_text_x = xs_host_api()->conf_int(kf, p->name, "face_text_x", 32);
	priv->face_text_y = xs_host_api()->conf_int(kf, p->name, "face_text_y", 59);
	priv->show_date = clock_conf_bool(kf, p->name, "show_date", FALSE);
	priv->date_format = xs_host_api()->conf_str(kf, p->name,
	                                            "date_format", "%Y.%m.%d");

	/* группа Alarm */
	priv->alarm_activated = clock_conf_bool(kf, p->name, "alarm_activated", FALSE);
	priv->alarm_h = xs_host_api()->conf_int(kf, p->name, "alarm_h", 7);
	priv->alarm_m = xs_host_api()->conf_int(kf, p->name, "alarm_m", 30);
	priv->alarm_s = xs_host_api()->conf_int(kf, p->name, "alarm_s", 0);
	priv->alarm_length = xs_host_api()->conf_int(kf, p->name,
	                                             "alarm_length", 500);
	priv->run_command = clock_conf_bool(kf, p->name, "run_command", FALSE);
	priv->alarm_command = xs_host_api()->conf_str(kf, p->name,
	                                              "alarm_command", "firefox");

	priv->x = xs_host_api()->conf_int(kf, p->name, "x", 80);
	priv->y = xs_host_api()->conf_int(kf, p->name, "y", 80);

	/* Create window */
	p->win = xs_host_api()->make_window(p, priv->x, priv->y,
	                                    (int)(240 * priv->scale),
	                                    (int)(240 * priv->scale));
	if (!p->win) {
		p->host->log("clock: failed to create window");
		g_free(priv->theme);
		g_free(priv);
		p->priv = NULL;
		return -1;
	}

	/* Apply opacity */
	xs_host_api()->set_opacity(p, priv->opacity);

	/* Загрузка темы: единый поиск с приоритетом пользователь -> система.
	 * Раньше здесь стоял собственный обход XDG, ~/.xscreenlets и
	 * /usr/share/screenlets/Clock - последнего каталога в системе нет,
	 * и системный каталог пакета не поддерживался вовсе. Теперь
	 * системный путь задаётся -DXS_THEME_DIR при сборке или --themedir. */
	char *theme_dir = xs_core_find_theme(p->type ? p->type : p->name,
	                                     priv->theme);
	gboolean loaded = theme_dir && xs_host_api()->theme_load(p, theme_dir);
	if (loaded)
		clock_read_theme_conf(theme_dir);
	g_free(theme_dir);

	if (xs_core_is_debug()) {
		/* Те же два места, что и в меню: пользовательские, затем
		 * системные темы пакета. Каталога screenlets (python2) здесь
		 * больше нет - перечислять его было вводящим в заблуждение:
		 * список показывал темы, которых в системе нет. */
		const char *td = xs_core_themedir();
		char *user_dir_dbg = g_build_filename(g_get_user_config_dir(),
			"xscreenlets", "themes", p->type ? p->type : p->name, NULL);
		char *sys_dir_dbg = td ? g_build_filename(td, p->type ? p->type : p->name, NULL) : NULL;
		const char *search_dirs[] = { user_dir_dbg, sys_dir_dbg };
		g_printerr("clock: available themes:\n");
		for (gsize i = 0; i < G_N_ELEMENTS(search_dirs); i++) {
			const char *dir_path = search_dirs[i];
			if (dir_path[0] == '\0') continue;
			DIR *dir = opendir(dir_path);
			if (!dir) continue;
			struct dirent *entry;
			while ((entry = readdir(dir)) != NULL) {
				char *entry_path = g_build_filename(dir_path, entry->d_name, NULL);
				struct stat st;
				if (entry->d_name[0] == '.') {
					g_free(entry_path);
					continue;
				}
				if (stat(entry_path, &st) == 0 && S_ISDIR(st.st_mode)) {
					g_printerr("clock: theme %s\n", entry->d_name);
				}
				g_free(entry_path);
			}
			closedir(dir);
		}
		g_free(user_dir_dbg);
		g_free(sys_dir_dbg);
	}

	if (!loaded) {
		p->host->log("clock: failed to load theme %s", priv->theme);
		gtk_widget_destroy(p->win);
		p->win = NULL;
		g_free(priv->theme);
		g_free(priv);
		p->priv = NULL;
		return -1;
	}

	/* Set tick interval */
	xs_host_api()->set_tick(p, 1000);
	return 0;
}

static void clock_render_cache(int w, int h, cairo_surface_t *old,
                               cairo_surface_t **surface_out,
                               cairo_t **cr_out)
{
	cairo_surface_t *surface;
	cairo_t *cr;

	if (old)
		cairo_surface_destroy(old);
	surface = cairo_image_surface_create(CAIRO_FORMAT_ARGB32, w, h);
	cr = cairo_create(surface);
	cairo_set_operator(cr, CAIRO_OPERATOR_CLEAR);
	cairo_paint(cr);
	cairo_set_operator(cr, CAIRO_OPERATOR_OVER);
	*surface_out = surface;
	*cr_out = cr;
}

static void clock_draw(XsPlugin *p, cairo_t *cr, int w, int h)
{
	PrivData *priv = p->priv;
	time_t t;
	struct tm tm_buf;
	struct tm *tm;

	if (!priv || !p->win)
		return;
	/* Clear already done by core, we just draw */
	if (w >= 16 && w <= 2048 && h >= 16 && h <= 2048) {
		cairo_t *cache_cr;

		if (!priv->base_cache || priv->cache_width != w ||
		    priv->cache_height != h) {
			clock_render_cache(w, h, priv->base_cache,
			                   &priv->base_cache, &cache_cr);
			if (priv->overlay_cache)
				cairo_surface_destroy(priv->overlay_cache);
			priv->overlay_cache = NULL;
			priv->cache_width = w;
			priv->cache_height = h;
			xs_host_api()->theme_draw(p, cache_cr,
			                           "clock-drop-shadow", 0, 0, w);
			xs_host_api()->theme_draw(p, cache_cr,
			                           "clock-face-shadow", 0, 0, w);
			xs_host_api()->theme_draw(p, cache_cr,
			                           "clock-face", 0, 0, w);
			xs_host_api()->theme_draw(p, cache_cr,
			                           "clock-marks", 0, 0, w);
			cairo_destroy(cache_cr);
		}
		cairo_set_source_surface(cr, priv->base_cache, 0, 0);
		cairo_pattern_set_filter(cairo_get_source(cr),
		                         CAIRO_FILTER_GOOD);
		cairo_paint(cr);
	} else {
		xs_host_api()->theme_draw(p, cr, "clock-drop-shadow", 0, 0, w);
		xs_host_api()->theme_draw(p, cr, "clock-face-shadow", 0, 0, w);
		xs_host_api()->theme_draw(p, cr, "clock-face", 0, 0, w);
		xs_host_api()->theme_draw(p, cr, "clock-marks", 0, 0, w);
	}

	/* Время: если задан timezone — UNIX-время + смещение зоны (без DST);
	 * затем добавляем time_offset (часы). Единый путь через gmtime_r. */
	t = time(NULL);
	if (priv->timezone && priv->timezone[0]) {
		GTimeZone *tz = g_time_zone_new_identifier(priv->timezone);
		if (tz) {
			t += (time_t)(g_time_zone_get_offset(tz, 0) /
			              G_TIME_SPAN_SECOND);
			g_time_zone_unref(tz);
			tm = gmtime_r(&t, &tm_buf);
		} else {
			tm = localtime_r(&t, &tm_buf);
		}
	} else {
		tm = localtime_r(&t, &tm_buf);
	}
	tm->tm_hour = (tm->tm_hour + (int)priv->time_offset + 48) % 24;
	{
	double hours, minutes, seconds;

	if (priv->h24) {
		hours = tm->tm_hour;
		minutes = tm->tm_min;
		seconds = tm->tm_sec;
	} else {
		hours = fmod(tm->tm_hour, 12.0);
		minutes = tm->tm_min;
		seconds = tm->tm_sec;
	}

	double hour_angle = (hours + minutes / 60.0) * (XS_PI / 6.0); /* 360/12 = 30 deg per hour */
	double minute_angle = (minutes + seconds / 60.0) * (XS_PI / 30.0); /* 360/60 = 6 deg per minute */
	double second_angle = seconds * (XS_PI / 30.0);

	double k = w / 100.0;            /* масштаб темы; 100 — размер SVG темы (t) */
	gboolean blink = FALSE;

	/* Мигание во время сработавшего будильника (чётные/нечётные секунды) */
	if (priv->alarm_activated && priv->alarm_fired) {
		time_t ta = time(NULL);
		struct tm ta_buf;
		struct tm *tam = localtime_r(&ta, &ta_buf);
		int ns = tam->tm_hour * 3600 + tam->tm_min * 60 + tam->tm_sec;

		if (ns < priv->alarm_until_s)
			blink = (ns % 2) == 1;
	}

	/* Hour hand */
	cairo_save(cr);
	cairo_translate(cr, w/2.0, h/2.0);   /* ось = центр окна */
	cairo_rotate(cr, hour_angle - XS_PI/2);    /* SVG рисует стрелку вправо; -90° превращает в 12:00 */
	cairo_scale(cr, k, k);
	xs_host_api()->theme_draw(p, cr, "clock-hour-hand", 0, 0, 100.0);
	xs_host_api()->theme_draw(p, cr, "clock-hour-hand-shadow", 0, 0, 100.0);
	cairo_restore(cr);

	/* Minute hand */
	cairo_save(cr);
	cairo_translate(cr, w/2.0, h/2.0);
	cairo_rotate(cr, minute_angle - XS_PI/2);
	cairo_scale(cr, k, k);
	xs_host_api()->theme_draw(p, cr, "clock-minute-hand", 0, 0, 100.0);
	xs_host_api()->theme_draw(p, cr, "clock-minute-hand-shadow", 0, 0, 100.0);
	cairo_restore(cr);

	/* Second hand */
	if (priv->show_seconds && !blink) {
		cairo_save(cr);
		cairo_translate(cr, w/2.0, h/2.0);
		cairo_rotate(cr, second_angle - XS_PI/2);
		cairo_scale(cr, k, k);
		xs_host_api()->theme_draw(p, cr, "clock-second-hand", 0, 0, 100.0);
		xs_host_api()->theme_draw(p, cr, "clock-second-hand-shadow", 0, 0, 100.0);
		cairo_restore(cr);
	}

	/* Glass and frame */
	if (w >= 16 && w <= 2048 && h >= 16 && h <= 2048) {
		cairo_t *cache_cr;

		if (!priv->overlay_cache) {
			clock_render_cache(w, h, NULL, &priv->overlay_cache,
			                   &cache_cr);
			xs_host_api()->theme_draw(p, cache_cr,
			                           "clock-glass", 0, 0, w);
			xs_host_api()->theme_draw(p, cache_cr,
			                           "clock-frame", 0, 0, w);
			cairo_destroy(cache_cr);
		}
		cairo_set_source_surface(cr, priv->overlay_cache, 0, 0);
		cairo_pattern_set_filter(cairo_get_source(cr),
		                         CAIRO_FILTER_GOOD);
		cairo_paint(cr);
	} else {
		xs_host_api()->theme_draw(p, cr, "clock-glass", 0, 0, w);
		xs_host_api()->theme_draw(p, cr, "clock-frame", 0, 0, w);
	}

	/* Face-текст и дата (группа Face, как в оригинале) */
	if ((priv->face_text && priv->face_text[0]) || priv->show_date) {
		PangoLayout *layout = pango_cairo_create_layout(cr);
		PangoFontDescription *fd;
		PangoRectangle ext;
		char *txt = NULL;
		char buf[256];

		if (priv->show_date)
			strftime(buf, sizeof(buf),
			         priv->date_format ? priv->date_format : "%d/%m/%Y",
			         tm);
		if (priv->face_text && priv->face_text[0] && priv->show_date)
			txt = g_strdup_printf("%s\n%s", priv->face_text, buf);
		else if (priv->face_text && priv->face_text[0])
			txt = g_strdup(priv->face_text);
		else
			txt = g_strdup(buf);

		fd = pango_font_description_from_string(
			priv->face_text_font ? priv->face_text_font : "Sans 5");
		pango_layout_set_font_description(layout, fd);
		pango_font_description_free(fd);
		pango_layout_set_text(layout, txt, -1);
		pango_layout_set_alignment(layout, PANGO_ALIGN_CENTER);
		pango_layout_get_pixel_extents(layout, NULL, &ext);
		cairo_set_source_rgba(cr, priv->face_color[0], priv->face_color[1],
		                      priv->face_color[2], priv->face_color[3]);
		/* x/y в процентах от размера (как в оригинале, координаты прямоугольника текста) */
		cairo_move_to(cr,
		              (w * priv->face_text_x / 100.0) - ext.width / 2.0,
		              (h * priv->face_text_y / 100.0));
		pango_cairo_show_layout(cr, layout);
		g_object_unref(layout);
		g_free(txt);
	}
	} /* конец блока стрелок/текста */
}

static guint clock_tick(XsPlugin *p)
{
	PrivData *priv;

	if (!p || !p->win || !p->priv)
		return 0; /* плагин без окна: таймер не нужен */
	priv = p->priv;

	/* Будильник: при достижении времени — мигание alarm_length/2 секунд,
	 * однократно в сутки; команда — если run_command. Мигание = попеременная
	 * отрисовка рамки (blink) через frame skip. */
	if (priv->alarm_activated) {
		time_t t = time(NULL);
		struct tm tm_buf;
		struct tm *tm = localtime_r(&t, &tm_buf);
		int now_s = tm->tm_hour * 3600 + tm->tm_min * 60 + tm->tm_sec;
		int alarm = priv->alarm_h * 3600 + priv->alarm_m * 60 +
		            priv->alarm_s;

		/* сброс флага в новый день */
		if (priv->alarm_fired_day != tm->tm_yday) {
			priv->alarm_fired_day = tm->tm_yday;
			priv->alarm_fired = FALSE;
		}
		if (!priv->alarm_fired && now_s == alarm) {
			priv->alarm_fired = TRUE;
			priv->alarm_until_s =
				(now_s + priv->alarm_length / 2) % 86400;
			if (priv->run_command && priv->alarm_command &&
			    priv->alarm_command[0])
				g_spawn_command_line_async(priv->alarm_command,
				                           NULL);
		}
	}
	return 1000; /* keep 1 second interval */
}

static gboolean clock_button(XsPlugin *p, GdkEventButton *ev)
{
	(void)p;
	(void)ev;
	return FALSE; /* let core handle drag */
}

static gboolean clock_motion(XsPlugin *p, GdkEventMotion *ev)
{
	(void)p;
	(void)ev;
	return FALSE;
}

static void clock_shutdown(XsPlugin *p)
{
	PrivData *priv = p->priv;
	if (priv) {
		if (priv->base_cache)
			cairo_surface_destroy(priv->base_cache);
		if (priv->overlay_cache)
			cairo_surface_destroy(priv->overlay_cache);
		g_free(priv->theme);
		g_free(priv);
		p->priv = NULL;
	}
}

static void clock_menu_add_themes(const char *dir, GPtrArray *names)
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
        for (i = 0; i < names->len; i++) {
            if (strcmp(g_ptr_array_index(names, i), fn) == 0) {
                dup = TRUE;
                break;
            }
        }
        if (!dup)
            g_ptr_array_add(names, g_strdup(fn));
        g_free(path);
    }
    g_dir_close(d);
}

static int clock_cmp_names(gconstpointer a, gconstpointer b)
{
    return strcmp(*(const char *const *)a, *(const char *const *)b);
}

/* Живая смена темы без recreate: как в оригинале — theme_name присваивается и
 * тема перегружается на месте; окно и открытый диалог живут. */
static gboolean clock_apply_theme(XsPlugin *p, const char *name)
{
    PrivData *priv = p->priv;
    char *dir;
    gboolean loaded = FALSE;

    if (!priv)
        return FALSE;
    /* Единый поиск темы вместо трёх жёстко зашитых путей: XDG, legacy
     * ~/.xscreenlets и несуществующий /usr/share/screenlets/Clock. */
    dir = xs_core_find_theme("clock", name);
    loaded = dir && xs_host_api()->theme_load(p, dir);
    g_free(dir);
    if (!loaded) {
        xs_host_api()->log("clock: cannot load theme %s", name);
        return FALSE;
    }
    g_free(priv->theme);
    priv->theme = g_strdup(name);
    xs_host_api()->conf_set_str(priv->kf, p->name, "theme", priv->theme);
    xs_core_plugin_conf_flush(p->name);
    if (p->win)
        gtk_widget_queue_draw(p->win);
    return TRUE;
}

static void clock_theme_toggled(GtkCheckMenuItem *mi, gpointer data)
{
    XsPlugin *p = data;

    if (!p || !p->priv || !gtk_check_menu_item_get_active(mi))
        return;
    clock_apply_theme(p, g_object_get_data(G_OBJECT(mi), "xs-theme"));
}

/* --- коллбеки применений опций (realtime, как options_callback оригинала) --- */

static void clock_bool_toggled(GtkToggleButton *btn, gpointer data)
{
    XsPlugin *p = data;
    PrivData *priv;
    const char *key = g_object_get_data(G_OBJECT(btn), "xs-key");
    gboolean active;

    if (!p || !p->priv || !key)
        return;
    priv = p->priv;
    active = gtk_toggle_button_get_active(btn);
    g_key_file_set_boolean(priv->kf, p->name, key, active);
    if (strcmp(key, "show_seconds_hand") == 0)
        priv->show_seconds = active;
    else if (strcmp(key, "show_date") == 0)
        priv->show_date = active;
    else if (strcmp(key, "alarm_activated") == 0) {
        priv->alarm_activated = active;
        priv->alarm_fired = FALSE;
    } else if (strcmp(key, "run_command") == 0)
        priv->run_command = active;
    xs_core_plugin_conf_flush(p->name);
    if (p->win)
        gtk_widget_queue_draw(p->win);
}

static void clock_entry_changed(GtkEditable *e, gpointer data)
{
    XsPlugin *p = data;
    PrivData *priv;
    const char *key = g_object_get_data(G_OBJECT(e), "xs-key");
    const char *text;

    if (!p || !p->priv || !key)
        return;
    priv = p->priv;
    text = gtk_entry_get_text(GTK_ENTRY(e));
    if (strcmp(key, "timezone") == 0) {
        g_free(priv->timezone);
        priv->timezone = g_strdup(text);
    } else if (strcmp(key, "date_format") == 0) {
        g_free(priv->date_format);
        priv->date_format = g_strdup(text);
    } else if (strcmp(key, "face_text") == 0) {
        g_free(priv->face_text);
        priv->face_text = g_strdup(text);
    } else if (strcmp(key, "alarm_command") == 0) {
        g_free(priv->alarm_command);
        priv->alarm_command = g_strdup(text);
    }
    g_key_file_set_string(priv->kf, p->name, key, text ? text : "");
    xs_core_plugin_conf_flush(p->name);
    if (p->win)
        gtk_widget_queue_draw(p->win);
}

static void clock_combo_changed(GtkComboBox *cb, gpointer data)
{
    XsPlugin *p = data;
    PrivData *priv;
    const char *key = g_object_get_data(G_OBJECT(cb), "xs-key");
    char *text;

    if (!p || !p->priv || !key)
        return;
    priv = p->priv;
    text = gtk_combo_box_text_get_active_text(GTK_COMBO_BOX_TEXT(cb));
    if (strcmp(key, "hour_format") == 0) {
        priv->h24 = text && strcmp(text, "24") == 0;
        g_key_file_set_string(priv->kf, p->name, "hour_format",
                              text ? text : "12");
    }
    g_free(text);
    xs_core_plugin_conf_flush(p->name);
    if (p->win)
        gtk_widget_queue_draw(p->win);
}

static void clock_spin_changed(GtkSpinButton *spin, gpointer data)
{
    XsPlugin *p = data;
    PrivData *priv;
    const char *key = g_object_get_data(G_OBJECT(spin), "xs-key");
    double v = gtk_spin_button_get_value(GTK_SPIN_BUTTON(spin));

    if (!p || !p->priv || !key)
        return;
    priv = p->priv;
    if (strcmp(key, "time_offset") == 0) {
        priv->time_offset = CLAMP(v, -12.0, 12.0);
        g_key_file_set_double(priv->kf, p->name, "time_offset",
                              priv->time_offset);
    } else if (strcmp(key, "face_text_x") == 0) {
        priv->face_text_x = (int)v;
        g_key_file_set_integer(priv->kf, p->name, key, (int)v);
    } else if (strcmp(key, "face_text_y") == 0) {
        priv->face_text_y = (int)v;
        g_key_file_set_integer(priv->kf, p->name, "face_text_y", (int)v);
    } else if (strcmp(key, "alarm_length") == 0) {
        priv->alarm_length = (int)v;
        g_key_file_set_integer(priv->kf, p->name, "alarm_length", (int)v);
    } else if (strcmp(key, "alarm_h") == 0) {
        priv->alarm_h = (int)v;
        g_key_file_set_integer(priv->kf, p->name, "alarm_h", (int)v);
    } else if (strcmp(key, "alarm_m") == 0) {
        priv->alarm_m = (int)v;
        g_key_file_set_integer(priv->kf, p->name, "alarm_m", (int)v);
    } else if (strcmp(key, "alarm_s") == 0) {
        priv->alarm_s = (int)v;
        g_key_file_set_integer(priv->kf, p->name, "alarm_s", (int)v);
    }
    xs_core_plugin_conf_flush(p->name);
    if (p->win)
        gtk_widget_queue_draw(p->win);
}

static void clock_color_set(GtkColorButton *btn, gpointer data)
{
    XsPlugin *p = data;
    PrivData *priv;
    GdkRGBA c;

    if (!p || !p->priv)
        return;
    priv = p->priv;
    gtk_color_chooser_get_rgba(GTK_COLOR_CHOOSER(btn), &c);
    priv->face_color[0] = c.red;
    priv->face_color[1] = c.green;
    priv->face_color[2] = c.blue;
    priv->face_color[3] = c.alpha;
    {
        char *s = g_strdup_printf("%g,%g,%g,%g", c.red, c.green, c.blue,
                                  c.alpha);
        g_key_file_set_string(priv->kf, p->name, "face_text_color", s);
        g_free(s);
    }
    xs_core_plugin_conf_flush(p->name);
    if (p->win)
        gtk_widget_queue_draw(p->win);
}

static void clock_font_set(GtkFontButton *btn, gpointer data)
{
    XsPlugin *p = data;
    PrivData *priv;
    const char *fname;

    if (!p || !p->priv)
        return;
    priv = p->priv;
    fname = gtk_font_chooser_get_font(GTK_FONT_CHOOSER(btn));
    g_free(priv->face_text_font);
    priv->face_text_font = g_strdup(fname);
    g_key_file_set_string(priv->kf, p->name, "face_text_font", fname);
    xs_core_plugin_conf_flush(p->name);
    if (p->win)
        gtk_widget_queue_draw(p->win);
}

/* Список тем для вкладки Themes (4 колонки из theme.conf [Theme]) */
static void clock_theme_conf_field(const char *dir, const char *field,
                                   char **out)
{
    GKeyFile *kf;
    char *path;
    char *val;

    if (!dir || !field || !*out)
        return;
    *out = NULL;
    path = g_build_filename(dir, "theme.conf", NULL);
    kf = g_key_file_new();
    if (g_key_file_load_from_file(kf, path, G_KEY_FILE_NONE, NULL))
        val = g_key_file_get_string(kf, "Theme", field, NULL);
    else
        val = NULL;
    g_key_file_free(kf);
    g_free(path);
    *out = val; /* NULL, если файла/поля нет — рендер подставит "-" */
}

static void clock_fill_themes(XsPlugin *p, GtkListStore *store)
{
    /* Пользовательские темы, затем темы пакета. Каталога
     * /usr/share/screenlets (python2) здесь больше нет. */
    const char *td = xs_core_themedir();
    char *user_dir = g_build_filename(g_get_user_config_dir(), "xscreenlets",
                                      "themes", p->type ? p->type : p->name, NULL);
    char *sys_dir = td ? g_build_filename(td, p->type ? p->type : p->name, NULL) : NULL;
    char *search_dirs[2] = { user_dir, sys_dir };
    GtkTreeIter it;
    size_t di;

    if (!p || !p->priv || !store)
        return;
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
            /* 4 колонки: dir-имя + theme.conf [Theme] (info/author/version) */
            clock_theme_conf_field(dir_path, "info", &info);
            clock_theme_conf_field(dir_path, "author", &author);
            clock_theme_conf_field(dir_path, "version", &version);
            g_free(dir_path);
            gtk_list_store_append(store, &it);
            gtk_list_store_set(store, &it,
                               0, fn, 1, info, 2, author, 3, version, -1);
            g_free(info);
            g_free(author);
            g_free(version);
        }
        g_dir_close(d);
    }
    g_free(user_dir);
    g_free(sys_dir);
}

/* --- вкладка Options: три группы, как в ClockScreenlet.py --- */

typedef struct {
    const char *info;
    const char *title;
} GroupInfo;

/* страница группы: VBox с заголовком-описанием; возвращает VBox */
static GtkWidget *clock_props_group(GtkNotebook *nb, const char *title,
                                    const char *info)
{
    GtkWidget *page = gtk_box_new(GTK_ORIENTATION_VERTICAL, 3);
    GtkWidget *lbl;
    GtkWidget *sep;

    gtk_container_set_border_width(GTK_CONTAINER(page), 10);
    if (info && info[0]) {
        lbl = gtk_label_new(info);
        gtk_widget_set_halign(lbl, GTK_ALIGN_START);
        gtk_box_pack_start(GTK_BOX(page), lbl, FALSE, FALSE, 7);
        sep = gtk_separator_new(GTK_ORIENTATION_HORIZONTAL);
        gtk_box_pack_start(GTK_BOX(page), sep, FALSE, FALSE, 5);
    }
    gtk_notebook_append_page(nb, page, gtk_label_new(title));
    return page;
}

static void clock_properties(XsPlugin *p, GtkNotebook *nb)
{
    PrivData *priv;
    static const char *hour_choices[] = {"12", "24", NULL};

    if (!p || !p->priv)
        return;
    priv = p->priv;

    /* --- Clock --- */
    {
        GtkWidget *page = clock_props_group(nb, _("Clock"),
                                            _("Clock-specific settings."));
        GtkWidget *w;

        w = xs_prop_add_string(GTK_BOX(page), "Time Zone",
                               "The Time Zone to use for this screenlet",
                               priv->timezone);
        g_object_set_data_full(G_OBJECT(w), "xs-key",
                               g_strdup("timezone"), g_free);
        g_signal_connect(w, "changed", G_CALLBACK(clock_entry_changed), p);

        w = xs_prop_add_float(GTK_BOX(page), "Time-Offset",
                              "The time offset for this Clock instance. "
                              "This can be used to create Clocks for "
                              "different timezones ...",
                              priv->time_offset, -12, 12, 0.5, 1);
        g_object_set_data_full(G_OBJECT(w), "xs-key",
                               g_strdup("time_offset"), g_free);
        g_signal_connect(w, "value-changed",
                         G_CALLBACK(clock_spin_changed), p);

        w = xs_prop_add_choices(GTK_BOX(page), "Hour-Format",
                                "The hour-format (12/24) ...",
                                hour_choices, priv->h24 ? "24" : "12");
        g_object_set_data_full(G_OBJECT(w), "xs-key",
                               g_strdup("hour_format"), g_free);
        g_signal_connect(w, "changed", G_CALLBACK(clock_combo_changed), p);

        w = xs_prop_add_bool(GTK_BOX(page), "Show seconds-hand",
                             "Show/Hide the seconds-hand ...",
                             priv->show_seconds);
        g_object_set_data_full(G_OBJECT(w), "xs-key",
                               g_strdup("show_seconds_hand"), g_free);
        g_signal_connect(w, "toggled", G_CALLBACK(clock_bool_toggled), p);
        gtk_widget_show_all(page);
    }

    /* --- Alarm --- */
    {
        GtkWidget *page = clock_props_group(nb, _("Alarm"),
                                            _("Settings for the Alarm-function."));
        GtkWidget *hbox = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 3);
        GtkWidget *w;
        static const struct { const char *key; int val; int max; } parts[3] = {
            {"alarm_h", 0, 23}, {"alarm_m", 0, 59}, {"alarm_s", 0, 59}
        };
        static const char *labels[3] = {"Alarm-Time", NULL, NULL};
        size_t i;
        int vals[3] = {priv->alarm_h, priv->alarm_m, priv->alarm_s};

        w = xs_prop_add_bool(GTK_BOX(page), "Activate Alarm",
                             "Activate the alarm for this clock-instance ...",
                             priv->alarm_activated);
        g_object_set_data_full(G_OBJECT(w), "xs-key",
                               g_strdup("alarm_activated"), g_free);
        g_signal_connect(w, "toggled", G_CALLBACK(clock_bool_toggled), p);

        /* Alarm-Time: 3 SpinButton в одной строке (TimeOption) */
        w = gtk_label_new(_("Alarm-Time"));
        gtk_widget_set_halign(w, GTK_ALIGN_START);
        gtk_widget_set_size_request(w, 180, 28);
        gtk_box_pack_start(GTK_BOX(hbox), w, FALSE, TRUE, 0);
        for (i = 0; i < G_N_ELEMENTS(parts); i++) {
            GtkWidget *spin = gtk_spin_button_new_with_range(0, parts[i].max, 1);
            gtk_spin_button_set_value(GTK_SPIN_BUTTON(spin), vals[i]);
            g_object_set_data_full(G_OBJECT(spin), "xs-key",
                                   g_strdup(parts[i].key), g_free);
            g_signal_connect(spin, "value-changed",
                             G_CALLBACK(clock_spin_changed), p);
            gtk_box_pack_start(GTK_BOX(hbox), spin, FALSE, TRUE, 0);
        }
        gtk_box_pack_start(GTK_BOX(page), hbox, FALSE, TRUE, 0);

        w = xs_prop_add_int(GTK_BOX(page), "Alarm stops after",
                            "The times the clock shall blink before "
                            "auto-stopped. Divide the number by two to get "
                            "the seconds ...",
                            priv->alarm_length, 0, 5000, 1);
        g_object_set_data_full(G_OBJECT(w), "xs-key",
                               g_strdup("alarm_length"), g_free);
        g_signal_connect(w, "value-changed",
                         G_CALLBACK(clock_spin_changed), p);

        w = xs_prop_add_bool(GTK_BOX(page), "Run a command",
                             "Run a command when the alarm is activated...",
                             priv->run_command);
        g_object_set_data_full(G_OBJECT(w), "xs-key",
                               g_strdup("run_command"), g_free);
        g_signal_connect(w, "toggled", G_CALLBACK(clock_bool_toggled), p);

        w = xs_prop_add_string(GTK_BOX(page), "Alarm command",
                               "The command that should be run when the "
                               "alarm goes off...",
                               priv->alarm_command);
        g_object_set_data_full(G_OBJECT(w), "xs-key",
                               g_strdup("alarm_command"), g_free);
        g_signal_connect(w, "changed", G_CALLBACK(clock_entry_changed), p);
        (void)labels;
    }

    /* --- Face --- */
    {
        GtkWidget *page = clock_props_group(nb, _("Face"),
                                            _("Additional settings for the "
                                              "face-layout ..."));
        GtkWidget *w;

        w = xs_prop_add_string(GTK_BOX(page), "Face-Text",
                               "The text/Pango-Markup to be placed on the "
                               "clock's face ...",
                               priv->face_text);
        g_object_set_data_full(G_OBJECT(w), "xs-key",
                               g_strdup("face_text"), g_free);
        g_signal_connect(w, "changed", G_CALLBACK(clock_entry_changed), p);

        w = xs_prop_add_font(GTK_BOX(page), "Text-Font",
                             "The font of the text (when no Markup is "
                             "used) ...",
                             priv->face_text_font);
        g_signal_connect(w, "font-set", G_CALLBACK(clock_font_set), p);

        w = xs_prop_add_color(GTK_BOX(page), "Text-Color",
                              "The color of the text (when no Markup is "
                              "used) ...",
                              priv->face_color[0], priv->face_color[1],
                              priv->face_color[2], priv->face_color[3]);
        g_signal_connect(w, "color-set", G_CALLBACK(clock_color_set), p);

        w = xs_prop_add_int(GTK_BOX(page), "X-Position of Text",
                            "The X-Position of the text-rectangle's upper "
                            "left corner ...",
                            priv->face_text_x, 0, 100, 1);
        g_object_set_data_full(G_OBJECT(w), "xs-key",
                               g_strdup("face_text_x"), g_free);
        g_signal_connect(w, "value-changed",
                         G_CALLBACK(clock_spin_changed), p);

        w = xs_prop_add_int(GTK_BOX(page), "Y-Position of Text",
                            "The Y-Position of the text-rectangle's upper "
                            "left corner ...",
                            priv->face_text_y, 0, 100, 1);
        g_object_set_data_full(G_OBJECT(w), "xs-key",
                               g_strdup("face_text_y"), g_free);
        g_signal_connect(w, "value-changed",
                         G_CALLBACK(clock_spin_changed), p);

        w = xs_prop_add_bool(GTK_BOX(page), "Show today's date",
                             "Show date on the clock's face ...",
                             priv->show_date);
        g_object_set_data_full(G_OBJECT(w), "xs-key",
                               g_strdup("show_date"), g_free);
        g_signal_connect(w, "toggled", G_CALLBACK(clock_bool_toggled), p);

        w = xs_prop_add_string(GTK_BOX(page), "Date Format",
                               "Format of the date displayed by this Clock. "
                               "Some vars are %d for day, %m for months and "
                               "%Y for the year.",
                               priv->date_format);
        g_object_set_data_full(G_OBJECT(w), "xs-key",
                               g_strdup("date_format"), g_free);
        g_signal_connect(w, "changed", G_CALLBACK(clock_entry_changed), p);
    }
}

static void clock_menu(XsPlugin *p, GtkMenu *m)
{
    PrivData *priv;
    char *user_dir;
    GPtrArray *names;
    GtkWidget *item;
    GtkWidget *sub;
    GtkWidget *mi;
    gsize i;

    if (!p || !m || !p->priv)
        return;
    priv = p->priv;

    /* Свой пункт Clock — как в оригинале */
    mi = gtk_menu_item_new_with_label(_("Get Clock Skins"));
    g_object_set_data_full(G_OBJECT(mi), "xs-cmd", g_strdup("p:get_skins"),
                           g_free);
    g_signal_connect(mi, "activate", G_CALLBACK(xs_core_menu_activate), p);
    gtk_menu_shell_append(GTK_MENU_SHELL(m), mi);
    xs_core_add_separator(GTK_WIDGET(m));

    /* Theme: пользовательские темы затеняют системные (как при загрузке) */
    user_dir = g_build_filename(g_get_user_config_dir(), "xscreenlets",
                                "themes", p->type ? p->type : p->name, NULL);;
    names = g_ptr_array_new_with_free_func(g_free);
    clock_menu_add_themes(user_dir, names);
    /* Системные темы пакета: /usr/share/xscreenlets/<апплет>/<тема> */
    const char *td = xs_core_themedir();
    char *sys_dir = td ? g_build_filename(td, p->type ? p->type : p->name, NULL) : NULL;
    clock_menu_add_themes(sys_dir, names);
    g_free(sys_dir);
    g_free(user_dir);
    if (names->len > 0) {
        g_ptr_array_sort(names, clock_cmp_names);
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
                             G_CALLBACK(clock_theme_toggled), p);
            gtk_menu_shell_append(GTK_MENU_SHELL(sub), mi);
        }
        item = gtk_menu_item_new_with_label(_("Theme"));
        gtk_menu_item_set_submenu(GTK_MENU_ITEM(item), sub);
        gtk_menu_shell_append(GTK_MENU_SHELL(m), item);
    }
    g_ptr_array_unref(names);
}

static void clock_menu_cmd(XsPlugin *p, const char *cmd)
{
    PrivData *priv = p->priv;

    if (strcmp(cmd, "get_skins") == 0) {
        /* как в оригинале ClockScreenlet.py: открыть страницу тем */
        g_spawn_command_line_async(
            "xdg-open http://gnome-look.org/index.php?xcontentmode=186", NULL);
    } else if (strcmp(cmd, "scale-applied") == 0) {
        /* scale изменён в Properties/Size-меню: часы имеют канон 240x240 —
         * подгоняем окно сами (ядро больше не хардкодит 240*x) */
        {
            GKeyFile *kf = xs_core_plugin_conf(p->name);
            double s = xs_host_api()->conf_dbl(kf, p->name, "scale", 1.0);
            if (s < 0.2) s = 0.2; else if (s > 10.0) s = 10.0;
            xs_host_api()->resize(p, (int)(240 * s), (int)(240 * s));
        }
    } else if (g_str_has_prefix(cmd, "theme:")) {
        clock_apply_theme(p, cmd + 6);
    } else if (g_str_has_prefix(cmd, "h24")) {
        priv->h24 = !priv->h24;
        xs_host_api()->conf_set_str(priv->kf, p->name, "h24", priv->h24 ? "true" : "false");
        xs_core_plugin_conf_flush(p->name);
        if (p->win)
            gtk_widget_queue_draw(p->win);
    } else if (g_str_has_prefix(cmd, "seconds")) {
        priv->show_seconds = !priv->show_seconds;
        xs_host_api()->conf_set_str(priv->kf, p->name, "show_seconds", priv->show_seconds ? "true" : "false");
        xs_core_plugin_conf_flush(p->name);
        if (p->win)
            gtk_widget_queue_draw(p->win);
    }
}

/* Plugin descriptor */
static XsPluginOps ops = {
	.init = clock_init,
	.draw = clock_draw,
	.tick = clock_tick,
	.button = clock_button,
	.motion = clock_motion,
	.shutdown = clock_shutdown,
	.menu = clock_menu,
	.menu_cmd = clock_menu_cmd,
	.properties = clock_properties,
	.fill_themes = clock_fill_themes
};

static XsPluginDesc desc = {
	.name = "clock",
	.api_version = XS_API_VERSION,
	.ops = &ops,
	.desc = "The Screenlet-version of MacSlow's cairo-clock. "
	        "A themeable clock with different themes.",
	.author = "kosmik2001 <kosmik2001@gmail.com>",
	.version = "0.6"
};

XsPluginDesc *xs_plugin_desc(void)
{
	return &desc;
}
