/* clearrss.c — Xscreenlets C/GTK3 plugin «clearrss», замена ClearRss 0.1.
 *
 * RSS/Atom reader с одной выбранной лентой, ручным обновлением и
 * периодическим refresh. Сетевой I/O выполняется асинхронно через
 * libsoup 3, XML разбирается libxml2 в worker thread; GTK/Pango/Cairo
 * используются исключительно в главном потоке. */
#include <gtk/gtk.h>
#include <glib.h>
#include <glib/gstdio.h>
#include <gdk/gdk.h>
#include <pango/pangocairo.h>
#include <libsoup/soup.h>
#include <libxml/parser.h>
#include <libxml/tree.h>
#include <string.h>
#include <math.h>
#include <time.h>
#include <sys/stat.h>
#include "xs_api.h"
#include "common.h"

#define RSS_W 200
#define RSS_H 200
#define RSS_MAX_ENTRIES 100
#define RSS_MAX_BYTES (8U * 1024U * 1024U)
#define RSS_TIMEOUT_SEC 20
#define RSS_PI 3.14159265358979323846
#define RSS_HEADER_ALIGN_LEFT 0
#define RSS_HEADER_ALIGN_CENTER 1
#define RSS_HEADER_ALIGN_RIGHT 2

typedef struct {
    char *title;
    char *summary;
    char *link;
    char *published;
    GDateTime *published_time;
} RssEntry;

typedef struct _RequestSet RequestSet;

typedef struct {
    XsPlugin *plugin;
    GKeyFile *kf;
    gdouble scale;
    gdouble opacity;
    gdouble text_color[4];
    gdouble time_color[4];
    gdouble header_color[4];
    gdouble background_color[4];
    char *text_font;
    char *time_font;
    char *header_font;
    char *theme;
    char *feed_name;
    char *feed_url;
    int update_minutes;
    int feed_number;
    int scroll_px;
    int content_extent;
    int content_offset;
    int window_width;
    int window_height;
    int news_count;
    int visible_count;
    gboolean auto_news_count;
    gboolean show_published_time;
    int button_pressed;
    /* Геометрия подсказки "...(more)" в координатах виджета: заполняется
     * при отрисовке, читается при попадании мыши. */
    int more_x, more_y, more_w, more_h;
    gboolean more_shown;
    /* Форма окна: применённый радиус и размер, для которых она уже
     * посчитана. Скругление у нас было ТОЛЬКО рисунком (cairo_clip внутри
     * отрисовки), а само окно оставалось прямоугольным, и в его углы был
     * виден рабочий стол. В frame_launcher это уже решено через
     * gdk_window_shape_combine_region, здесь повторяем. */
    int shape_radius;
    int shape_w, shape_h;
    gboolean loading;
    gboolean show_feed_name;
    int header_align;
    GPtrArray *entries;
    GString *status;
    char *site_url;
    RequestSet *requests;
    SoupSession *session;
    guint refresh_source;
    guint feed_url_source;
    gint64 last_refresh_us;
    guint64 generation;
} PrivData;

typedef struct _RequestSet {
    gint refcount;
    gint alive;
    GPtrArray *jobs;
    GMutex lock;
    gboolean active;
} RequestSet;

typedef struct {
    RequestSet *set;
    GBytes *bytes;
    GInputStream *stream;
    GByteArray *body;
    SoupMessage *message;
    SoupSession *session;
    GCancellable *cancellable;
    char *url;
    char *instance_name;
    guint idle_source;
    guint64 generation;
    gboolean reading;
    gboolean error;
} FetchJob;

static gboolean rss_display_entries(PrivData *priv)
{
    return priv && priv->entries && priv->entries->len;
}

static void rss_open_current(XsPlugin *p)
{
    PrivData *priv = p ? p->priv : NULL;
    const RssEntry *entry;
    const char *url;
    /* argv ОБЯЗАН быть NULL-терминирован: g_spawn_* читает массив до
     * первого NULL. Размер 2 означает только два полезных элемента, но
     * без argv[2] = NULL GLib уходит в мусор за пределы массива и
     * возвращает FALSE - процесс не запускается, ошибки не видно. */
    char *argv[3];

    if (!priv)
        return;
    entry = rss_display_entries(priv) ? g_ptr_array_index(priv->entries,
                              CLAMP(priv->feed_number, 0,
                                    (int)priv->entries->len - 1)) : NULL;
    url = entry && entry->link && entry->link[0] ? entry->link :
          (priv->site_url && priv->site_url[0] ? priv->site_url : NULL);
    xs_host_api()->log("clearrss: open_current feed_number=%d entries=%u "
                       "has_entry=%d link=%s site_url=%s scroll_px=%d",
                       priv->feed_number,
                       priv->entries ? priv->entries->len : 0,
                       entry ? 1 : 0,
                       (entry && entry->link) ? entry->link : "(null)",
                       (priv->site_url && priv->site_url[0]) ? priv->site_url
                                                            : "(null)",
                       priv->scroll_px);
    if (!url) {
        xs_host_api()->log("clearrss: open_current - no url, nothing opened");
        return;
    }
    argv[0] = (char *)"xdg-open";
    argv[1] = (char *)url;
    argv[2] = NULL;
    if (!g_spawn_async(NULL, argv, NULL, G_SPAWN_SEARCH_PATH,
                       NULL, NULL, NULL, NULL))
        xs_host_api()->log("clearrss: open_current - spawn FAILED for %s",
                           url);
    else
        xs_host_api()->log("clearrss: open_current - spawned xdg-open %s",
                           url);
}

static void rss_job_free(FetchJob *job);
static void rss_request_set_stop(RequestSet *set);
static gboolean rss_finish_fetch(gpointer data);
static void rss_request_refresh(XsPlugin *p, gboolean force);
static void rss_scroll_by(XsPlugin *p, int delta, gboolean page_when_fit);
static int rss_viewport_height(XsPlugin *p);
static int rss_display_count(PrivData *priv, PangoLayout *layout, int height);
static gboolean rss_display_entries(PrivData *priv);
static void rss_add_theme_dirs(const char *dir, GPtrArray *names);
static void rss_open_current(XsPlugin *p);

/* ---------- theme ---------- */

static char *rss_project_root_from_plugdir(void)
{
    const char *plugdir = xs_core_plugdir();

    char *parent, *out;

    if (!plugdir || !plugdir[0])
        return NULL;
    /* g_path_get_dirname() возвращает новую строку: освобождаем её,
     * иначе течёт по 37 байт на каждый вызов. */
    parent = g_path_get_dirname(plugdir);
    out = g_build_filename(parent, "themes", "clearrss", NULL);
    g_free(parent);
    return out;
}

static gboolean rss_load_theme(XsPlugin *p, const char *name)
{
    char *dir;
    char *user;
    gboolean ok;

    if (!p || !name || !name[0])
        return FALSE;
    /* Единый поиск: пользовательские темы ($XDG_CONFIG_HOME, legacy
     * ~/.xscreenlets) имеют приоритет над системными. Прежде здесь стоял
     * свой обход - XDG, потом dirname(plugdir), потом несуществующий
     * /usr/share/screenlets/ClearRss. С переносом плагинов в
     * /usr/libexec/xscreenlets dirname(plugdir) дал бы /usr/libexec, и темы
     * перестали бы находиться. Системный каталог теперь задаётся
     * -DXS_THEME_DIR при сборке или --themedir. */
    user = xs_core_find_theme("clearrss", name);
    ok = xs_host_api()->theme_load(p, user);
    if (getenv("XSCREENLETS_DEBUG_THEME"))
        p->host->log("clearrss: theme dir=%s loaded=%d shadow=%d shadow_mid=%d button_bg=%d",
                     user ? user : "(null)", ok,
                     xs_core_theme_has(p, "shadow"),
                     xs_core_theme_has(p, "shadow_mid"),
                     xs_core_theme_has(p, "button_bg"));
    g_free(user);
    if (ok)
        return TRUE;
    /* Запасной путь: тема рядом с плагином в дереве исходников - нужен
     * при разработке, когда демон запускается из build/. */
    dir = rss_project_root_from_plugdir();
    if (dir) {
        char *project = g_build_filename(dir, name, NULL);
        ok = xs_host_api()->theme_load(p, project);
        g_free(project);
    }
    g_free(dir);
    return ok;
}

/* ---------- config helpers ---------- */
static gboolean rss_conf_bool(GKeyFile *kf, const char *sec, const char *key,
                              gboolean def)
{
    char *s = xs_host_api()->conf_str(kf, sec, key, def ? "true" : "false");
    gboolean value = g_ascii_strcasecmp(s, "true") == 0 ||
                     strcmp(s, "1") == 0 ||
                     g_ascii_strcasecmp(s, "yes") == 0;

    g_free(s);
    return value;
}

static gboolean rss_parse_color(const char *text, gdouble out[4])
{
    const char *p = text;
    int i;

    if (!text || !text[0])
        return FALSE;
    for (i = 0; i < 4; i++) {
        char *end = NULL;
        gdouble value = g_ascii_strtod(p, &end);

        if (end == p || !isfinite(value))
            return FALSE;
        out[i] = CLAMP(value, 0.0, 1.0);
        p = end;
        while (g_ascii_isspace(*p))
            p++;
        if (i < 3) {
            if (*p != ',')
                return FALSE;
            p++;
        } else if (*p != '\0') {
            return FALSE;
        }
    }
    return TRUE;
}

static char *rss_color_string(const GdkRGBA *color)
{
    char r[32], g[32], b[32], a[32];
    char *out;

    g_ascii_formatd(r, sizeof(r), "%.9g", color->red);
    g_ascii_formatd(g, sizeof(g), "%.9g", color->green);
    g_ascii_formatd(b, sizeof(b), "%.9g", color->blue);
    g_ascii_formatd(a, sizeof(a), "%.9g", color->alpha);
    out = g_strdup_printf("%s,%s,%s,%s", r, g, b, a);
    return out;
}

static void rss_ensure_color(GKeyFile *kf, const char *sec, const char *key,
                            const gdouble def[4])
{
    char *value;
    gdouble parsed[4];
    gboolean valid = FALSE;

    if (g_key_file_has_key(kf, sec, key, NULL)) {
        char *existing = g_key_file_get_string(kf, sec, key, NULL);

        if (existing) {
            valid = rss_parse_color(existing, parsed);
            g_free(existing);
        }
        if (valid)
            return;
    }
    value = rss_color_string(&(GdkRGBA){def[0], def[1], def[2], def[3]});
    g_key_file_set_string(kf, sec, key, value);
    g_free(value);
}

static void rss_read_color(GKeyFile *kf, const char *sec, const char *key,
                           const gdouble def[4], gdouble out[4])
{
    char *s = xs_host_api()->conf_str(kf, sec, key, NULL);

    memcpy(out, def, sizeof(gdouble) * 4);
    if (!rss_parse_color(s, out)) {
        memcpy(out, def, sizeof(gdouble) * 4);
        {
            char *fallback = rss_color_string(
                &(GdkRGBA){def[0], def[1], def[2], def[3]});

            g_key_file_set_string(kf, sec, key, fallback);
            g_free(fallback);
        }
    }
    g_free(s);
}

static void rss_flush(PrivData *priv)
{
    if (priv && priv->kf)
        xs_core_plugin_conf_flush(priv->plugin->name);
}

static void rss_status(PrivData *priv, const char *format, ...)
{
    va_list ap;

    if (!priv)
        return;
    g_string_truncate(priv->status, 0);
    va_start(ap, format);
    g_string_vprintf(priv->status, format, ap);
    va_end(ap);
    if (priv->plugin && priv->plugin->win)
        gtk_widget_queue_draw(priv->plugin->win);
}

/* ---------- XML helpers ---------- */
static char *rss_plain_text(const char *html);
static GDateTime *rss_parse_published(const char *raw);

static gboolean rss_local_name(const xmlNode *node, const char *name)
{
    return node && node->type == XML_ELEMENT_NODE && node->name &&
           xmlStrcasecmp(node->name, (const xmlChar *)name) == 0;
}

static char *rss_node_text(const xmlNode *root, const char *name)
{
    xmlNode *node;

    if (!root)
        return NULL;
    for (node = (xmlNode *)root; node; node = node->next) {
        if (rss_local_name(node, name)) {
            xmlChar *raw = xmlNodeGetContent(node);
            char *value;

            if (!raw)
                return NULL;
            value = g_strdup((const char *)raw);
            xmlFree(raw);
            g_strstrip(value);
            return value;
        }
    }
    return NULL;
}

static char *rss_child_text(const xmlNode *parent, const char *name)
{
    xmlNode *node;

    if (!parent)
        return NULL;
    for (node = parent->children; node; node = node->next) {
        if (rss_local_name(node, name)) {
            xmlChar *raw = xmlNodeGetContent(node);
            char *value;

            if (!raw)
                return NULL;
            value = g_strdup((const char *)raw);
            xmlFree(raw);
            g_strstrip(value);
            return value;
        }
    }
    return NULL;
}

static char *rss_child_plain(const xmlNode *parent, const char *name)
{
    char *raw = rss_child_text(parent, name);
    char *plain = rss_plain_text(raw);

    g_free(raw);
    return plain;
}

static char *rss_node_link(const xmlNode *parent)
{
    xmlNode *node;
    char *fallback = NULL;

    if (!parent)
        return NULL;
    for (node = parent->children; node; node = node->next) {
        if (rss_local_name(node, "link")) {
            xmlChar *href = xmlGetProp(node, BAD_CAST "href");
            char *value = NULL;

            if (href) {
                value = g_strdup((const char *)href);
                xmlFree(href);
            } else {
                xmlChar *raw = xmlNodeGetContent(node);
                if (raw) {
                    value = g_strdup((const char *)raw);
                    xmlFree(raw);
                }
            }
            if (value && value[0])
                return value;
            g_free(value);
        } else if (!fallback &&
                   (rss_local_name(node, "guid") || rss_local_name(node, "id"))) {
            xmlChar *raw = xmlNodeGetContent(node);
            if (raw) {
                fallback = g_strdup((const char *)raw);
                xmlFree(raw);
            }
        }
    }
    if (fallback && (!g_str_has_prefix(fallback, "http://") &&
                     !g_str_has_prefix(fallback, "https://"))) {
        g_free(fallback);
        fallback = NULL;
    }
    return fallback;
}

static char *rss_channel_link(const xmlNode *parent)
{
    xmlNode *node;
    char *fallback = NULL;

    if (!parent)
        return NULL;
    for (node = parent->children; node; node = node->next) {
        if (rss_local_name(node, "link")) {
            xmlChar *href = xmlGetProp(node, BAD_CAST "href");
            xmlChar *rel = xmlGetProp(node, BAD_CAST "rel");
            char *value = NULL;
            gboolean alternate = !rel ||
                g_ascii_strcasecmp((const char *)rel, "alternate") == 0;

            if (href)
                value = g_strdup((const char *)href);
            else {
                xmlChar *raw = xmlNodeGetContent(node);
                if (raw) {
                    value = g_strdup((const char *)raw);
                    xmlFree(raw);
                }
            }
            if (rel)
                xmlFree(rel);
            if (href)
                xmlFree(href);
            if (alternate && value && value[0])
                return value;
            g_free(value);
        } else if (!fallback && rss_local_name(node, "id")) {
            xmlChar *raw = xmlNodeGetContent(node);
            if (raw) {
                fallback = g_strdup((const char *)raw);
                xmlFree(raw);
            }
        }
    }
    if (fallback && (!g_str_has_prefix(fallback, "http://") &&
                     !g_str_has_prefix(fallback, "https://"))) {
        g_free(fallback);
        fallback = NULL;
    }
    return fallback;
}

static char *rss_plain_text(const char *html)
{
    GString *out;
    const char *p;
    gboolean in_tag = FALSE;
    gboolean space_pending = FALSE;

    if (!html)
        return g_strdup("");
    out = g_string_new(NULL);
    for (p = html; *p; p++) {
        guchar c = (guchar)*p;

        if (c == '<') {
            in_tag = TRUE;
            if (p[1] == '/') {
                const char *q = p + 2;
                while (*q && *q != '>') q++;
                if (g_ascii_strncasecmp(p + 2, "br", 2) == 0 ||
                    g_ascii_strncasecmp(p + 2, "p", 1) == 0)
                    g_string_append_c(out, '\n');
            }
            continue;
        }
        if (in_tag) {
            if (*p == '>')
                in_tag = FALSE;
            continue;
        }
        if (c == '&') {
            /* минимальное декодирование XML entities */
            if (g_str_has_prefix(p, "&amp;")) { g_string_append_c(out, '&'); p += 4; }
            else if (g_str_has_prefix(p, "&lt;")) { g_string_append_c(out, '<'); p += 3; }
            else if (g_str_has_prefix(p, "&gt;")) { g_string_append_c(out, '>'); p += 3; }
            else if (g_str_has_prefix(p, "&quot;")) { g_string_append_c(out, '"'); p += 5; }
            else if (g_str_has_prefix(p, "&#39;")) { g_string_append_c(out, '\''); p += 4; }
            else if (g_str_has_prefix(p, "&apos;")) { g_string_append_c(out, '\''); p += 5; }
            else g_string_append_c(out, '&');
            space_pending = FALSE;
            continue;
        }
        if (g_ascii_isspace(c)) {
            if (out->len)
                space_pending = TRUE;
            continue;
        }
        if (space_pending) {
            g_string_append_c(out, ' ');
            space_pending = FALSE;
        }
        g_string_append_c(out, *p);
    }
    return g_string_free(out, FALSE);
}

static void rss_entry_free(gpointer data)
{
    RssEntry *entry = data;

    if (!entry)
        return;
    g_free(entry->title);
    g_free(entry->summary);
    g_free(entry->link);
    g_free(entry->published);
    g_clear_pointer(&entry->published_time, g_date_time_unref);
    g_free(entry);
}

static void rss_parse_feed(const char *data, gsize len, GPtrArray *entries,
                           char **feed_title, char **feed_url)
{
    xmlDocPtr doc;
    xmlNode *root;
    xmlNode *node;
    int count = 0;

    if (len > RSS_MAX_BYTES)
        return;
    doc = xmlReadMemory(data, (int)len, "feed.xml", NULL,
                        XML_PARSE_NONET | XML_PARSE_NOERROR |
                        XML_PARSE_NOWARNING | XML_PARSE_RECOVER);
    if (!doc)
        return;
    root = xmlDocGetRootElement(doc);
    if (rss_local_name(root, "feed")) {
        char *title = rss_node_text(root, "title");
        char *link = rss_channel_link(root);
        if (link)
            *feed_url = link;
        else
            g_free(link);
        if (title)
            *feed_title = title;
        for (node = root->children; node; node = node->next) {
            if (!rss_local_name(node, "entry") || count >= RSS_MAX_ENTRIES)
                continue;
            RssEntry *entry = g_new0(RssEntry, 1);
            entry->title = rss_child_text(node, "title");
            entry->summary = rss_child_plain(node, "summary");
            if (!entry->summary || !entry->summary[0]) {
                g_free(entry->summary);
                entry->summary = rss_child_plain(node, "content");
            }
            entry->link = rss_node_link(node);
            entry->published = rss_child_text(node, "published");
            if (!entry->published)
                entry->published = rss_child_text(node, "updated");
            entry->published_time = rss_parse_published(entry->published);
            if (!entry->title && !entry->summary) {
                rss_entry_free(entry);
                continue;
            }
            g_ptr_array_add(entries, entry);
            count++;
        }
    } else {
        xmlNode *channel = NULL;
        for (node = root ? root->children : NULL; node; node = node->next)
            if (rss_local_name(node, "channel")) { channel = node; break; }
        if (channel) {
            char *title = rss_child_text(channel, "title");
            char *link = rss_channel_link(channel);
            if (title)
                *feed_title = title;
            if (link)
                *feed_url = link;
            else
                g_free(link);
            for (node = channel->children; node; node = node->next) {
                if (!rss_local_name(node, "item") || count >= RSS_MAX_ENTRIES)
                    continue;
                RssEntry *entry = g_new0(RssEntry, 1);
                entry->title = rss_child_text(node, "title");
                entry->summary = rss_child_plain(node, "description");
                entry->link = rss_node_link(node);
                entry->published = rss_child_text(node, "pubDate");
                if (!entry->published)
                    entry->published = rss_child_text(node, "date");
                entry->published_time = rss_parse_published(entry->published);
                if (!entry->title && !entry->summary && !entry->link) {
                    rss_entry_free(entry);
                    continue;
                }
                g_ptr_array_add(entries, entry);
                count++;
            }
        }
    }
    xmlFreeDoc(doc);
}

/* ---------- networking / worker ---------- */

static RequestSet *rss_request_set_ref(RequestSet *set)
{
    if (set)
        g_atomic_int_inc(&set->refcount);
    return set;
}

static void rss_request_set_stop(RequestSet *set)
{
    if (!set)
        return;
    g_mutex_lock(&set->lock);
    set->active = FALSE;
    g_mutex_unlock(&set->lock);
}

static void rss_request_set_unref(RequestSet *set)
{
    if (!set || !g_atomic_int_dec_and_test(&set->refcount))
        return;
    g_mutex_lock(&set->lock);
    set->active = FALSE;
    g_mutex_unlock(&set->lock);
    g_ptr_array_unref(set->jobs);
    g_mutex_clear(&set->lock);
    g_free(set);
}

static void rss_job_free(FetchJob *job)
{
    if (!job)
        return;
    g_clear_pointer(&job->bytes, g_bytes_unref);
    g_clear_object(&job->stream);
    g_clear_pointer(&job->body, g_byte_array_unref);
    g_clear_object(&job->message);
    g_clear_object(&job->cancellable);
    g_clear_object(&job->session);
    g_free(job->url);
    g_free(job->instance_name);
    if (job->idle_source)
        g_source_remove(job->idle_source);
    rss_request_set_unref(job->set);
    g_free(job);
}

static gboolean rss_finish_fetch(gpointer data)
{
    FetchJob *job = data;
    XsPlugin *p = NULL;
    gboolean active;
    gboolean same_url;
    gboolean loaded = FALSE;
    gsize size;
    gconstpointer bytes;
    GPtrArray *parsed = g_ptr_array_new_with_free_func(rss_entry_free);
    char *parsed_title = NULL;
    char *parsed_url = NULL;

    g_mutex_lock(&job->set->lock);
    active = job->set->active;
    g_mutex_unlock(&job->set->lock);
    p = active ? xs_core_find_instance(job->instance_name) : NULL;
    if (p) {
        PrivData *current = p->priv;
        same_url = current && current->generation == job->generation &&
                   g_strcmp0(current->feed_url, job->url) == 0;
        if (same_url)
            current->loading = FALSE;
    } else {
        same_url = FALSE;
    }
    size = job->bytes ? g_bytes_get_size(job->bytes) : 0;
    bytes = job->bytes ? g_bytes_get_data(job->bytes, NULL) : NULL;
    if (p && same_url && size && !job->error) {
        rss_parse_feed((const char *)bytes, size, parsed,
                       &parsed_title, &parsed_url);
        if (parsed->len) {
            PrivData *priv = p->priv;

            g_clear_pointer(&priv->entries, g_ptr_array_unref);
            priv->entries = parsed;
            priv->feed_number = CLAMP(priv->feed_number, 0,
                                      (int)priv->entries->len - 1);
            priv->scroll_px = 0;
            if (parsed_title && priv->feed_name &&
                g_strcmp0(priv->feed_name, "RSS") == 0) {
                g_free(priv->feed_name);
                priv->feed_name = parsed_title;
            } else {
                g_free(parsed_title);
            }
            if (parsed_url && parsed_url[0]) {
                g_free(priv->site_url);
                priv->site_url = parsed_url;
                parsed_url = NULL;
            }
            rss_status(priv, "%u entries", parsed->len);
            loaded = TRUE;
            priv->last_refresh_us = g_get_monotonic_time();
            if (p->win)
                gtk_widget_queue_draw(p->win);
        } else {
            g_ptr_array_unref(parsed);
            g_free(parsed_title);
        }
    } else {
        g_ptr_array_unref(parsed);
        g_free(parsed_title);
    }
    if (p && same_url && !loaded) {
        if (job->error && bytes)
            rss_status((PrivData *)p->priv, "Feed error: %s",
                       (const char *)bytes);
        else
            rss_status((PrivData *)p->priv,
                       "Feed unavailable or empty: %s", job->url);
    }
    g_free(parsed_url);
    g_mutex_lock(&job->set->lock);
    g_ptr_array_remove_fast(job->set->jobs, job);
    job->set->alive--;
    g_mutex_unlock(&job->set->lock);
    job->idle_source = 0;
    rss_job_free(job);
    return G_SOURCE_REMOVE;
}

static void rss_read_thread(GTask *task, gpointer source,
                           gpointer data, GCancellable *cancellable)
{
    FetchJob *job = g_task_get_task_data(task);
    GError *error = NULL;
    GByteArray *body;
    guchar buffer[8192];
    gsize got;
    gsize total = 0;

    (void)data;
    (void)source;
    if (!job) {
        g_task_return_new_error(task, G_IO_ERROR, G_IO_ERROR_FAILED,
                                "Feed read job was destroyed");
        return;
    }
    body = g_byte_array_new();
    while ((got = g_input_stream_read(job->stream, buffer, sizeof(buffer),
                                      cancellable, &error)) > 0) {
        if (total + got > RSS_MAX_BYTES) {
            g_set_error_literal(&error, G_IO_ERROR, G_IO_ERROR_FAILED,
                                "Feed response is too large");
            break;
        }
        g_byte_array_append(body, buffer, got);
        total += got;
    }
    g_input_stream_close(job->stream, NULL, NULL);
    if (error) {
        g_byte_array_unref(body);
        g_task_return_error(task, error);
        return;
    }
    g_task_return_pointer(task, g_byte_array_free_to_bytes(body),
                          (GDestroyNotify)g_bytes_unref);
}

static void rss_read_done(GObject *source, GAsyncResult *result,
                          gpointer user_data)
{
    FetchJob *job = user_data;
    GError *error = NULL;
    GBytes *bytes;

    (void)source;
    bytes = g_task_propagate_pointer(G_TASK(result), &error);
    if (!bytes) {
        char *text = g_strdup(error ? error->message : "network request failed");
        gsize len = strlen(text);
        job->bytes = g_bytes_new_take(text, len + 1);
        job->error = TRUE;
        g_clear_error(&error);
    } else {
        job->bytes = bytes;
    }
    job->reading = FALSE;
    g_clear_object(&job->session);
    job->idle_source = g_idle_add(rss_finish_fetch, job);
}

static void rss_fetch_done(GObject *source, GAsyncResult *result,
                          gpointer user_data)
{
    FetchJob *job = user_data;
    GError *error = NULL;
    GInputStream *stream;
    GTask *task;

    stream = soup_session_send_finish(SOUP_SESSION(source), result, &error);
    if (!stream) {
        char *text = g_strdup(error ? error->message : "network request failed");
        gsize len = strlen(text);
        job->bytes = g_bytes_new_take(text, len + 1);
        job->error = TRUE;
        g_clear_error(&error);
        g_clear_object(&job->session);
        job->idle_source = g_idle_add(rss_finish_fetch, job);
        return;
    }
    job->stream = stream;
    if (soup_message_get_status(job->message) >= 400) {
        char *text = g_strdup_printf("HTTP error %u\n",
                                     soup_message_get_status(job->message));
        gsize len = strlen(text);
        job->bytes = g_bytes_new_take(text, len + 1);
        job->error = TRUE;
        g_clear_object(&job->session);
        job->idle_source = g_idle_add(rss_finish_fetch, job);
        return;
    }
    job->reading = TRUE;
    task = g_task_new(NULL, job->cancellable, rss_read_done, job);
    g_task_set_task_data(task, job, NULL);
    g_task_run_in_thread(task, rss_read_thread);
    g_object_unref(task);
}

static void rss_request_refresh(XsPlugin *p, gboolean force)
{
    PrivData *priv = p ? p->priv : NULL;
    FetchJob *job;
    SoupMessage *msg;

    if (!priv || !priv->session || !priv->requests || !priv->feed_url ||
        !priv->feed_url[0] || priv->loading)
        return;
    if (!force && priv->last_refresh_us &&
        g_get_monotonic_time() - priv->last_refresh_us <
        (gint64)priv->update_minutes * 60 * 1000000)
        return;
    msg = soup_message_new("GET", priv->feed_url);
    if (!msg)
        return;
    job = g_new0(FetchJob, 1);
    job->set = rss_request_set_ref(priv->requests);
    job->message = msg;
    job->session = g_object_ref(priv->session);
    job->cancellable = g_cancellable_new();
    job->url = g_strdup(priv->feed_url);
    job->instance_name = g_strdup(p->name);
    priv->generation++;
    job->generation = priv->generation;
    priv->loading = TRUE;
    rss_status(priv, "Refreshing...");
    g_mutex_lock(&priv->requests->lock);
    g_ptr_array_add(priv->requests->jobs, job);
    priv->requests->alive++;
    g_mutex_unlock(&priv->requests->lock);
    soup_session_send_async(job->session, job->message,
                            G_PRIORITY_DEFAULT, job->cancellable,
                            rss_fetch_done, job);
}

static gboolean rss_refresh_timer(gpointer data)
{
    XsPlugin *p = data;

    if (!p || !p->priv || !p->win)
        return G_SOURCE_REMOVE;
    rss_request_refresh(p, FALSE);
    return G_SOURCE_CONTINUE;
}

/* ---------- drawing ---------- */
static GDateTime *rss_parse_published(const char *raw)
{
    static const char *months[] = {
        "Jan", "Feb", "Mar", "Apr", "May", "Jun",
        "Jul", "Aug", "Sep", "Oct", "Nov", "Dec"
    };
    const char *date;
    const char *zone;
    char month[16];
    gint day, year, hour, minute, second;
    gint month_num = 0;
    gint offset = 0;
    GDateTime *utc;
    gsize i;

    if (!raw || !raw[0])
        return NULL;
    {
        GDateTime *iso = g_date_time_new_from_iso8601(raw, NULL);

        if (iso)
            return iso;
    }

    date = strchr(raw, ',');
    date = date ? date + 1 : raw;
    if (sscanf(date, " %d %15s %d %d:%d:%d",
               &day, month, &year, &hour, &minute, &second) != 6)
        return NULL;
    for (i = 0; i < G_N_ELEMENTS(months); i++) {
        if (g_ascii_strcasecmp(month, months[i]) == 0) {
            month_num = (gint)i + 1;
            break;
        }
    }
    if (!month_num)
        return NULL;

    zone = strrchr(raw, ' ');
    if (zone && zone[1] == '+' && strlen(zone + 1) >= 5) {
        offset = (zone[2] - '0') * 10 + (zone[3] - '0');
        offset = offset * 60 + (zone[4] - '0') * 10 + (zone[5] - '0');
        if (offset > 12 * 60 || (offset % 60) != 0)
            offset = 0;
    } else if (zone && zone[1] == '-' && strlen(zone + 1) >= 5) {
        offset = -((zone[2] - '0') * 10 + (zone[3] - '0'));
        offset = offset * 60 - (zone[4] - '0') * 10 - (zone[5] - '0');
        if (offset < -12 * 60 || (offset % 60) != 0)
            offset = 0;
    }

    utc = g_date_time_new_utc(year, month_num, day, hour, minute, second);
    if (!utc)
        return NULL;
    {
        gint64 epoch = g_date_time_to_unix(utc) - offset * 60LL;

        g_date_time_unref(utc);
        return g_date_time_new_from_unix_utc(epoch);
    }
}

static char *rss_entry_title_markup(const PrivData *priv,
                                    const RssEntry *entry)
{
    char *title;
    char *escaped;
    char *clock;
    GDateTime *local;
    char *result;
    char *clock_markup;
    PangoFontDescription *time_desc;
    char *time_desc_string;

    if (!entry)
        return g_strdup("");
    title = entry->title ? entry->title : "";
    if (!priv || !priv->show_published_time || !entry->published_time) {
        escaped = g_markup_escape_text(title, -1);
        result = g_strdup_printf("<b>%s</b>", escaped);
        g_free(escaped);
        return result;
    }

    local = g_date_time_new_from_unix_local(
        g_date_time_to_unix(entry->published_time));
    if (!local) {
        escaped = g_markup_escape_text(title, -1);
        result = g_strdup_printf("<b>%s</b>", escaped);
        g_free(escaped);
        return result;
    }
    clock = g_date_time_format(local, "%H:%M:%S");
    time_desc = pango_font_description_from_string(
        priv->time_font ? priv->time_font : "Sans Bold 9");
    if (!time_desc)
        time_desc = pango_font_description_from_string("Sans Bold 9");
    if (time_desc) {
        if (pango_font_description_get_size(time_desc) <= 0)
            pango_font_description_set_size(time_desc, 9 * PANGO_SCALE);
        time_desc_string = pango_font_description_to_string(time_desc);
        pango_font_description_free(time_desc);
    } else {
        time_desc_string = g_strdup("Sans Bold 9");
    }
    clock_markup = g_strdup_printf(
        "<span font_desc=\"%s\" foreground=\"#%02X%02X%02X\">%s</span>",
        time_desc_string,
        (unsigned)lround(CLAMP(priv->time_color[0], 0.0, 1.0) * 255.0),
        (unsigned)lround(CLAMP(priv->time_color[1], 0.0, 1.0) * 255.0),
        (unsigned)lround(CLAMP(priv->time_color[2], 0.0, 1.0) * 255.0), clock);
    escaped = g_markup_escape_text(title, -1);
    if (escaped[0])
        result = g_strdup_printf("%s: <b>%s</b>", clock_markup, escaped);
    else
        result = g_strdup(clock_markup);
    g_free(clock_markup);
    g_free(time_desc_string);
    g_free(clock);
    g_free(escaped);
    g_date_time_unref(local);
    return result;
}

static int rss_font_size(const PrivData *priv)
{
    PangoFontDescription *font;
    int size;

    font = pango_font_description_from_string(
        priv && priv->text_font ? priv->text_font : "Sans 9");
    size = font ? pango_font_description_get_size(font) / PANGO_SCALE : 9;
    if (font)
        pango_font_description_free(font);
    return MAX(6, size);
}

static int rss_header_height(PrivData *priv)
{
    (void)priv;
    /* Верхний визуальный блок SVG — rect высотой около 39px.
     * Не используем h/2: это оставляло текст ниже невидимого запаса. */
    return 40;
}

static int rss_control_height(const PrivData *priv)
{
    (void)priv;
    /* Раньше кнопки имели радиус 6px и попадали в полосу 20px.
     * Теперь рисуем/hit-test полосу 32px, а сами круги — 24px. */
    return 32;
}

static int rss_button_radius(const PrivData *priv)
{
    (void)priv;
    return 12;
}

static int rss_viewport_height(XsPlugin *p)
{
    PrivData *priv = p ? p->priv : NULL;
    int h = priv ? priv->window_height : RSS_H;

    /* Заголовок и нижние кнопки имеют фиксированную высоту. Увеличение
     * окна должно расширять только область списка, а не header/footer. */
    return MAX(24, h - rss_header_height(priv) - rss_control_height(priv) - 14);
}

static int rss_display_count(PrivData *priv, PangoLayout *layout, int height)
{
    int requested;
    int viewport;
    int line_h;
    int count = 0;
    PangoRectangle logical;
    GString *all;

    if (!priv || !priv->entries || !priv->entries->len || !layout)
        return 0;
    requested = priv->auto_news_count ? RSS_MAX_ENTRIES : priv->news_count;
    requested = CLAMP(requested, 1, (int)priv->entries->len);
    viewport = MAX(24, height - rss_header_height(priv) -
                   rss_control_height(priv) - 14);
    line_h = rss_font_size(priv) + 3;
    all = g_string_new(NULL);

    /* Считаем реальные строки каждой записи через Pango, пока их
     * суммарная высота помещается в viewport. Это устраняет пустую
     * нижнюю четверть и не зависит от эвристики длины текста. */
    for (gsize i = 0; i < (gsize)requested; i++) {
        const RssEntry *entry = g_ptr_array_index(priv->entries, i);
        char *title = rss_entry_title_markup(priv, entry);
        char *summary = g_markup_escape_text(entry->summary ? entry->summary : "", -1);

        if (count)
            g_string_append_c(all, '\n');
        g_string_append_printf(all, "%s\n%s", title, summary);
        g_free(title);
        g_free(summary);
        pango_layout_set_markup(layout, all->str, -1);
        pango_layout_get_pixel_extents(layout, NULL, &logical);
        if (count > 0 && logical.height + line_h / 2 > viewport)
            break;
        count++;
    }
    g_string_free(all, TRUE);
    return CLAMP(MAX(1, count), 1, (int)priv->entries->len);
}

/* Радиус скругления углов окна. Раньше он нигде не был нужен как
 * отдельная величина: скругление существовало только как путь
 * rss_draw_rounded внутри отрисовки. Для формы окна значение нужно
 * числом, поэтому здесь константа - та же, что используется в рисунке. */
/* Радиус скругления углов окна. ЕДИНСТВЕННОЕ место, где он считается:
 * и форма окна (rss_apply_shape), и заливка, и clip темы берут значение
 * отсюда. Раньше здесь стояло константное 17, а в rss_draw радиус
 * считался отдельно как MIN(MIN(17, w/8), h/8) - при w<137 или h<137
 * значения расходились, и рисунок вылезал за углы, которые окно уже
 * срезало. */
static int rss_corner_radius(int w, int h)
{
    return MIN(MIN(17, w / 8), h / 8);
}

/* Регион формы окна: полосы по 1 px, средняя полоса сплошная.
 * Без средней полосы регион состоит из двух "скобок" у краёв и всё
 * содержимое окна в середине обрезается до рабочего стола. */
static cairo_region_t *rss_rounded_region(int width, int height, int radius)
{
    cairo_region_t *region;
    cairo_rectangle_int_t box;
    double scaled;

    if (width <= 0 || height <= 0)
        return NULL;
    scaled = MIN(radius, MIN(width, height) / 2.0);
    if (scaled <= 0.0)
        return NULL;
    region = cairo_region_create();
    if (!region)
        return NULL;
    for (int i = 0; i <= (int)ceil(scaled); i++) {
        double d = fabs(i - scaled);
        int cut = 0;
        if (d <= scaled)
            cut = (int)floor(scaled -
                             sqrt(scaled * scaled - d * d));
        box.x = i;
        box.y = cut;
        box.width = 1;
        box.height = height - 2 * cut;
        if (box.height > 0)
            cairo_region_union_rectangle(region, &box);
        box.x = width - 1 - i;
        if (box.height > 0)
            cairo_region_union_rectangle(region, &box);
    }
    {
        int mid = (int)ceil(scaled);
        box.x = mid;
        box.y = 0;
        box.width = width - 2 * mid;
        box.height = height;
        if (box.width > 0)
            cairo_region_union_rectangle(region, &box);
    }
    return region;
}

/* Применяет форму окна. Радиус и размер запоминаем, чтобы не пересчитывать
 * регион на каждой перерисовке. */
static void rss_apply_shape(XsPlugin *p, int w, int h)
{
    PrivData *priv = p ? p->priv : NULL;
    GdkWindow *window;
    cairo_region_t *region;
    int radius;

    if (!priv || !p->win || w <= 0 || h <= 0)
        return;
    radius = rss_corner_radius(w, h);
    if (priv->shape_radius == radius && priv->shape_w == w &&
        priv->shape_h == h)
        return;
    window = gtk_widget_get_window(p->win);
    if (!window)
        return;
    region = rss_rounded_region(w, h, radius);
    if (region) {
        gdk_window_shape_combine_region(window, region, 0, 0);
        cairo_region_destroy(region);
    }
    priv->shape_radius = radius;
    priv->shape_w = w;
    priv->shape_h = h;
}

static void rss_draw_rounded(cairo_t *cr, double x, double y, double w,
                             double h, double radius)
{
    /* Радиус углов r берётся как есть. Раньше здесь стояло
     * d = M_PI * radius / 180.0 — это перевод радиуса ИЗ ГРАДУСОВ в
     * радианы, то есть radius принимался за угол. При radius=17 выходило:
     * радиус дуги 0.30 px (вместо 17) и углы 17°/34°/51° вместо
     * 90°/180°/270°. Плюс центр ВТОРОЙ дуги стоял (x2+d, y2+d) — это
     * верх-левый угол, хотя путь только что пришёл вниз-лево. Итог:
     * путь самопересекался, и cairo_fill давал треугольник с
     * диагональю плюс вытянутые овалы на нижних углах.
     * Ниже все четыре дуги идут по часовой стрелке, центры — свои у
     * каждого угла, углы кратны RSS_PI/2. */
    double r = MIN(radius, MIN(w, h) / 2.0);
    double x1 = x + w, y1 = y + h, x2 = x, y2 = y;

    cairo_new_path(cr);
    cairo_arc(cr, x1 - r, y1 - r, r, 0.0, RSS_PI / 2.0);            /* низ-право */
    cairo_line_to(cr, x2 + r, y1);
    cairo_arc(cr, x2 + r, y1 - r, r, RSS_PI / 2.0, RSS_PI);        /* низ-лево */
    cairo_line_to(cr, x2, y2 + r);
    cairo_arc(cr, x2 + r, y2 + r, r, RSS_PI, 3.0 * RSS_PI / 2.0);  /* верх-лево */
    cairo_line_to(cr, x1 - r, y2);
    cairo_arc(cr, x1 - r, y2 + r, r, 3.0 * RSS_PI / 2.0, 2.0 * RSS_PI); /* верх-право */
    cairo_close_path(cr);
}

static void rss_draw(XsPlugin *p, cairo_t *cr, int w, int h)
{
    PrivData *priv = p ? p->priv : NULL;
    PangoLayout *layout;
    PangoFontDescription *font;
    /* Форма окна применяется ВСЕГДА и ДО всей отрисовки. Это свойство
     * окна, а не рисунка: скругление держалось только на cairo_clip внутри
     * отрисовки, само окно оставалось прямоугольным, и в его углы был виден
     * рабочий стол. Раньше вызов стоял внутри if (theme_has "background"),
     * то есть не выполнялся для тем без элемента background. */
    rss_apply_shape(p, w, h);
    PangoRectangle logical;
    int text_h;
    int entry_count;
    int viewport_h;
    int header_h;
    int content_w;
    int first;
    double radius;
    const RssEntry *entry;
    int i;
    GString *all;

    if (!priv || !p->win)
        return;
    if (xs_host_api()->get_host_backdrop) {
        cairo_surface_t *host_bg = xs_host_api()->get_host_backdrop(p);
        if (host_bg) {
            cairo_save(cr);
            cairo_set_source_surface(cr, host_bg, 0, 0);
            cairo_set_operator(cr, CAIRO_OPERATOR_SOURCE);
            cairo_paint(cr);
            cairo_restore(cr);
        }
    }
    viewport_h = rss_viewport_height(p);
    header_h = rss_header_height(priv);
    content_w = MAX(20, w - 20);
    /* Тот же радиус, что и у формы окна - из rss_corner_radius(), а не
     * считаем здесь заново: два независимых расчёта разъезжаются при
     * изменении размера окна, и фон вылезает за срезанные углы. */
    radius = rss_corner_radius(w, h);
    cairo_save(cr);
    cairo_scale(cr, 1.0, 1.0);
    cairo_set_operator(cr, CAIRO_OPERATOR_OVER);
    if (g_strcmp0(priv->theme, "default") == 0) {
        cairo_set_source_rgba(cr, priv->background_color[0],
                              priv->background_color[1],
                              priv->background_color[2],
                              priv->background_color[3]);
        rss_draw_rounded(cr, 0, 0, w, h, radius);
        cairo_fill(cr);
    } else {
        /* Темы без своего рисунка (Simple) рисуем САМИ: тёмное стекло с
         * малой прозрачностью + тонкая круглая рамка.
         *
         * Без этого окно было полностью прозрачным: залить его было нечем,
         * рисунка темы нет, и мышью окно не ухватить - перетаскивать
         * приходилось за пиксель в углу.
         *
         * Рамка рисуется кодом, а НЕ элементом темы. Элемент в теме
         * растягивается по осям раздельно (сверху Y x1.0, снизу Y x3.88 для
         * окна 322x428), поэтому белый контур в background.svg Simple
         * выходил 1.5 px сверху и 5.8 px снизу - разная толщина и разные
         * угловые срезы. Здесь толщина задаётся в пикселях и одинакова со
         * всех сторон по построению.
         *
         * Прозрачность стекла - 10%: рабочий стол должен просвечивать,
         * иначе окно снова станет неразличимым. */
        cairo_set_source_rgba(cr, 0.04, 0.04, 0.05, 0.10);
        rss_draw_rounded(cr, 0, 0, w, h, radius);
        cairo_fill(cr);
        /* рамка по краю, отступ 0.5 px, чтобы не срезалась формой окна */
        cairo_set_source_rgba(cr, 1, 1, 1, 0.32);
        cairo_set_line_width(cr, 1.0);
        rss_draw_rounded(cr, 0.5, 0.5, w - 1.0, h - 1.0, radius);
        cairo_stroke(cr);
    }
    if (xs_core_theme_has(p, "background")) {
        /* Нижняя часть темы берётся только из исходного диапазона
         * y=100..200, поэтому верхний серый градиент не дублируется.
         * Обрезка — ТОТ ЖЕ скруглённый путь, что и у заливки цветом:
         * раньше здесь стоял cairo_rectangle, и рисунок темы выходил
         * за скруглённые углы квадратом — скругление работало для цвета,
         * но не для рисунка. cairo_clip берёт текущий путь как область и
         * НЕ съедает его, поэтому следующий cairo_rectangle + cairo_clip
         * просто пересекает его с полосой. */
        cairo_save(cr);
        rss_draw_rounded(cr, 0, 0, w, h, radius);
        cairo_clip(cr);
        cairo_rectangle(cr, 0, rss_header_height(priv), w,
                        h - rss_header_height(priv));
        cairo_clip(cr);
        xs_host_api()->theme_draw_full(
            p, cr, "background", 0, 2 * rss_header_height(priv) - h, w,
            2 * (h - rss_header_height(priv)));
        cairo_restore(cr);

        /* Верхняя полоса рисуется один раз в натуральном масштабе.
         * Обрезка та же скруглённая, иначе рисунок в верхних углах
         * выходил бы за скругление квадратом. */
        cairo_save(cr);
        rss_draw_rounded(cr, 0, 0, w, h, radius);
        cairo_clip(cr);
        cairo_rectangle(cr, 0, 0, w, rss_header_height(priv));
        cairo_clip(cr);
        xs_host_api()->theme_draw_full(p, cr, "background", 0, 0, w, RSS_H);
        cairo_restore(cr);
    }

    layout = pango_cairo_create_layout(cr);
    font = pango_font_description_from_string(
        priv->text_font ? priv->text_font : "Sans 9");
    pango_layout_set_font_description(layout, font);
    pango_font_description_free(font);
    pango_layout_set_width(layout, content_w * PANGO_SCALE);
    pango_layout_set_wrap(layout, PANGO_WRAP_WORD_CHAR);
    pango_layout_set_spacing(layout, 1 * PANGO_SCALE);
    entry_count = rss_display_count(priv, layout, h);
    if (priv->show_feed_name) {
        char *feed = priv->feed_name ? priv->feed_name : "RSS";
        char *heading = g_markup_escape_text(feed, -1);
        PangoFontDescription *header_font = pango_font_description_from_string(
            priv->header_font ? priv->header_font : "Sans Bold 9");
        if (!header_font)
            header_font = pango_font_description_from_string("Sans Bold 9");
        cairo_save(cr);
        cairo_rectangle(cr, 7, 7, w - 14, rss_header_height(priv));
        cairo_clip(cr);
        pango_layout_set_font_description(layout, header_font);
        pango_layout_set_markup(layout, heading, -1);
        pango_font_description_free(header_font);
        pango_layout_set_width(layout, (w - 20) * PANGO_SCALE);
        pango_layout_set_wrap(layout, PANGO_WRAP_NONE);
        pango_layout_get_pixel_extents(layout, NULL, &logical);
        {
            double header_x = 10;
            int header_text_w = MAX(1, logical.width);
            int header_area_w = MAX(1, w - 20);

            if (priv->header_align == RSS_HEADER_ALIGN_CENTER)
                header_x = 10 + (header_area_w - header_text_w) / 2.0;
            else if (priv->header_align == RSS_HEADER_ALIGN_RIGHT)
                header_x = w - 10 - header_text_w;
            cairo_set_source_rgba(cr, priv->header_color[0], priv->header_color[1],
                                  priv->header_color[2], priv->header_color[3]);
            cairo_move_to(cr, header_x, 10);
        }
        pango_cairo_show_layout(cr, layout);
        cairo_restore(cr);
        g_free(heading);
        pango_layout_set_width(layout, content_w * PANGO_SCALE);
        pango_layout_set_wrap(layout, PANGO_WRAP_WORD_CHAR);
        font = pango_font_description_from_string(
            priv->text_font ? priv->text_font : "Sans 9");
        if (!font)
            font = pango_font_description_from_string("Sans 9");
        pango_layout_set_font_description(layout, font);
        pango_font_description_free(font);
    }

    if (!priv->loading && priv->status && priv->status->len &&
        (!priv->entries || !priv->entries->len)) {
        pango_layout_set_text(layout, priv->status->str, -1);
        cairo_set_source_rgba(cr, priv->text_color[0], priv->text_color[1],
                              priv->text_color[2], priv->text_color[3]);
        cairo_move_to(cr, 10, 10);
        pango_cairo_show_layout(cr, layout);
        g_object_unref(layout);
        goto controls;
    }
    if (!priv->entries || !priv->entries->len) {
        pango_layout_set_text(layout, "Refreshing...", -1);
        cairo_set_source_rgba(cr, priv->text_color[0], priv->text_color[1],
                              priv->text_color[2], priv->text_color[3]);
        cairo_move_to(cr, 10, 10);
        pango_cairo_show_layout(cr, layout);
        g_object_unref(layout);
        goto controls;
    }
    if (!rss_display_entries(priv)) {
        g_object_unref(layout);
        goto controls;
    }
    first = CLAMP(priv->feed_number, 0,
                  (int)priv->entries->len - entry_count);
    all = g_string_new(NULL);
    for (i = 0; i < entry_count; i++) {
        entry = g_ptr_array_index(priv->entries, first + i);
        char *title = rss_entry_title_markup(priv, entry);
        char *summary = g_markup_escape_text(entry->summary ? entry->summary : "", -1);
        g_string_append_printf(all, "%s\n%s", title, summary);
        if (i + 1 < entry_count)
            g_string_append_c(all, '\n');
        g_free(title);
        g_free(summary);
    }
    pango_layout_set_markup(layout, all->str, -1);
    g_string_free(all, TRUE);
    pango_layout_get_pixel_extents(layout, NULL, &logical);
    text_h = logical.height;
    priv->content_extent = text_h;
    priv->visible_count = entry_count;
    cairo_set_source_rgba(cr, priv->text_color[0], priv->text_color[1],
                          priv->text_color[2], priv->text_color[3]);
    int content_top = header_h + 3;
    int content_bottom = h - rss_control_height(priv) - 3;

    cairo_save(cr);
    cairo_rectangle(cr, 7, content_top, w - 14,
                    MAX(24, content_bottom - content_top));
    cairo_clip(cr);
    cairo_move_to(cr, 10, content_top - logical.y - priv->scroll_px);
    pango_cairo_show_layout(cr, layout);
    cairo_restore(cr);
    g_object_unref(layout);

    if (text_h > viewport_h) {
        PangoLayout *hint = pango_cairo_create_layout(cr);
        char *more = g_strdup_printf("...%s",
                                     priv->scroll_px + viewport_h < text_h ? "(more)" : "");
        font = pango_font_description_from_string(
            priv->text_font ? priv->text_font : "Sans 8");
        pango_layout_set_font_description(hint, font);
        pango_font_description_free(font);
        pango_layout_set_text(hint, more, -1);
        g_free(more);
        cairo_set_source_rgba(cr, priv->text_color[0] * .7,
                              priv->text_color[1] * .7,
                              priv->text_color[2] * .7, priv->text_color[3]);
        {
            PangoRectangle logical;
            PangoFontMetrics *fm;
            int baseline = h - rss_control_height(priv) + 2;
            int ascent, descent;
            cairo_move_to(cr, 8, baseline);
            pango_cairo_show_layout(cr, hint);
            /* Запоминаем, где нарисован "...(more)", чтобы по клику на него
             * открыть запись.
             *
             * ВАЖНО: pango рисует текст ВВЕРХ от базовой линии, то есть
             * вверх на ascent и вниз лишь на descent (у "...(more)" это
             * пара пикселей от скобок). Раньше область клика считалась как
             * [baseline, baseline + height] - целиком ПОД базовой линией,
             * то есть по видимому тексту не попадало ни пикселя, кроме
             * хвоста скобок. Поэтому клик срабатывал только если удачно
             * целиться в пару пикселей десцендера. Теперь храним верх и
             * низ настоящего текста: [baseline - ascent, baseline + descent].
             *
             * Ширину берём из pango, а не задаём константой: длина строки
             * зависит от наличия "(more)" и от шрифта. */
            pango_layout_get_pixel_extents(hint, NULL, &logical);
            /* pango_layout_get_font_metrics() НЕ СУЩЕСТВУЕТ - такой функции
             * в Pango нет, компилятор падал. Метрики шрифта берём из
             * контекста layout: layout создана на том же cr, что и весь
             * applet, поэтому ascent/descent соответствуют видимому тексту. */
            fm = pango_context_get_metrics(pango_layout_get_context(hint),
                                           NULL, NULL);
            ascent = pango_font_metrics_get_ascent(fm) / PANGO_SCALE;
            descent = pango_font_metrics_get_descent(fm) / PANGO_SCALE;
            pango_font_metrics_unref(fm);
            priv->more_x = 8;
            priv->more_y = baseline - ascent;
            priv->more_w = logical.width;
            priv->more_h = ascent + descent;
            /* Диагностика: печатаем вычисленный прямоугольник, чтобы
             * сравнить его с реальным положением букв на снимке экрана. */
            xs_host_api()->log("clearrss: more_box x=%d y=%d w=%d h=%d "
                               "baseline=%d ascent=%d descent=%d",
                               priv->more_x, priv->more_y, priv->more_w,
                               priv->more_h, baseline, ascent, descent);
        }
        g_object_unref(hint);
        /* Сброс при прокрутке: когда всё влезло, подсказки "(more)" нет,
         * и кликать больше некуда. Ставим флаг здесь, а не в rss_scroll_by,
         * чтобы состояние всегда совпадало с тем, что реально нарисовано. */
        priv->more_shown = (priv->scroll_px + viewport_h < text_h);
        if (!priv->more_shown) {
            priv->more_x = priv->more_w = 0;
            priv->more_h = 0;
        }
    } else {
        /* Всё влезло - блок с подсказкой не выполняется вовсе. Без этого
         * сброса more_shown остался бы от прошлого кадра, и клик попадал
         * бы в невидимую область. */
        priv->more_shown = FALSE;
        priv->more_x = priv->more_y = priv->more_w = priv->more_h = 0;
    }
controls:
    /* Кнопки как в оригинале: previous page / reset / next page. */
    {
        /* Радиус КНОПКИ. Раньше эта переменная называлась radius и затеняла
         * внешний radius - радиус угла окна. Из-за этого clip формы окна в
         * rss_draw_rounded() строился радиусом кнопки (12) вместо радиуса
         * угла (17): в полосе 12..17 px фон и тень рисовались по одной
         * форме, а gdk_window_shape_combine_region срезала окно по другой.
         * Теперь имена различаются и clip берёт внешний radius. */
        int btn_r = rss_button_radius(priv);
        int cy = h - 10 - btn_r;
        int x1 = w - 90;
        int x2 = w - 58;
        int x3 = w - 26;
        /* Три круга нужно заливать ПО ОДНОМУ. cairo_arc, если есть
         * текущая точка, сначала проводит ЛИНИЮ от неё к началу дуги, а
         * cairo_fill заливает весь накопленный путь целиком. Раньше три
         * дуги шли подряд и заливались одним cairo_fill — между ними
         * появлялись две соединительные линии (1->2 и 2->3), и вокруг
         * средней кнопки возникал овал. Раньше же первая дуга получала
         * линию от (0,0) после pango_cairo_show_layout — отсюда был
         * треугольник через весь апплет. cairo_fill съедает путь, поэтому
         * каждая кнопка заливается отдельным вызовом и путь не
         * накапливается. */
        /* Тень кнопок рисуется ОТДЕЛЬНЫМИ элементами темы, а не частью
         * background.svg. В исходной теме она была продублирована три раза
         * прямо в background.svg (path2270, path3260, path3264) — одинаковые
         * круги 14x14 единицы под каждой из трёх кнопок. Проблема была не в
         * самих тенях, а в том, что background.svg масштабируется по осям
         * раздельно (X x1.61, Y x3.88 для окна 322x428), и круг превращался
         * в овал 23x62. Здесь тени вынесены в отдельные файлы и рисуются в
         * квадрате side x side, поэтому масштаб всегда равномерный.
         *
         * У правой кнопки свой элемент "button_bg" — отзеркаленная вручную
         * тень (её правили в GIMP и положили в тему), у левой и средней
         * общий "shadow". side = 2*btn_r*1.25, а не 2*btn_r: при равном
         * диаметре тень полностью уходит под кнопку и её не видно.
         * Коэффициент 1.25 даёт ореол ~3 px при шаге кнопок 32 px. */
        {
            /* Ореол шире, чем раньше: при 1.25 тень почти полностью уходила
             * под кнопку, и на чёрном фоне читалась как ровная заливка. */
            double side = 2.0 * btn_r * 1.6;
            const int btn_x[3] = { x1, x2, x3 };
            /* Отзеркаленная вручную тень button_bg рисовалась тем же
             * side = 2*btn_r*1.25, что и общая, и оказывалась почти целиком
             * скрыта под кнопкой, и наружу выходил узкий кольцевой ореол.
             *
             * ВАЖНО: сам ореол принципиально не может быть ровным. И в
             * shadow.svg, и в button_bg.png тень залита ЛИНЕЙНЫМ градиентом
             * (linearGradient3254), а не радиальным, поэтому она густая с
             * одного края и почти прозрачная с другого. Зеркаливание в GIMP
             * этого не исправляет - оно переворачивает асимметрию, но не
             * убирает её. Любой размер и любой сдвиг лишь перемещают несим-
             * метричное пятно; увеличение до 1.6 делает его заметнее, а не
             * ровнее. Чинить надо сам файл: заливка должна быть радиальной
             * и симметричной относительно центра круга. */
            /* Тени И круги кнопок обрезаются ОДНОЙ и той же формой окна.
             * Раньше тень рисовалась под обрезкой, а круги — уже после
             * cairo_restore, без неё. У правой кнопки это давало отрывной
             * кусок в скруглённом углу: тень уходила в угол и срезалась по
             * одной форме, а круг по другой, и между ними оставался
             * фрагмент, не связанный с кнопкой. Правая кнопка стоит в 26 px
             * от правого края при радиусе скругления 17, поэтому её тень
             * (радиус 15) всегда заходит в зону угла — это не баг, но
             * обрезать надо обе части одинаково. */
            cairo_save(cr);
            rss_draw_rounded(cr, 0, 0, w, h, radius); /* внешний: радиус угла окна */
            cairo_clip(cr);
            for (int i = 0; i < 3; i++) {
                /* У каждой кнопки своя тень:
                 *   0 (предыдущая страница) - shadow.svg, исходная тема
                 *   1 (сброс прокрутки)      - shadow_mid.svg, симметричная
                 *   2 (следующая страница)  - button_bg.svg, зеркальная
                 *
                 * Средняя кнопка ничего не "листает" в сторону, и раньше
                 * брала ту же shadow.svg, что и левая, поэтому её полумесяц
                 * указывал влево и кнопка читалась как копия левой. У неё
                 * собственный элемент с радиальным градиентом. */
                const char *el = (i == 0)   ? "shadow"
                                 : (i == 1) ? "shadow_mid"
                                            : "button_bg";
                /* Тема может не иметь части элементов: в Simple есть только
                 * shadow.svg, и там все три кнопки получают одну круглую
                 * тень. Отсутствующий элемент -> общая "shadow". */
                if (!xs_core_theme_has(p, el))
                    el = "shadow";
                if (xs_core_theme_has(p, el)) {
                    /* Тени в теме полупрозрачные и на почти чёрном фоне
                     * applet-а практически не читаются. Рисуем во временную
                     * группу и накладываем с усилением контраста: так тень
                     * остаётся мягкой, но становится видимой. */
                    cairo_push_group(cr);
                    xs_host_api()->theme_draw_full(
                        p, cr, el, btn_x[i] - side / 2.0, cy - side / 2.0,
                        side, side);
                    cairo_pop_group_to_source(cr);
                    cairo_paint_with_alpha(cr, 2.2);
                }
            }
            cairo_set_source_rgba(cr, 0.25, 0.25, 0.25, .35);
            for (int i = 0; i < 3; i++) {
                cairo_new_path(cr);
                cairo_arc(cr, btn_x[i], cy, btn_r, 0, 2 * RSS_PI);
                cairo_fill(cr);
            }
            cairo_restore(cr);
        }
        /* Значки рисуются ПУТЯМИ, а не шрифтовыми глифами.
         *
         * Шрифтовой "‹"/"›" зависел от начертания Sans и на 24 px читался
         * как «медиатор». Позже выяснилось, что «медиатором» на деле
         * выглядела сама кнопка вместе с тенью при увеличении: контур тени
         * неровный, и на круглом zoom это читается как заострённый лепесток.
         * Поэтому значки — снова ломаные из двух отрезков, а работа идёт
         * над тенью (отдельный элемент темы "shadow"). */
        {
            /* Уменьшены на 20%: прежний 0.30/0.42/0.62 давал шевроны,
             * которые читались крупнее самих круглых кнопок. */
            const double lw = radius * 0.24;        /* толщина штриха */
            const double ax = radius * 0.336;       /* вынос по X */
            const double ay = radius * 0.496;       /* вынос по Y */
            const int btn[3] = { x1, x2, x3 };

            cairo_set_line_width(cr, lw);
            cairo_set_line_cap(cr, CAIRO_LINE_CAP_ROUND);
            cairo_set_line_join(cr, CAIRO_LINE_JOIN_ROUND);

            /* средняя кнопка — закрашенная точка */
            cairo_set_source_rgba(cr, 1, 1, 1, .9);
            cairo_arc(cr, x2, cy, radius * 0.17, 0, 2 * RSS_PI);
            cairo_fill(cr);

            /* левый шеврон "<" и правый ">" как зеркало друг друга */
            cairo_set_source_rgba(cr, 1, 1, 1, .9);
            for (int i = 0; i < 3; i += 2) {
                double dir = (i == 0) ? -1.0 : 1.0;  /* -1: влево, +1: вправо */
                cairo_new_path(cr);
                cairo_move_to(cr, btn[i] - dir * ax, cy - ay);
                cairo_line_to(cr, btn[i] + dir * ax, cy);
                cairo_line_to(cr, btn[i] - dir * ax, cy + ay);
                cairo_stroke(cr);
            }
        }
    }
    cairo_restore(cr);
}

/* ---------- mouse ---------- */
static gboolean rss_button(XsPlugin *p, GdkEventButton *ev)
{
    PrivData *priv = p ? p->priv : NULL;
    GtkAllocation allocation;
    double x, y;

    if (!p || !p->priv || !p->win)
        return FALSE;
    gtk_widget_get_allocation(p->win, &allocation);
    x = ev->x;
    y = ev->y;
    if (ev->type == GDK_BUTTON_PRESS) {
        int radius = rss_button_radius(priv);
        int cy = allocation.height - 10 - radius;

        priv->button_pressed = 0;
        /* Клик по "...(more)" открывает запись. Проверяем ДО кнопок внизу:
         * подсказка лежит в той же нижней полосе, и без этой проверки она
         * перехватывала бы часть нажатий. */
        if (priv->more_shown && priv->more_w > 0 && priv->more_h > 0 &&
            x >= priv->more_x && x <= priv->more_x + priv->more_w &&
            y >= priv->more_y && y <= priv->more_y + priv->more_h) {
            priv->button_pressed = 4;
            xs_host_api()->log("clearrss: more hint press at %.1f,%.1f "
                               "(box %d,%d %dx%d)", x, y, priv->more_x,
                               priv->more_y, priv->more_w, priv->more_h);
            gtk_widget_queue_draw(p->win);
            return TRUE;
        }
        if (y >= cy - radius && y <= cy + radius) {
            if (x >= allocation.width - 90 - radius &&
                x <= allocation.width - 90 + radius)
                priv->button_pressed = 1;
            else if (x >= allocation.width - 58 - radius &&
                     x <= allocation.width - 58 + radius)
                priv->button_pressed = 2;
            else if (x >= allocation.width - 26 - radius &&
                     x <= allocation.width - 26 + radius)
                priv->button_pressed = 3;
            xs_host_api()->log("clearrss: button press at %.1f,%.1f -> %d",
                               x, y, priv->button_pressed);
        }
        if (priv->button_pressed)
            gtk_widget_queue_draw(p->win);
        return priv->button_pressed != 0;
    }
    if (ev->type == GDK_BUTTON_RELEASE) {
        int button = priv->button_pressed;
        priv->button_pressed = 0;
        xs_host_api()->log("clearrss: button release at %.1f,%.1f", x, y);
        if (!button)
            return FALSE;
        if (button == 4) {
            /* Открываем запись, на которую указывает прокрутка, и сбрасываем
             * прокрутку, чтобы следующий клик открывал следующую. */
            rss_open_current(p);
            priv->scroll_px = 0;
            if (p->win)
                gtk_widget_queue_draw(p->win);
        } else if (button == 1)
            rss_scroll_by(p, -170, TRUE);
        else if (button == 2) {
            priv->scroll_px = 0;
            if (p->win)
                gtk_widget_queue_draw(p->win);
        } else {
            rss_scroll_by(p, 170, TRUE);
        }
        return TRUE;
    }
    return FALSE;
}

static void rss_scroll_by(XsPlugin *p, int delta, gboolean page_when_fit)
{
    PrivData *priv = p ? p->priv : NULL;
    int max;
    int old_scroll;
    int old_feed;
    int page;
    gboolean changed;

    if (!priv)
        return;
    max = MAX(0, priv->content_extent - rss_viewport_height(p));
    if (max > 0) {
        old_scroll = priv->scroll_px;
        priv->scroll_px = CLAMP(priv->scroll_px + delta, 0, max);
        changed = priv->scroll_px != old_scroll;
        xs_host_api()->log("clearrss: scroll %d -> %d (max=%d, visible=%d)",
                           old_scroll, priv->scroll_px, max,
                           priv->visible_count);
    } else if (page_when_fit) {
        page = MAX(1, priv->visible_count);
        old_feed = priv->feed_number;
        priv->feed_number = CLAMP(old_feed + (delta > 0 ? page : -page),
                                   0, MAX(0, (priv->entries ?
                                              (int)priv->entries->len - 1 : 0)));
        changed = priv->feed_number != old_feed;
        xs_host_api()->log("clearrss: page %d -> %d (page=%d, entries=%u)",
                           old_feed, priv->feed_number, page,
                           priv->entries ? priv->entries->len : 0);
    } else {
        return;
    }
    if (changed && p->win)
        gtk_widget_queue_draw(p->win);
}

static gboolean rss_scroll(XsPlugin *p, GdkEventScroll *ev)
{
    if (ev->direction == GDK_SCROLL_UP) rss_scroll_by(p, -60, FALSE);
    else if (ev->direction == GDK_SCROLL_DOWN) rss_scroll_by(p, 60, FALSE);
    else if (ev->direction == GDK_SCROLL_SMOOTH) {
        if (ev->delta_y < 0) rss_scroll_by(p, -60, FALSE);
        else if (ev->delta_y > 0) rss_scroll_by(p, 60, FALSE);
    } else return FALSE;
    return TRUE;
}

/* ---------- menu / properties ---------- */
static void rss_menu_cmd(XsPlugin *p, const char *cmd)
{
    PrivData *priv = p ? p->priv : NULL;

    if (!priv)
        return;
    if (strcmp(cmd, "refresh") == 0) {
        priv->scroll_px = 0;
        rss_request_refresh(p, TRUE);
    } else if (strcmp(cmd, "prev_item") == 0) {
        if (priv->entries && priv->entries->len) {
            priv->feed_number = CLAMP(priv->feed_number + 1, 0,
                                      (int)priv->entries->len - 1);
            priv->scroll_px = 0;
        }
        gtk_widget_queue_draw(p->win);
    } else if (strcmp(cmd, "next_item") == 0) {
        if (priv->entries && priv->entries->len) {
            priv->feed_number = CLAMP(priv->feed_number - 1, 0,
                                      (int)priv->entries->len - 1);
            priv->scroll_px = 0;
        }
        gtk_widget_queue_draw(p->win);
    } else if (strcmp(cmd, "firefox") == 0) {
        rss_open_current(p);
    } else if (g_str_has_prefix(cmd, "feed:")) {
        g_free(priv->feed_url);
        priv->feed_url = g_strdup(cmd + 5);
        priv->generation++;
        priv->loading = FALSE;
        g_free(priv->site_url);
        priv->site_url = NULL;
        g_key_file_set_string(priv->kf, p->name, "feed_url", priv->feed_url);
        g_key_file_set_string(priv->kf, p->name, "feed_name", "RSS");
        g_free(priv->feed_name);
        priv->feed_name = g_strdup("RSS");
        rss_flush(priv);
        rss_request_refresh(p, TRUE);
    } else if (g_str_has_prefix(cmd, "theme:")) {
        g_free(priv->theme); priv->theme = g_strdup(cmd + 6);
        rss_load_theme(p, priv->theme);
        xs_host_api()->conf_set_str(priv->kf, p->name, "theme", priv->theme);
        rss_flush(priv);
        gtk_widget_queue_draw(p->win);
    } else if (strcmp(cmd, "scale-applied") == 0) {
        priv->scale = xs_host_api()->conf_dbl(priv->kf, p->name, "scale", 1.0);
        xs_host_api()->resize(p, MAX(1, (int)(200 * priv->scale)),
                              MAX(1, (int)(200 * priv->scale)));
    }
}

static void rss_menu_item(GtkWidget *menu, XsPlugin *p, const char *label,
                          const char *cmd)
{
    GtkWidget *mi = gtk_menu_item_new_with_label(label);
    g_object_set_data_full(G_OBJECT(mi), "xs-cmd", g_strdup_printf("p:%s", cmd), g_free);
    g_signal_connect(mi, "activate", G_CALLBACK(xs_core_menu_activate), p);
    gtk_menu_shell_append(GTK_MENU_SHELL(menu), mi);
}

static void rss_theme_toggled(GtkCheckMenuItem *mi, gpointer data)
{
    XsPlugin *p = data;
    const char *theme;

    if (!p || !p->priv || !gtk_check_menu_item_get_active(mi))
        return;
    theme = g_object_get_data(G_OBJECT(mi), "xs-theme");
    if (theme) {
        char *cmd = g_strdup_printf("theme:%s", theme);
        rss_menu_cmd(p, cmd);
        g_free(cmd);
    }
}

static void rss_menu(XsPlugin *p, GtkMenu *menu)
{
    GtkWidget *item, *sub, *mi;
    static const struct { const char *name; const char *url; } feeds[] = {
        {"LWN", "https://lwn.net/headlines/newrss"},
        {"Phoronix", "https://www.phoronix.com/rss.php"},
        {"BBC World", "https://feeds.bbci.co.uk/news/world/rss.xml"},
        {"NASA", "https://www.nasa.gov/feed/"},
        {NULL, NULL}
    };
    int i;

    item = gtk_menu_item_new_with_label("Feeds");
    sub = gtk_menu_new();
    for (i = 0; feeds[i].name; i++) {
        char *cmd = g_strdup_printf("feed:%s", feeds[i].url);
        mi = gtk_menu_item_new_with_label(feeds[i].name);
        g_object_set_data_full(G_OBJECT(mi), "xs-cmd", g_strdup_printf("p:%s", cmd), g_free);
        g_signal_connect(mi, "activate", G_CALLBACK(xs_core_menu_activate), p);
        gtk_menu_shell_append(GTK_MENU_SHELL(sub), mi);
        g_free(cmd);
    }
    gtk_menu_item_set_submenu(GTK_MENU_ITEM(item), sub);
    gtk_menu_shell_append(GTK_MENU_SHELL(menu), item);
    rss_menu_item(GTK_WIDGET(menu), p, "View this News", "firefox");
    rss_menu_item(GTK_WIDGET(menu), p, "Refresh", "refresh");
    rss_menu_item(GTK_WIDGET(menu), p, "Previous item", "prev_item");
    rss_menu_item(GTK_WIDGET(menu), p, "Next item", "next_item");
    item = gtk_menu_item_new_with_label("Themes");
    sub = gtk_menu_new();
    {
        GPtrArray *names = g_ptr_array_new_with_free_func(g_free);
        char *user = g_build_filename(g_get_user_config_dir(), "xscreenlets",
                                      "themes", "clearrss", NULL);
        char *project = rss_project_root_from_plugdir();
        char *system = g_build_filename("/usr/share/screenlets",
                                        "ClearRss", "themes", NULL);

        rss_add_theme_dirs(user, names);
        rss_add_theme_dirs(project, names);
        rss_add_theme_dirs(system, names);
        g_ptr_array_sort(names, (GCompareFunc)strcmp);
        for (i = 0; i < (int)names->len; i++) {
            const char *name = g_ptr_array_index(names, i);

            mi = gtk_check_menu_item_new_with_label(name);
            if (p->priv && g_strcmp0(((PrivData *)p->priv)->theme, name) == 0)
                gtk_check_menu_item_set_active(GTK_CHECK_MENU_ITEM(mi), TRUE);
            g_object_set_data_full(G_OBJECT(mi), "xs-theme", g_strdup(name), g_free);
            g_signal_connect(mi, "toggled", G_CALLBACK(rss_theme_toggled), p);
            gtk_menu_shell_append(GTK_MENU_SHELL(sub), mi);
        }
        g_ptr_array_unref(names);
        g_free(user); g_free(project); g_free(system);
    }
    gtk_menu_item_set_submenu(GTK_MENU_ITEM(item), sub);
    gtk_menu_shell_append(GTK_MENU_SHELL(menu), item);
}

static gboolean rss_feed_url_timeout(gpointer data)
{
    XsPlugin *p = data;
    PrivData *priv = p ? p->priv : NULL;

    if (!priv)
        return G_SOURCE_REMOVE;
    priv->feed_url_source = 0;
    rss_request_refresh(p, TRUE);
    return G_SOURCE_REMOVE;
}

static void rss_queue_feed_refresh(XsPlugin *p)
{
    PrivData *priv = p ? p->priv : NULL;

    if (!priv)
        return;
    if (priv->feed_url_source)
        g_source_remove(priv->feed_url_source);
    priv->feed_url_source = g_timeout_add(700, rss_feed_url_timeout, p);
}

static void rss_entry_changed(GtkEditable *edit, gpointer data)
{
    XsPlugin *p = data;
    PrivData *priv = p ? p->priv : NULL;
    const char *key = g_object_get_data(G_OBJECT(edit), "xs-key");
    const char *text = gtk_entry_get_text(GTK_ENTRY(edit));

    if (!priv || !key)
        return;
    g_key_file_set_string(priv->kf, p->name, key, text ? text : "");
    if (strcmp(key, "feed_name") == 0) {
        g_free(priv->feed_name); priv->feed_name = g_strdup(text);
    } else if (strcmp(key, "feed_url") == 0) {
        g_free(priv->feed_url);
        priv->feed_url = g_strdup(text ? text : "");
        priv->generation++;
        priv->loading = FALSE;
        g_free(priv->site_url);
        priv->site_url = NULL;
        rss_queue_feed_refresh(p);
    }
    rss_flush(priv);
    if (p->win) gtk_widget_queue_draw(p->win);
}

static void rss_bool_toggled(GtkToggleButton *btn, gpointer data)
{
    XsPlugin *p = data; PrivData *priv = p ? p->priv : NULL;
    const char *key = g_object_get_data(G_OBJECT(btn), "xs-key");
    gboolean active;
    if (!priv || !key) return;
    active = gtk_toggle_button_get_active(btn);
    g_key_file_set_boolean(priv->kf, p->name, key, active);
    if (strcmp(key, "show_feed_name") == 0)
        priv->show_feed_name = active;
    else if (strcmp(key, "show_published_time") == 0)
        priv->show_published_time = active;
    rss_flush(priv); if (p->win) gtk_widget_queue_draw(p->win);
}

static void rss_window_resize(XsPlugin *p, int width, int height)
{
    PrivData *priv = p ? p->priv : NULL;

    if (!priv || !p->win)
        return;
    priv->window_width = CLAMP(width, 100, 1200);
    priv->window_height = CLAMP(height, 80, 1200);
    xs_host_api()->resize(p, priv->window_width, priv->window_height);
    gtk_widget_queue_draw(p->win);
}

static void rss_size_changed(GtkSpinButton *spin, gpointer data)
{
    XsPlugin *p = data;
    PrivData *priv = p ? p->priv : NULL;
    const char *key = g_object_get_data(G_OBJECT(spin), "xs-key");
    int width;
    int height;

    if (!priv || !key)
        return;
    width = priv->window_width;
    height = priv->window_height;
    if (strcmp(key, "window_width") == 0)
        width = gtk_spin_button_get_value_as_int(GTK_SPIN_BUTTON(spin));
    else if (strcmp(key, "window_height") == 0)
        height = gtk_spin_button_get_value_as_int(GTK_SPIN_BUTTON(spin));
    else
        return;
    rss_window_resize(p, width, height);
    g_key_file_set_integer(priv->kf, p->name, "window_width",
                           priv->window_width);
    g_key_file_set_integer(priv->kf, p->name, "window_height",
                           priv->window_height);
    rss_flush(priv);
    priv->scroll_px = 0;
}

static void rss_news_count_changed(GtkSpinButton *spin, gpointer data)
{
    XsPlugin *p = data;
    PrivData *priv = p ? p->priv : NULL;
    if (!priv)
        return;
    priv->news_count = CLAMP(gtk_spin_button_get_value_as_int(
                                 GTK_SPIN_BUTTON(spin)), 1, RSS_MAX_ENTRIES);
    g_key_file_set_integer(priv->kf, p->name, "news_count", priv->news_count);
    rss_flush(priv);
    if (p->win)
        gtk_widget_queue_draw(p->win);
}

static void rss_auto_news_toggled(GtkToggleButton *btn, gpointer data)
{
    XsPlugin *p = data;
    PrivData *priv = p ? p->priv : NULL;
    GtkWidget *spin;

    if (!priv)
        return;
    priv->auto_news_count = gtk_toggle_button_get_active(btn);
    g_key_file_set_boolean(priv->kf, p->name, "auto_news_count",
                           priv->auto_news_count);
    spin = g_object_get_data(G_OBJECT(btn), "xs-news-count-spin");
    if (spin)
        gtk_widget_set_sensitive(spin, !priv->auto_news_count);
    rss_flush(priv);
    if (p->win)
        gtk_widget_queue_draw(p->win);
}

static void rss_int_changed(GtkSpinButton *spin, gpointer data)
{
    XsPlugin *p = data; PrivData *priv = p ? p->priv : NULL;
    const char *key = g_object_get_data(G_OBJECT(spin), "xs-key");
    int value = gtk_spin_button_get_value_as_int(GTK_SPIN_BUTTON(spin));
    if (!priv || !key) return;
    if (strcmp(key, "update_interval") == 0) {
        priv->update_minutes = CLAMP(value, 1, 60);
        g_key_file_set_integer(priv->kf, p->name, key, priv->update_minutes);
        rss_request_refresh(p, FALSE);
    }
    rss_flush(priv);
}

static void rss_color_set(GtkColorButton *btn, gpointer data)
{
    XsPlugin *p = data; PrivData *priv = p ? p->priv : NULL;
    const char *key = g_object_get_data(G_OBJECT(btn), "xs-key");
    GdkRGBA c;
    char *s;
    if (!priv || !key) return;
    gtk_color_chooser_get_rgba(GTK_COLOR_CHOOSER(btn), &c);
    if (strcmp(key, "rgba_color") == 0) {
        priv->text_color[0] = c.red; priv->text_color[1] = c.green;
        priv->text_color[2] = c.blue; priv->text_color[3] = c.alpha;
    } else if (strcmp(key, "time_color") == 0) {
        priv->time_color[0] = c.red; priv->time_color[1] = c.green;
        priv->time_color[2] = c.blue; priv->time_color[3] = c.alpha;
    } else if (strcmp(key, "header_color") == 0) {
        priv->header_color[0] = c.red; priv->header_color[1] = c.green;
        priv->header_color[2] = c.blue; priv->header_color[3] = c.alpha;
    } else {
        priv->background_color[0] = c.red; priv->background_color[1] = c.green;
        priv->background_color[2] = c.blue; priv->background_color[3] = c.alpha;
    }
    s = rss_color_string(&c);
    xs_host_api()->log("clearrss: color saved '%s'=%s (saved)", key, s);
    g_key_file_set_string(priv->kf, p->name, key, s);
    g_free(s);
    rss_flush(priv); if (p->win) gtk_widget_queue_draw(p->win);
}

static void rss_font_set(GtkFontButton *btn, gpointer data)
{
    XsPlugin *p = data; PrivData *priv = p ? p->priv : NULL;
    const char *font = gtk_font_chooser_get_font(GTK_FONT_CHOOSER(btn));
    const char *key = g_object_get_data(G_OBJECT(btn), "xs-key");
    if (!priv || !font || !key) return;
    if (strcmp(key, "time_font") == 0) {
        g_free(priv->time_font);
        priv->time_font = g_strdup(font);
        g_key_file_set_string(priv->kf, p->name, "time_font", font);
        g_key_file_remove_key(priv->kf, p->name, "time_font_size", NULL);
    } else if (strcmp(key, "header_font") == 0) {
        g_free(priv->header_font);
        priv->header_font = g_strdup(font);
        g_key_file_set_string(priv->kf, p->name, "header_font", font);
    } else {
        g_free(priv->text_font); priv->text_font = g_strdup(font);
        g_key_file_set_string(priv->kf, p->name, "font", font);
    }
    rss_flush(priv); if (p->win) gtk_widget_queue_draw(p->win);
}

static void rss_header_align_changed(GtkToggleButton *btn, gpointer data)
{
    XsPlugin *p = data;
    PrivData *priv = p ? p->priv : NULL;
    int align;
    gboolean active;

    if (!priv)
        return;
    align = GPOINTER_TO_INT(g_object_get_data(G_OBJECT(btn), "xs-align"));
    active = gtk_toggle_button_get_active(btn);
    if (active) {
        priv->header_align = align;
        g_key_file_set_integer(priv->kf, p->name, "header_align", align);
        rss_flush(priv);
        if (p->win)
            gtk_widget_queue_draw(p->win);
    }
}

static GtkWidget *rss_header_align_buttons(XsPlugin *p, PrivData *priv)
{
    GtkWidget *box = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 0);
    const char *icons[3] = {
        "format-text-align-left",
        "format-text-align-center",
        "format-text-align-right"
    };
    const int aligns[3] = {RSS_HEADER_ALIGN_LEFT, RSS_HEADER_ALIGN_CENTER,
                           RSS_HEADER_ALIGN_RIGHT};
    GtkWidget *first = NULL;
    int i;

    for (i = 0; i < 3; i++) {
        GtkWidget *button = first
            ? gtk_radio_button_new_from_widget(GTK_RADIO_BUTTON(first))
            : gtk_radio_button_new(NULL);
        GtkWidget *image = gtk_image_new_from_icon_name(icons[i],
                                                         GTK_ICON_SIZE_MENU);

        if (!first)
            first = button;
        gtk_button_set_image(GTK_BUTTON(button), image);
        gtk_button_set_always_show_image(GTK_BUTTON(button), TRUE);
        gtk_button_set_relief(GTK_BUTTON(button), GTK_RELIEF_NONE);
        gtk_widget_set_tooltip_text(button, "Align header left/center/right");
        gtk_widget_set_size_request(button, 30, 26);
        gtk_toggle_button_set_mode(GTK_TOGGLE_BUTTON(button), FALSE);
        gtk_toggle_button_set_active(
            GTK_TOGGLE_BUTTON(button), priv->header_align == aligns[i]);
        g_object_set_data(G_OBJECT(button), "xs-align", GINT_TO_POINTER(aligns[i]));
        g_signal_connect(button, "toggled", G_CALLBACK(rss_header_align_changed), p);
        gtk_box_pack_start(GTK_BOX(box), button, FALSE, FALSE, 0);
    }
    return box;
}

static GtkWidget *rss_header_font_color_row(XsPlugin *p, PrivData *priv)
{
    GtkWidget *row = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 0);
    GtkWidget *label = gtk_label_new("Header");
    GtkWidget *font;
    GtkWidget *spacer;
    GtkWidget *color;
    GdkRGBA rgba = {
        priv->header_color[0], priv->header_color[1],
        priv->header_color[2], priv->header_color[3]
    };

    gtk_widget_set_halign(label, GTK_ALIGN_START);
    gtk_widget_set_size_request(label, 180, 28);
    gtk_box_pack_start(GTK_BOX(row), label, FALSE, FALSE, 0);
    gtk_box_pack_start(GTK_BOX(row),
                       gtk_separator_new(GTK_ORIENTATION_VERTICAL),
                       FALSE, TRUE, 5);
    gtk_widget_set_size_request(row, -1, 28);
    gtk_widget_set_hexpand(row, TRUE);

    font = gtk_font_button_new();
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wdeprecated-declarations"
    if (priv->header_font)
        gtk_font_button_set_font_name(GTK_FONT_BUTTON(font), priv->header_font);
#pragma GCC diagnostic pop
    gtk_widget_set_tooltip_text(font, "Header font and size");
    gtk_widget_set_halign(font, GTK_ALIGN_START);
    gtk_box_pack_start(GTK_BOX(row), font, FALSE, TRUE, 0);
    g_object_set_data_full(G_OBJECT(font), "xs-key", g_strdup("header_font"),
                           g_free);
    g_signal_connect(font, "font-set", G_CALLBACK(rss_font_set), p);

    spacer = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 0);
    gtk_widget_set_hexpand(spacer, TRUE);
    gtk_box_pack_start(GTK_BOX(row), spacer, TRUE, TRUE, 0);

    color = gtk_color_button_new_with_rgba(&rgba);
    gtk_color_chooser_set_use_alpha(GTK_COLOR_CHOOSER(color), TRUE);
    gtk_widget_set_tooltip_text(color, "Header color");
    gtk_widget_set_halign(color, GTK_ALIGN_END);
    gtk_box_pack_start(GTK_BOX(row), color, FALSE, TRUE, 0);
    g_object_set_data_full(G_OBJECT(color), "xs-key", g_strdup("header_color"),
                           g_free);
    g_signal_connect(color, "color-set", G_CALLBACK(rss_color_set), p);
    return row;
}

static GtkWidget *rss_time_settings_row(XsPlugin *p, PrivData *priv)
{
    GtkWidget *row = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 0);
    GtkWidget *label = gtk_label_new("Header");
    GtkWidget *font;
    GtkWidget *spacer;
    GtkWidget *color;
    GdkRGBA rgba = {
        priv->time_color[0], priv->time_color[1],
        priv->time_color[2], priv->time_color[3]
    };

    gtk_widget_set_halign(label, GTK_ALIGN_START);
    gtk_widget_set_size_request(label, 180, 28);
    gtk_box_pack_start(GTK_BOX(row), label, FALSE, FALSE, 0);
    gtk_box_pack_start(GTK_BOX(row),
                       gtk_separator_new(GTK_ORIENTATION_VERTICAL),
                       FALSE, TRUE, 5);
    gtk_widget_set_size_request(row, -1, 28);
    gtk_widget_set_hexpand(row, TRUE);

    font = gtk_font_button_new();
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wdeprecated-declarations"
    if (priv->time_font)
        gtk_font_button_set_font_name(GTK_FONT_BUTTON(font), priv->time_font);
#pragma GCC diagnostic pop
    gtk_widget_set_tooltip_text(font, "Published-time font and size");
    gtk_widget_set_halign(font, GTK_ALIGN_START);
    gtk_box_pack_start(GTK_BOX(row), font, FALSE, TRUE, 0);
    g_object_set_data_full(G_OBJECT(font), "xs-key", g_strdup("time_font"),
                           g_free);
    g_signal_connect(font, "font-set", G_CALLBACK(rss_font_set), p);

    spacer = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 0);
    gtk_widget_set_hexpand(spacer, TRUE);
    gtk_box_pack_start(GTK_BOX(row), spacer, TRUE, TRUE, 0);

    color = gtk_color_button_new_with_rgba(&rgba);
    gtk_color_chooser_set_use_alpha(GTK_COLOR_CHOOSER(color), TRUE);
    gtk_widget_set_tooltip_text(color, "Published-time color");
    gtk_widget_set_halign(color, GTK_ALIGN_END);
    gtk_box_pack_start(GTK_BOX(row), color, FALSE, TRUE, 0);
    g_object_set_data_full(G_OBJECT(color), "xs-key", g_strdup("time_color"),
                           g_free);
    g_signal_connect(color, "color-set", G_CALLBACK(rss_color_set), p);
    return row;
}

static GtkWidget *rss_text_settings_row(XsPlugin *p, PrivData *priv)
{
    GtkWidget *row = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 0);
    GtkWidget *label = gtk_label_new("Text");
    GtkWidget *font;
    GtkWidget *spacer;
    GtkWidget *color;
    GdkRGBA rgba = {
        priv->text_color[0], priv->text_color[1],
        priv->text_color[2], priv->text_color[3]
    };

    gtk_widget_set_halign(label, GTK_ALIGN_START);
    gtk_widget_set_size_request(label, 180, 28);
    gtk_box_pack_start(GTK_BOX(row), label, FALSE, FALSE, 0);
    gtk_box_pack_start(GTK_BOX(row),
                       gtk_separator_new(GTK_ORIENTATION_VERTICAL),
                       FALSE, TRUE, 5);
    gtk_widget_set_size_request(row, -1, 28);
    gtk_widget_set_hexpand(row, TRUE);

    font = gtk_font_button_new();
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wdeprecated-declarations"
    if (priv->text_font)
        gtk_font_button_set_font_name(GTK_FONT_BUTTON(font), priv->text_font);
#pragma GCC diagnostic pop
    gtk_widget_set_tooltip_text(font, "Text font and size");
    gtk_widget_set_halign(font, GTK_ALIGN_START);
    gtk_box_pack_start(GTK_BOX(row), font, FALSE, TRUE, 0);
    g_object_set_data_full(G_OBJECT(font), "xs-key", g_strdup("font"),
                           g_free);
    g_signal_connect(font, "font-set", G_CALLBACK(rss_font_set), p);

    spacer = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 0);
    gtk_widget_set_hexpand(spacer, TRUE);
    gtk_box_pack_start(GTK_BOX(row), spacer, TRUE, TRUE, 0);

    color = gtk_color_button_new_with_rgba(&rgba);
    gtk_color_chooser_set_use_alpha(GTK_COLOR_CHOOSER(color), TRUE);
    gtk_widget_set_tooltip_text(color, "Text color");
    gtk_widget_set_halign(color, GTK_ALIGN_END);
    gtk_box_pack_start(GTK_BOX(row), color, FALSE, TRUE, 0);
    g_object_set_data_full(G_OBJECT(color), "xs-key", g_strdup("rgba_color"),
                           g_free);
    g_signal_connect(color, "color-set", G_CALLBACK(rss_color_set), p);
    return row;
}

static void rss_properties(XsPlugin *p, GtkNotebook *nb)
{
    PrivData *priv = p ? p->priv : NULL;
    GtkWidget *page, *w, *auto_toggle;
    if (!priv) return;
    page = gtk_box_new(GTK_ORIENTATION_VERTICAL, 5);
    gtk_container_set_border_width(GTK_CONTAINER(page), 10);
    xs_prop_add_group_header(GTK_BOX(page), "Rss-specific settings.");
    {
        GtkWidget *feed_frame = gtk_frame_new("Feed");
        GtkWidget *feed_box = gtk_box_new(GTK_ORIENTATION_VERTICAL, 4);

        gtk_frame_set_shadow_type(GTK_FRAME(feed_frame), GTK_SHADOW_IN);
        gtk_container_set_border_width(GTK_CONTAINER(feed_frame), 7);
        gtk_container_add(GTK_CONTAINER(feed_frame), feed_box);
        w = xs_prop_add_string(GTK_BOX(feed_box), "Feed name", "Feed name", priv->feed_name);
        g_object_set_data_full(G_OBJECT(w), "xs-key", g_strdup("feed_name"), g_free);
        g_signal_connect(w, "changed", G_CALLBACK(rss_entry_changed), p);
        w = xs_prop_add_string(GTK_BOX(feed_box), "Feed URL", "RSS or Atom feed URL", priv->feed_url);
        g_object_set_data_full(G_OBJECT(w), "xs-key", g_strdup("feed_url"), g_free);
        g_signal_connect(w, "changed", G_CALLBACK(rss_entry_changed), p);
        gtk_box_pack_start(GTK_BOX(feed_box),
                           gtk_separator_new(GTK_ORIENTATION_HORIZONTAL),
                           FALSE, FALSE, 3);
        w = xs_prop_add_int(GTK_BOX(feed_box), "Update interval", "Refresh interval in minutes", priv->update_minutes, 1, 60, 1);
        g_object_set_data_full(G_OBJECT(w), "xs-key", g_strdup("update_interval"), g_free);
        g_signal_connect(w, "value-changed", G_CALLBACK(rss_int_changed), p);
        w = xs_prop_add_bool(GTK_BOX(feed_box), "Auto news count", "Fit as many news as window and font allow", priv->auto_news_count);
        auto_toggle = w;
        g_signal_connect(w, "toggled", G_CALLBACK(rss_auto_news_toggled), p);
        w = xs_prop_add_int(GTK_BOX(feed_box), "News count", "Number of news when Auto is off", priv->news_count, 1, RSS_MAX_ENTRIES, 1);
        g_object_set_data_full(G_OBJECT(w), "xs-key", g_strdup("news_count"), g_free);
        g_signal_connect(w, "value-changed", G_CALLBACK(rss_news_count_changed), p);
        gtk_widget_set_sensitive(w, !priv->auto_news_count);
        g_object_set_data(G_OBJECT(w), "xs-news-count-spin", w);
        g_object_set_data(G_OBJECT(auto_toggle), "xs-news-count-spin", w);
        gtk_box_pack_start(GTK_BOX(feed_box),
                           gtk_separator_new(GTK_ORIENTATION_HORIZONTAL),
                           FALSE, FALSE, 3);
        w = xs_prop_add_int(GTK_BOX(feed_box), "Window width", "Width in pixels", priv->window_width, 100, 1200, 1);
        g_object_set_data_full(G_OBJECT(w), "xs-key", g_strdup("window_width"), g_free);
        g_signal_connect(w, "value-changed", G_CALLBACK(rss_size_changed), p);
        w = xs_prop_add_int(GTK_BOX(feed_box), "Window height", "Height in pixels", priv->window_height, 80, 1200, 1);
        g_object_set_data_full(G_OBJECT(w), "xs-key", g_strdup("window_height"), g_free);
        g_signal_connect(w, "value-changed", G_CALLBACK(rss_size_changed), p);
        gtk_box_pack_start(GTK_BOX(page), feed_frame, FALSE, FALSE, 0);
    }
    {
        GtkWidget *header_frame = gtk_frame_new("Header");
        GtkWidget *header_box = gtk_box_new(GTK_ORIENTATION_VERTICAL, 4);

        gtk_frame_set_shadow_type(GTK_FRAME(header_frame), GTK_SHADOW_IN);
        gtk_container_set_border_width(GTK_CONTAINER(header_frame), 7);
        gtk_container_add(GTK_CONTAINER(header_frame), header_box);
        w = xs_prop_add_bool(GTK_BOX(header_box), "Show feed name", "Show the feed name above the current entry", priv->show_feed_name);
        g_object_set_data_full(G_OBJECT(w), "xs-key", g_strdup("show_feed_name"), g_free);
        g_signal_connect(w, "toggled", G_CALLBACK(rss_bool_toggled), p);
        gtk_box_pack_start(GTK_BOX(header_box),
                           rss_header_font_color_row(p, priv),
                           FALSE, FALSE, 0);
        w = rss_header_align_buttons(p, priv);
        w = xs_prop_add_row(GTK_BOX(header_box), "Header align", "Header alignment", w);
        gtk_box_pack_start(GTK_BOX(page), header_frame, FALSE, FALSE, 0);
    }
    {
        GtkWidget *text_frame = gtk_frame_new("Text settings");
        GtkWidget *text_box = gtk_box_new(GTK_ORIENTATION_VERTICAL, 4);

        gtk_frame_set_shadow_type(GTK_FRAME(text_frame), GTK_SHADOW_IN);
        gtk_container_set_border_width(GTK_CONTAINER(text_frame), 7);
        gtk_container_add(GTK_CONTAINER(text_frame), text_box);
        w = xs_prop_add_bool(GTK_BOX(text_box), "Show published time", "Show HH:MM:SS before each news title", priv->show_published_time);
        g_object_set_data_full(G_OBJECT(w), "xs-key", g_strdup("show_published_time"), g_free);
        g_signal_connect(w, "toggled", G_CALLBACK(rss_bool_toggled), p);
        gtk_box_pack_start(GTK_BOX(text_box), rss_time_settings_row(p, priv),
                           FALSE, FALSE, 0);
        gtk_box_pack_start(GTK_BOX(text_box), gtk_separator_new(GTK_ORIENTATION_HORIZONTAL), FALSE, FALSE, 3);
        gtk_box_pack_start(GTK_BOX(text_box), rss_text_settings_row(p, priv),
                           FALSE, FALSE, 0);
        gtk_box_pack_start(GTK_BOX(text_box), gtk_separator_new(GTK_ORIENTATION_HORIZONTAL), FALSE, FALSE, 3);
        w = xs_prop_add_color(GTK_BOX(text_box), "Back color", "Only with the default theme", priv->background_color[0], priv->background_color[1], priv->background_color[2], priv->background_color[3]);
        g_object_set_data_full(G_OBJECT(w), "xs-key", g_strdup("background_color"), g_free);
        g_signal_connect(w, "color-set", G_CALLBACK(rss_color_set), p);
        gtk_box_pack_start(GTK_BOX(page), text_frame, FALSE, FALSE, 0);
    }
    gtk_widget_show_all(page);
    gtk_notebook_append_page(nb, page, gtk_label_new("Rss"));
}

static void rss_add_theme_dirs(const char *dir, GPtrArray *names)
{
    GDir *d = g_dir_open(dir, 0, NULL);
    const char *name;
    if (!d) return;
    while ((name = g_dir_read_name(d))) {
        char *path = g_build_filename(dir, name, NULL);
        struct stat st;
        gboolean duplicate = FALSE;
        for (gsize i = 0; names && i < names->len; i++)
            if (g_strcmp0(g_ptr_array_index(names, i), name) == 0) duplicate = TRUE;
        if (stat(path, &st) == 0 && S_ISDIR(st.st_mode) && !duplicate) {
            g_ptr_array_add(names, g_strdup(name));
        }
        g_free(path);
    }
    g_dir_close(d);
}

static void rss_fill_themes(XsPlugin *p, GtkListStore *store)
{
    GPtrArray *names = g_ptr_array_new_with_free_func(g_free);
    (void)p;
    char *user = g_build_filename(g_get_user_config_dir(), "xscreenlets", "themes", "clearrss", NULL);
    char *project = rss_project_root_from_plugdir();
    char *system = g_build_filename("/usr/share/screenlets", "ClearRss", "themes", NULL);
    rss_add_theme_dirs(user, names);
    rss_add_theme_dirs(project, names);
    rss_add_theme_dirs(system, names);
    g_ptr_array_sort(names, (GCompareFunc)strcmp);
    for (gsize i = 0; i < names->len; i++) {
        GtkTreeIter it;
        gtk_list_store_append(store, &it);
        gtk_list_store_set(store, &it, 0, g_ptr_array_index(names, i), 1, NULL, 2, NULL, 3, NULL, -1);
    }
    g_ptr_array_unref(names);
    g_free(user); g_free(project); g_free(system);
}

/* ---------- init / tick / shutdown ---------- */
static void rss_cancel_fetch(PrivData *priv)
{
    RequestSet *requests = NULL;

    if (priv && priv->refresh_source) {
        g_source_remove(priv->refresh_source);
        priv->refresh_source = 0;
    }
    if (priv && priv->feed_url_source) {
        g_source_remove(priv->feed_url_source);
        priv->feed_url_source = 0;
    }
    if (priv && priv->session)
        soup_session_abort(priv->session);
    if (priv && priv->requests) {
        requests = priv->requests;
        priv->requests = NULL;
        rss_request_set_stop(requests);
        g_mutex_lock(&requests->lock);
        for (gsize i = 0; i < requests->jobs->len; i++) {
            FetchJob *job = g_ptr_array_index(requests->jobs, i);
            if (job->cancellable)
                g_cancellable_cancel(job->cancellable);
        }
        g_mutex_unlock(&requests->lock);
    }
    rss_request_set_unref(requests);
}

static int rss_init(XsPlugin *p, GKeyFile *kf)
{
    static const gdouble tc[4] = {1, 1, 1, .9};
    static const gdouble hc[4] = {1, 1, 1, .9};
    static const gdouble bc[4] = {0, 0, 0, .8};
    PrivData *priv = g_new0(PrivData, 1);
    double scale;
    int x, y;
    int window_width;
    int window_height;
    gboolean auto_count;
    int news_count;

    p->priv = priv; priv->plugin = p; priv->kf = kf;
    priv->entries = g_ptr_array_new_with_free_func(rss_entry_free);
    priv->status = g_string_new("Refreshing...");
    priv->feed_name = xs_host_api()->conf_str(kf, p->name, "feed_name", "Digg");
    priv->feed_url = xs_host_api()->conf_str(kf, p->name, "feed_url",
                                             "https://lwn.net/headlines/newrss");
    priv->site_url = xs_host_api()->conf_str(kf, p->name, "site_url", "");
    priv->theme = xs_host_api()->conf_str(kf, p->name, "theme", "default");
    priv->text_font = xs_host_api()->conf_str(kf, p->name, "font", "Sans 9");
    priv->update_minutes = CLAMP(xs_host_api()->conf_int(kf, p->name, "update_interval", 5), 1, 60);
    priv->show_feed_name = rss_conf_bool(kf, p->name, "show_feed_name", TRUE);
    rss_read_color(kf, p->name, "rgba_color", tc, priv->text_color);
    rss_read_color(kf, p->name, "time_color", tc, priv->time_color);
    rss_read_color(kf, p->name, "header_color", hc, priv->header_color);
    rss_read_color(kf, p->name, "background_color", bc, priv->background_color);
    rss_ensure_color(kf, p->name, "rgba_color", tc);
    rss_ensure_color(kf, p->name, "time_color", tc);
    rss_ensure_color(kf, p->name, "header_color", hc);
    rss_ensure_color(kf, p->name, "background_color", bc);
    xs_host_api()->log("clearrss: config '%s' text=%g,%g,%g,%g", p->name,
                       priv->text_color[0], priv->text_color[1],
                       priv->text_color[2], priv->text_color[3]);
    priv->time_font = xs_host_api()->conf_str(kf, p->name, "time_font", "Sans Bold 9");
    priv->header_font = xs_host_api()->conf_str(kf, p->name, "header_font", "Sans Bold 9");
    if (!g_key_file_has_key(kf, p->name, "time_font", NULL))
        g_key_file_set_string(kf, p->name, "time_font", priv->time_font);
    if (!g_key_file_has_key(kf, p->name, "header_font", NULL))
        g_key_file_set_string(kf, p->name, "header_font", priv->header_font);
    if (g_key_file_has_key(kf, p->name, "time_font_size", NULL)) {
        g_key_file_remove_key(kf, p->name, "time_font_size", NULL);
    }
    scale = xs_host_api()->conf_dbl(kf, p->name, "scale", 1.0);
    priv->scale = CLAMP(scale, .2, 10.0);
    priv->opacity = xs_host_api()->conf_dbl(kf, p->name, "opacity", 1.0);
    x = xs_host_api()->conf_int(kf, p->name, "x", 80);
    y = xs_host_api()->conf_int(kf, p->name, "y", 80);
    window_width = CLAMP(xs_host_api()->conf_int(kf, p->name, "window_width",
                                                RSS_W), 100, 1200);
    window_height = CLAMP(xs_host_api()->conf_int(kf, p->name, "window_height",
                                                 RSS_H), 80, 1200);
    auto_count = rss_conf_bool(kf, p->name, "auto_news_count", FALSE);
    news_count = CLAMP(xs_host_api()->conf_int(kf, p->name, "news_count", 5),
                       1, RSS_MAX_ENTRIES);
    if (!g_key_file_has_key(kf, p->name, "window_width", NULL))
        g_key_file_set_integer(kf, p->name, "window_width", window_width);
    if (!g_key_file_has_key(kf, p->name, "window_height", NULL))
        g_key_file_set_integer(kf, p->name, "window_height", window_height);
    if (!g_key_file_has_key(kf, p->name, "auto_news_count", NULL))
        g_key_file_set_boolean(kf, p->name, "auto_news_count", auto_count);
    if (!g_key_file_has_key(kf, p->name, "news_count", NULL))
        g_key_file_set_integer(kf, p->name, "news_count", news_count);
    priv->show_published_time = rss_conf_bool(kf, p->name,
                                                "show_published_time", TRUE);
    priv->header_align = CLAMP(xs_host_api()->conf_int(kf, p->name,
                                                       "header_align", RSS_HEADER_ALIGN_LEFT),
                               RSS_HEADER_ALIGN_LEFT, RSS_HEADER_ALIGN_RIGHT);
    if (!g_key_file_has_key(kf, p->name, "header_align", NULL))
        g_key_file_set_integer(kf, p->name, "header_align", priv->header_align);
    if (!g_key_file_has_key(kf, p->name, "show_published_time", NULL))
        g_key_file_set_boolean(kf, p->name, "show_published_time",
                               priv->show_published_time);
    priv->window_width = window_width;
    priv->window_height = window_height;
    priv->auto_news_count = auto_count;
    priv->news_count = news_count;
    xs_core_plugin_conf_flush(p->name);
    p->win = xs_host_api()->make_window(p, x, y, window_width, window_height);
    if (!p->win) {
        p->host->log("clearrss: failed to create window");
        g_ptr_array_unref(priv->entries); g_string_free(priv->status, TRUE);
        g_free(priv->feed_name); g_free(priv->feed_url); g_free(priv->theme);
        g_free(priv->text_font); g_free(priv->time_font); g_free(priv->header_font);
        g_free(priv); p->priv = NULL; return -1;
    }
    xs_host_api()->set_opacity(p, CLAMP(priv->opacity, .1, 1.0));
    rss_load_theme(p, priv->theme);
    priv->requests = g_new0(RequestSet, 1);
    priv->requests->refcount = 1;
    priv->requests->jobs = g_ptr_array_new();
    g_mutex_init(&priv->requests->lock);
    priv->requests->active = TRUE;
    priv->session = soup_session_new();
    soup_session_set_timeout(priv->session, RSS_TIMEOUT_SEC);
    soup_session_set_user_agent(priv->session, "Xscreenlets-ClearRSS/0.1");
    priv->refresh_source = g_timeout_add_seconds(30, rss_refresh_timer, p);
    rss_request_refresh(p, TRUE);
    xs_host_api()->set_tick(p, 1000);
    return 0;
}

static guint rss_tick(XsPlugin *p)
{
    (void)p;
    return 1000;
}

static void rss_shutdown(XsPlugin *p)
{
    PrivData *priv = p ? p->priv : NULL;
    if (!priv) return;
    rss_cancel_fetch(priv);
    if (priv->session)
        g_object_unref(priv->session);
    if (priv->entries) g_ptr_array_unref(priv->entries);
    if (priv->status) g_string_free(priv->status, TRUE);
    g_free(priv->feed_name);
    g_free(priv->feed_url);
    g_free(priv->theme);
    g_free(priv->text_font);
    g_free(priv->time_font);
    g_free(priv->header_font);
    g_free(priv->site_url);
    g_free(priv);
    p->priv = NULL;
}

static XsPluginOps rss_ops = {
    .init = rss_init,
    .draw = rss_draw,
    .tick = rss_tick,
    .button = rss_button,
    .motion = NULL,
    .shutdown = rss_shutdown,
    .menu = rss_menu,
    .menu_cmd = rss_menu_cmd,
    .properties = rss_properties,
    .fill_themes = rss_fill_themes,
    .scroll = rss_scroll
};
static XsPluginDesc rss_desc = {
    .name = "clearrss",
    .api_version = XS_API_VERSION,
    .ops = &rss_ops,
    .desc = "Screenlet for reading RSS and Atom feeds, with scrolling and opening the selected story.",
    .author = "Helder Fraga aka Whise; C/GTK3 rewrite for Xscreenlets",
    .version = "0.2"
};
XsPluginDesc *xs_plugin_desc(void) { return &rss_desc; }
