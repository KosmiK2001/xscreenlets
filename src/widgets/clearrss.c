/* clearrss.c — Xscreenlets C/GTK3 plugin «clearrss», замена ClearRss 0.1.
 *
 * RSS/Atom reader с одной выбранной лентой, ручным обновлением и
 * периодическим refresh. Сетевой I/O выполняется асинхронно через
 * libsoup 3, XML разбирается libxml2 в worker thread; GTK/Pango/Cairo
 * используются исключительно в главном потоке. */
#include <gtk/gtk.h>
#include <glib.h>
#include <glib/gstdio.h>
#include <pango/pangocairo.h>
#include <libsoup/soup.h>
#include <libxml/parser.h>
#include <libxml/tree.h>
#include <string.h>
#include <math.h>
#include <sys/stat.h>
#include "xs_api.h"
#include "common.h"

#define RSS_W 200
#define RSS_H 200
#define RSS_MAX_ENTRIES 100
#define RSS_MAX_BYTES (8U * 1024U * 1024U)
#define RSS_TIMEOUT_SEC 20
#define RSS_PI 3.14159265358979323846

typedef struct {
    char *title;
    char *summary;
    char *link;
} RssEntry;

typedef struct _RequestSet RequestSet;

typedef struct {
    XsPlugin *plugin;
    GKeyFile *kf;
    gdouble scale;
    gdouble opacity;
    gdouble text_color[4];
    gdouble background_color[4];
    char *text_font;
    char *theme;
    char *feed_name;
    char *feed_url;
    int update_minutes;
    int feed_number;
    int scroll_px;
    int content_extent;
    int content_offset;
    int button_pressed;
    gboolean loading;
    gboolean show_feed_name;
    GPtrArray *entries;
    GString *status;
    char *site_url;
    RequestSet *requests;
    SoupSession *session;
    guint refresh_source;
    gint64 last_refresh_us;
    guint64 generation;
} PrivData;

typedef struct _RequestSet {
    gint refcount;
    gint alive;
    GPtrArray *jobs;
    GThreadPool *pool;
    GMutex lock;
    gboolean active;
} RequestSet;

typedef struct {
    RequestSet *set;
    GBytes *bytes;
    SoupMessage *message;
    SoupSession *session;
    char *url;
    char *instance_name;
    guint idle_source;
    guint64 generation;
    gboolean error;
} FetchJob;

static const RssEntry *rss_current_entry(PrivData *priv)
{
    gint index;

    if (!priv || !priv->entries || !priv->entries->len)
        return NULL;
    index = CLAMP(priv->feed_number, 0, (gint)priv->entries->len - 1);
    return g_ptr_array_index(priv->entries, index);
}

static void rss_open_current(XsPlugin *p)
{
    PrivData *priv = p ? p->priv : NULL;
    char *argv[2];

    if (!priv || !priv->site_url || !priv->site_url[0])
        return;
    argv[0] = (char *)"xdg-open";
    argv[1] = priv->site_url;
    g_spawn_async(NULL, argv, NULL, G_SPAWN_SEARCH_PATH,
                  NULL, NULL, NULL, NULL);
}

static void rss_job_free(FetchJob *job);
static void rss_request_set_stop(RequestSet *set);
static gboolean rss_finish_fetch(gpointer data);
static void rss_request_refresh(XsPlugin *p, gboolean force);
static void rss_scroll_by(XsPlugin *p, int delta);
static const RssEntry *rss_current_entry(PrivData *priv);
static void rss_add_theme_dirs(const char *dir, GPtrArray *names);
static void rss_open_current(XsPlugin *p);

/* ---------- theme ---------- */
static char *rss_project_root_from_plugdir(void)
{
    const char *plugdir = xs_core_plugdir();

    if (!plugdir || !plugdir[0])
        return NULL;
    return g_build_filename(g_path_get_dirname(plugdir), "themes", "clearrss", NULL);
}

static gboolean rss_load_theme(XsPlugin *p, const char *name)
{
    char *dir;
    char *user;
    gboolean ok;

    if (!p || !name || !name[0])
        return FALSE;
    user = g_build_filename(g_get_user_config_dir(), "xscreenlets",
                            "themes", "clearrss", name, NULL);
    ok = xs_host_api()->theme_load(p, user);
    g_free(user);
    if (ok)
        return TRUE;
    dir = rss_project_root_from_plugdir();
    if (dir) {
        char *project = g_build_filename(dir, name, NULL);
        ok = xs_host_api()->theme_load(p, project);
        g_free(project);
    }
    g_free(dir);
    if (!ok) {
        dir = g_build_filename("/usr/share/screenlets", "ClearRss",
                               "themes", name, NULL);
        ok = xs_host_api()->theme_load(p, dir);
        g_free(dir);
    }
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

static void rss_read_color(GKeyFile *kf, const char *sec, const char *key,
                           const gdouble def[4], gdouble out[4])
{
    char *s = xs_host_api()->conf_str(kf, sec, key, NULL);

    out[0] = def[0]; out[1] = def[1]; out[2] = def[2]; out[3] = def[3];
    if (!s || sscanf(s, "%lf,%lf,%lf,%lf", &out[0], &out[1], &out[2],
                     &out[3]) != 4) {
        out[0] = def[0]; out[1] = def[1]; out[2] = def[2]; out[3] = def[3];
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
    for (node = parent->children; node; node = node->next)
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
    return NULL;
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
            entry->summary = rss_plain_text(rss_child_text(node, "summary"));
            if (!entry->summary || !entry->summary[0]) {
                g_free(entry->summary);
                entry->summary = rss_plain_text(rss_child_text(node, "content"));
            }
            entry->link = rss_node_link(node);
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
                entry->summary = rss_plain_text(rss_child_text(node, "description"));
                entry->link = rss_node_link(node);
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
    if (set->pool) {
        g_thread_pool_set_max_threads(set->pool, 1, NULL);
        g_thread_pool_free(g_steal_pointer(&set->pool), FALSE, TRUE);
    }
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
    g_object_unref(job->message);
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

static void rss_fetch_threaded(gpointer data, gpointer user_data)
{
    FetchJob *job = user_data;
    GError *error = NULL;
    GBytes *bytes;

    (void)data;

    bytes = soup_session_send_and_read(job->session, job->message, NULL,
                                       &error);
    g_clear_object(&job->session);
    if (bytes && soup_message_get_status(job->message) >= 400) {
        g_bytes_unref(bytes);
        {
        char *msg = g_strdup_printf("HTTP error %u\n",
                                    soup_message_get_status(job->message));
        job->bytes = g_bytes_new_take(msg, strlen(msg));
        }
        job->error = TRUE;
    } else if (bytes) {
        if (g_bytes_get_size(bytes) > RSS_MAX_BYTES) {
            job->error = TRUE;
            job->bytes = g_bytes_new_static("Feed response is too large\n",
                                           sizeof("Feed response is too large\n") - 1);
            g_bytes_unref(bytes);
        } else {
            job->bytes = g_bytes_ref(bytes);
            g_bytes_unref(bytes);
        }
    } else {
        GString *text = g_string_new(error ? error->message :
                                     "network request failed");
        gsize len = text->len + 1;
        char *data = g_string_free(text, FALSE);

        g_clear_error(&error);
        job->bytes = g_bytes_new_take(data, len);
        job->error = TRUE;
    }
    job->idle_source = g_idle_add(rss_finish_fetch, job);
}

static void rss_request_refresh(XsPlugin *p, gboolean force)
{
    PrivData *priv = p ? p->priv : NULL;
    FetchJob *job;
    GError *error = NULL;
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
    soup_message_headers_append(soup_message_get_request_headers(msg),
                                 "Accept",
                                 "application/rss+xml, application/atom+xml, application/xml;q=0.9, text/xml;q=0.8, */*;q=0.1");
    job = g_new0(FetchJob, 1);
    job->set = rss_request_set_ref(priv->requests);
    job->message = msg;
    job->session = g_object_ref(priv->session);
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
    if (!g_thread_pool_push(priv->requests->pool, job, &error)) {
        g_clear_error(&error);
        g_mutex_lock(&priv->requests->lock);
        g_ptr_array_remove_fast(priv->requests->jobs, job);
        priv->requests->alive--;
        g_mutex_unlock(&priv->requests->lock);
        priv->loading = FALSE;
        rss_job_free(job);
    }
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
static void rss_draw_rounded(cairo_t *cr, double x, double y, double w,
                             double h, double radius)
{
    double d = M_PI * radius / 180.0;
    double x1 = x + w, y1 = y + h, x2 = x, y2 = y;

    cairo_new_sub_path(cr);
    cairo_arc(cr, x1 - d, y1 - d, d, 0, 2 * d);
    cairo_line_to(cr, x2 + d, y1);
    cairo_arc(cr, x2 + d, y2 + d, d, 2 * d, d);
    cairo_line_to(cr, x1, y2 + d);
    cairo_arc(cr, x1 - d, y2 + d, d, 3 * d, d);
    cairo_line_to(cr, x2 + d, y2);
    cairo_arc(cr, x2 + d, y2 + d, d, 0, d);
    cairo_close_path(cr);
}

static void rss_draw(XsPlugin *p, cairo_t *cr, int w, int h)
{
    PrivData *priv = p ? p->priv : NULL;
    const RssEntry *entry;
    PangoLayout *layout;
    PangoFontDescription *font;
    PangoRectangle logical;
    double k;
    double ox, oy;
    int text_w, text_h;
    int entry_count;

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
    k = MIN(w / 200.0, h / 200.0);
    ox = (w - 200.0 * k) / 2.0;
    oy = (h - 200.0 * k) / 2.0;
    cairo_save(cr);
    cairo_translate(cr, ox, oy);
    cairo_scale(cr, k, k);
    cairo_set_operator(cr, CAIRO_OPERATOR_OVER);
    if (g_strcmp0(priv->theme, "default") == 0) {
        cairo_set_source_rgba(cr, priv->background_color[0],
                              priv->background_color[1],
                              priv->background_color[2],
                              priv->background_color[3]);
        rss_draw_rounded(cr, 0, 0, 200, 200, 17);
        cairo_fill(cr);
    }
    if (xs_core_theme_has(p, "background"))
        xs_host_api()->theme_draw_full(p, cr, "background", 0, 0, 200, 200);

    layout = pango_cairo_create_layout(cr);
    font = pango_font_description_from_string(
        priv->text_font ? priv->text_font : "Sans 9");
    pango_layout_set_font_description(layout, font);
    pango_font_description_free(font);
    pango_layout_set_width(layout, (int)(190 * PANGO_SCALE));
    pango_layout_set_wrap(layout, PANGO_WRAP_WORD_CHAR);
    pango_layout_set_spacing(layout, 1 * PANGO_SCALE);

    if (!priv->loading && priv->status && priv->status->len &&
        (!priv->entries || !priv->entries->len)) {
        pango_layout_set_text(layout, priv->status->str, -1);
        cairo_set_source_rgba(cr, priv->text_color[0], priv->text_color[1],
                              priv->text_color[2], priv->text_color[3]);
        cairo_move_to(cr, 10, 10);
        pango_cairo_show_layout(cr, layout);
        goto controls;
    }
    if (!priv->entries || !priv->entries->len) {
        pango_layout_set_text(layout, "Refreshing...", -1);
        cairo_set_source_rgba(cr, priv->text_color[0], priv->text_color[1],
                              priv->text_color[2], priv->text_color[3]);
        cairo_move_to(cr, 10, 10);
        pango_cairo_show_layout(cr, layout);
        goto controls;
    }
    entry = rss_current_entry(priv);
    if (!entry)
        goto controls;
    {
        char *feed = priv->feed_name ? priv->feed_name : "RSS";
        char *heading = priv->show_feed_name ?
                        g_markup_escape_text(feed, -1) : g_strdup("");
        char *title = g_markup_escape_text(entry->title ? entry->title : "", -1);
        char *summary = g_markup_escape_text(entry->summary ? entry->summary : "", -1);
        char *body;

        if (priv->show_feed_name)
            body = g_strdup_printf("<b>%s</b>\n\n%s\n\n%s",
                                    heading, title, summary);
        else
            body = g_strdup_printf("%s\n\n%s", title, summary);
        pango_layout_set_markup(layout, body, -1);
        g_free(body);
        g_free(summary);
        g_free(title);
        g_free(heading);
    }
    pango_layout_get_pixel_extents(layout, NULL, &logical);
    text_w = logical.width;
    text_h = logical.height;
    priv->content_extent = text_h;
    cairo_set_source_rgba(cr, priv->text_color[0], priv->text_color[1],
                          priv->text_color[2], priv->text_color[3]);
    cairo_save(cr);
    cairo_rectangle(cr, 7, 7, 186, 174);
    cairo_clip(cr);
    cairo_move_to(cr, 10, 10 - priv->scroll_px);
    pango_cairo_show_layout(cr, layout);
    cairo_restore(cr);
    g_object_unref(layout);

    if (text_h > 174) {
        PangoLayout *hint = pango_cairo_create_layout(cr);
        char *more = g_strdup_printf("...%s",
                                     priv->scroll_px + 174 < text_h ? "(more)" : "");
        font = pango_font_description_from_string(
            priv->text_font ? priv->text_font : "Sans 8");
        pango_layout_set_font_description(hint, font);
        pango_font_description_free(font);
        pango_layout_set_text(hint, more, -1);
        g_free(more);
        cairo_set_source_rgba(cr, priv->text_color[0] * .7,
                              priv->text_color[1] * .7,
                              priv->text_color[2] * .7, priv->text_color[3]);
        cairo_move_to(cr, 8, 184);
        pango_cairo_show_layout(cr, hint);
        g_object_unref(hint);
    }
controls:
    /* Кнопки как в оригинале: previous page / reset / next page. */
    cairo_set_line_width(cr, 1.2);
    cairo_set_source_rgba(cr, 0.25, 0.25, 0.25, .35);
    cairo_arc(cr, 142, 190, 6, 0, 2 * RSS_PI);
    cairo_arc(cr, 162, 190, 6, 0, 2 * RSS_PI);
    cairo_arc(cr, 182, 190, 6, 0, 2 * RSS_PI);
    cairo_fill(cr);
    cairo_set_source_rgba(cr, 1, 1, 1, .9);
    cairo_select_font_face(cr, "Sans", CAIRO_FONT_SLANT_NORMAL,
                           CAIRO_FONT_WEIGHT_BOLD);
    cairo_set_font_size(cr, 11);
    cairo_move_to(cr, 138, 194); cairo_show_text(cr, "‹");
    cairo_move_to(cr, 158, 194); cairo_show_text(cr, "·");
    cairo_move_to(cr, 178, 194); cairo_show_text(cr, "›");
    cairo_restore(cr);
    (void)text_w;
    (void)entry_count;
    (void)RSS_W;
    (void)RSS_H;
    (void)RSS_PI;
}

/* ---------- mouse ---------- */
static gboolean rss_button(XsPlugin *p, GdkEventButton *ev)
{
    PrivData *priv = p ? p->priv : NULL;
    GtkAllocation allocation;
    double x, y, k;

    if (!p || !p->priv || !p->win)
        return FALSE;
    gtk_widget_get_allocation(p->win, &allocation);
    k = MIN((double)allocation.width / 200.0,
            (double)allocation.height / 200.0);
    if (k <= 0.0)
        return FALSE;
    x = (ev->x - (allocation.width - 200.0 * k) / 2.0) / k;
    y = (ev->y - (allocation.height - 200.0 * k) / 2.0) / k;
    if (ev->type == GDK_BUTTON_PRESS) {
        priv->button_pressed = 0;
        if (y >= 183.0 && y <= 196.7) {
            if (x >= 135.0 && x <= 149.0)
                priv->button_pressed = 1;
            else if (x >= 155.0 && x <= 168.0)
                priv->button_pressed = 2;
            else if (x >= 174.0 && x <= 187.0)
                priv->button_pressed = 3;
        }
        if (priv->button_pressed)
            gtk_widget_queue_draw(p->win);
        return priv->button_pressed != 0;
    }
    if (ev->type == GDK_BUTTON_RELEASE) {
        int button = priv->button_pressed;
        priv->button_pressed = 0;
        if (!button)
            return FALSE;
        if (button == 1)
            rss_scroll_by(p, -170);
        else if (button == 2) {
            priv->scroll_px = 0;
            if (p->win)
                gtk_widget_queue_draw(p->win);
        } else {
            rss_scroll_by(p, 170);
        }
        return TRUE;
    }
    return FALSE;
}

static void rss_scroll_by(XsPlugin *p, int delta)
{
    PrivData *priv = p ? p->priv : NULL;
    int max;

    if (!priv)
        return;
    max = MAX(0, priv->content_extent - 174);
    priv->scroll_px = CLAMP(priv->scroll_px + delta, 0, max);
    if (p->win)
        gtk_widget_queue_draw(p->win);
}

static gboolean rss_scroll(XsPlugin *p, GdkEventScroll *ev)
{
    if (ev->direction == GDK_SCROLL_UP) rss_scroll_by(p, -60);
    else if (ev->direction == GDK_SCROLL_DOWN) rss_scroll_by(p, 60);
    else if (ev->direction == GDK_SCROLL_SMOOTH) {
        if (ev->delta_y < 0) rss_scroll_by(p, -60);
        else if (ev->delta_y > 0) rss_scroll_by(p, 60);
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

    rss_menu_item(GTK_WIDGET(menu), p, "View this News", "firefox");
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
        rss_request_refresh(p, TRUE);
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
    if (strcmp(key, "show_feed_name") == 0) priv->show_feed_name = active;
    rss_flush(priv); if (p->win) gtk_widget_queue_draw(p->win);
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
    } else {
        priv->background_color[0] = c.red; priv->background_color[1] = c.green;
        priv->background_color[2] = c.blue; priv->background_color[3] = c.alpha;
    }
    s = g_strdup_printf("%g,%g,%g,%g", c.red, c.green, c.blue, c.alpha);
    g_key_file_set_string(priv->kf, p->name, key, s); g_free(s);
    rss_flush(priv); if (p->win) gtk_widget_queue_draw(p->win);
}

static void rss_font_set(GtkFontButton *btn, gpointer data)
{
    XsPlugin *p = data; PrivData *priv = p ? p->priv : NULL;
    const char *font = gtk_font_chooser_get_font(GTK_FONT_CHOOSER(btn));
    if (!priv || !font) return;
    g_free(priv->text_font); priv->text_font = g_strdup(font);
    g_key_file_set_string(priv->kf, p->name, "font", font);
    rss_flush(priv); if (p->win) gtk_widget_queue_draw(p->win);
}

static void rss_properties(XsPlugin *p, GtkNotebook *nb)
{
    PrivData *priv = p ? p->priv : NULL;
    GtkWidget *page, *w;
    if (!priv) return;
    page = gtk_box_new(GTK_ORIENTATION_VERTICAL, 5);
    gtk_container_set_border_width(GTK_CONTAINER(page), 10);
    xs_prop_add_group_header(GTK_BOX(page), "Rss-specific settings.");
    w = xs_prop_add_string(GTK_BOX(page), "Feed name", "Feed name", priv->feed_name);
    g_object_set_data_full(G_OBJECT(w), "xs-key", g_strdup("feed_name"), g_free);
    g_signal_connect(w, "changed", G_CALLBACK(rss_entry_changed), p);
    w = xs_prop_add_string(GTK_BOX(page), "Feed URL", "RSS or Atom feed URL", priv->feed_url);
    g_object_set_data_full(G_OBJECT(w), "xs-key", g_strdup("feed_url"), g_free);
    g_signal_connect(w, "changed", G_CALLBACK(rss_entry_changed), p);
    w = xs_prop_add_int(GTK_BOX(page), "Update interval", "Refresh interval in minutes", priv->update_minutes, 1, 60, 1);
    g_object_set_data_full(G_OBJECT(w), "xs-key", g_strdup("update_interval"), g_free);
    g_signal_connect(w, "value-changed", G_CALLBACK(rss_int_changed), p);
    w = xs_prop_add_bool(GTK_BOX(page), "Show feed name", "Show the feed name above the current entry", priv->show_feed_name);
    g_object_set_data_full(G_OBJECT(w), "xs-key", g_strdup("show_feed_name"), g_free);
    g_signal_connect(w, "toggled", G_CALLBACK(rss_bool_toggled), p);
    w = xs_prop_add_color(GTK_BOX(page), "Text color", "Default text color", priv->text_color[0], priv->text_color[1], priv->text_color[2], priv->text_color[3]);
    g_object_set_data_full(G_OBJECT(w), "xs-key", g_strdup("rgba_color"), g_free);
    g_signal_connect(w, "color-set", G_CALLBACK(rss_color_set), p);
    w = xs_prop_add_color(GTK_BOX(page), "Back color", "Only with the default theme", priv->background_color[0], priv->background_color[1], priv->background_color[2], priv->background_color[3]);
    g_object_set_data_full(G_OBJECT(w), "xs-key", g_strdup("background_color"), g_free);
    g_signal_connect(w, "color-set", G_CALLBACK(rss_color_set), p);
    w = xs_prop_add_font(GTK_BOX(page), "Text Font", "Text font", priv->text_font);
    g_signal_connect(w, "font-set", G_CALLBACK(rss_font_set), p);
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
    if (priv && priv->session)
        soup_session_abort(priv->session);
    if (priv && priv->requests) {
        requests = priv->requests;
        priv->requests = NULL;
        rss_request_set_stop(requests);
    }
    rss_request_set_unref(requests);
}

static int rss_init(XsPlugin *p, GKeyFile *kf)
{
    static const gdouble tc[4] = {1, 1, 1, .9};
    static const gdouble bc[4] = {0, 0, 0, .8};
    PrivData *priv = g_new0(PrivData, 1);
    double scale;
    int x, y;

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
    rss_read_color(kf, p->name, "background_color", bc, priv->background_color);
    scale = xs_host_api()->conf_dbl(kf, p->name, "scale", 1.0);
    priv->scale = CLAMP(scale, .2, 10.0);
    priv->opacity = xs_host_api()->conf_dbl(kf, p->name, "opacity", 1.0);
    x = xs_host_api()->conf_int(kf, p->name, "x", 80);
    y = xs_host_api()->conf_int(kf, p->name, "y", 80);
    p->win = xs_host_api()->make_window(p, x, y, (int)(200 * priv->scale), (int)(200 * priv->scale));
    if (!p->win) {
        p->host->log("clearrss: failed to create window");
        g_ptr_array_unref(priv->entries); g_string_free(priv->status, TRUE);
        g_free(priv->feed_name); g_free(priv->feed_url); g_free(priv->theme); g_free(priv->text_font);
        g_free(priv); p->priv = NULL; return -1;
    }
    xs_host_api()->set_opacity(p, CLAMP(priv->opacity, .1, 1.0));
    rss_load_theme(p, priv->theme);
    priv->requests = g_new0(RequestSet, 1);
    priv->requests->refcount = 1;
    priv->requests->jobs = g_ptr_array_new();
    g_mutex_init(&priv->requests->lock);
    priv->requests->active = TRUE;
    priv->requests->pool = g_thread_pool_new(rss_fetch_threaded,
                                              "xs-clearrss", 2, FALSE, NULL);
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
