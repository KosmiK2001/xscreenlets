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
    gdouble background_color[4];
    char *text_font;
    char *time_font;
    int time_font_size;
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
    gboolean loading;
    gboolean show_feed_name;
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
    char *argv[2];

    if (!priv)
        return;
    entry = rss_display_entries(priv) ? g_ptr_array_index(priv->entries,
                              CLAMP(priv->feed_number, 0,
                                    (int)priv->entries->len - 1)) : NULL;
    url = entry && entry->link && entry->link[0] ? entry->link :
          (priv->site_url && priv->site_url[0] ? priv->site_url : NULL);
    if (!url)
        return;
    argv[0] = (char *)"xdg-open";
    argv[1] = (char *)url;
    g_spawn_async(NULL, argv, NULL, G_SPAWN_SEARCH_PATH,
                  NULL, NULL, NULL, NULL);
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
        priv->time_font ? priv->time_font : "Sans Bold");
    if (time_desc) {
        pango_font_description_set_size(time_desc,
                                       CLAMP(priv->time_font_size, 6, 48) *
                                       PANGO_SCALE);
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
    PangoLayout *layout;
    PangoFontDescription *font;
    PangoRectangle logical;
    int text_h;
    int entry_count;
    int viewport_h;
    int header_h;
    int content_w;
    int first;
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
    cairo_save(cr);
    cairo_scale(cr, 1.0, 1.0);
    cairo_set_operator(cr, CAIRO_OPERATOR_OVER);
    if (g_strcmp0(priv->theme, "default") == 0) {
        cairo_set_source_rgba(cr, priv->background_color[0],
                              priv->background_color[1],
                              priv->background_color[2],
                              priv->background_color[3]);
        rss_draw_rounded(cr, 0, 0, w, h, MIN(MIN(17, w / 8), h / 8));
        cairo_fill(cr);
    }
    if (xs_core_theme_has(p, "background")) {
        /* Нижняя часть темы берётся только из исходного диапазона
         * y=100..200, поэтому верхний серый градиент не дублируется. */
        cairo_save(cr);
        cairo_rectangle(cr, 0, rss_header_height(priv), w,
                        h - rss_header_height(priv));
        cairo_clip(cr);
        xs_host_api()->theme_draw_full(
            p, cr, "background", 0, 2 * rss_header_height(priv) - h, w,
            2 * (h - rss_header_height(priv)));
        cairo_restore(cr);

        /* Верхняя полоса рисуется один раз в натуральном масштабе. */
        cairo_save(cr);
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
        cairo_save(cr);
        cairo_rectangle(cr, 7, 7, w - 14, rss_header_height(priv));
        cairo_clip(cr);
        pango_layout_set_markup(layout, heading, -1);
        cairo_set_source_rgba(cr, priv->text_color[0], priv->text_color[1],
                              priv->text_color[2], priv->text_color[3]);
        cairo_move_to(cr, 10, 10);
        pango_cairo_show_layout(cr, layout);
        cairo_restore(cr);
        g_free(heading);
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
        cairo_move_to(cr, 8, h - rss_control_height(priv) + 2);
        pango_cairo_show_layout(cr, hint);
        g_object_unref(hint);
    }
controls:
    /* Кнопки как в оригинале: previous page / reset / next page. */
    {
        int radius = rss_button_radius(priv);
        int cy = h - 10 - radius;
        int x1 = w - 90;
        int x2 = w - 58;
        int x3 = w - 26;
        cairo_set_line_width(cr, 1.2);
        cairo_set_source_rgba(cr, 0.25, 0.25, 0.25, .35);
        cairo_arc(cr, x1, cy, radius, 0, 2 * RSS_PI);
        cairo_arc(cr, x2, cy, radius, 0, 2 * RSS_PI);
        cairo_arc(cr, x3, cy, radius, 0, 2 * RSS_PI);
        cairo_fill(cr);
        cairo_set_source_rgba(cr, 1, 1, 1, .9);
        cairo_select_font_face(cr, "Sans", CAIRO_FONT_SLANT_NORMAL,
                               CAIRO_FONT_WEIGHT_BOLD);
        cairo_set_font_size(cr, 11 + radius);
        {
            cairo_text_extents_t ext;
            const char *glyph = "‹";
            cairo_text_extents(cr, glyph, &ext);
            cairo_move_to(cr, x1 - ext.width / 2.0,
                          cy - ext.height / 2.0 - ext.y_bearing);
            cairo_show_text(cr, glyph);
            glyph = "·";
            cairo_text_extents(cr, glyph, &ext);
            cairo_move_to(cr, x2 - ext.width / 2.0,
                          cy - ext.height / 2.0 - ext.y_bearing);
            cairo_show_text(cr, glyph);
            glyph = "›";
            cairo_text_extents(cr, glyph, &ext);
            cairo_move_to(cr, x3 - ext.width / 2.0,
                          cy - ext.height / 2.0 - ext.y_bearing);
            cairo_show_text(cr, glyph);
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
        if (button == 1)
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
    } else if (strcmp(key, "time_font_size") == 0) {
        priv->time_font_size = CLAMP(value, 6, 48);
        g_key_file_set_integer(priv->kf, p->name, key, priv->time_font_size);
        if (p->win)
            gtk_widget_queue_draw(p->win);
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
        g_free(priv->time_font); priv->time_font = g_strdup(font);
        g_key_file_set_string(priv->kf, p->name, key, font);
    } else {
        g_free(priv->text_font); priv->text_font = g_strdup(font);
        g_key_file_set_string(priv->kf, p->name, "font", font);
    }
    rss_flush(priv); if (p->win) gtk_widget_queue_draw(p->win);
}

static void rss_properties(XsPlugin *p, GtkNotebook *nb)
{
    PrivData *priv = p ? p->priv : NULL;
    GtkWidget *page, *w, *auto_toggle;
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
    w = xs_prop_add_int(GTK_BOX(page), "Window width", "Width in pixels", priv->window_width, 100, 1200, 1);
    g_object_set_data_full(G_OBJECT(w), "xs-key", g_strdup("window_width"), g_free);
    g_signal_connect(w, "value-changed", G_CALLBACK(rss_size_changed), p);
    w = xs_prop_add_int(GTK_BOX(page), "Window height", "Height in pixels", priv->window_height, 80, 1200, 1);
    g_object_set_data_full(G_OBJECT(w), "xs-key", g_strdup("window_height"), g_free);
    g_signal_connect(w, "value-changed", G_CALLBACK(rss_size_changed), p);
    w = xs_prop_add_int(GTK_BOX(page), "Time font size", "Published-time font size in points", priv->time_font_size, 6, 48, 1);
    g_object_set_data_full(G_OBJECT(w), "xs-key", g_strdup("time_font_size"), g_free);
    g_signal_connect(w, "value-changed", G_CALLBACK(rss_int_changed), p);
    w = xs_prop_add_bool(GTK_BOX(page), "Auto news count", "Fit as many news as window and font allow", priv->auto_news_count);
    auto_toggle = w;
    g_signal_connect(w, "toggled", G_CALLBACK(rss_auto_news_toggled), p);
    w = xs_prop_add_int(GTK_BOX(page), "News count", "Number of news when Auto is off", priv->news_count, 1, RSS_MAX_ENTRIES, 1);
    g_object_set_data_full(G_OBJECT(w), "xs-key", g_strdup("news_count"), g_free);
    g_signal_connect(w, "value-changed", G_CALLBACK(rss_news_count_changed), p);
    gtk_widget_set_sensitive(w, !priv->auto_news_count);
    g_object_set_data(G_OBJECT(w), "xs-news-count-spin", w);
    g_object_set_data(G_OBJECT(auto_toggle), "xs-news-count-spin", w);
    w = xs_prop_add_bool(GTK_BOX(page), "Show feed name", "Show the feed name above the current entry", priv->show_feed_name);
    g_object_set_data_full(G_OBJECT(w), "xs-key", g_strdup("show_feed_name"), g_free);
    g_signal_connect(w, "toggled", G_CALLBACK(rss_bool_toggled), p);
    w = xs_prop_add_bool(GTK_BOX(page), "Show published time", "Show HH:MM:SS before each news title", priv->show_published_time);
    g_object_set_data_full(G_OBJECT(w), "xs-key", g_strdup("show_published_time"), g_free);
    g_signal_connect(w, "toggled", G_CALLBACK(rss_bool_toggled), p);
    w = xs_prop_add_color(GTK_BOX(page), "Text color", "Default text color", priv->text_color[0], priv->text_color[1], priv->text_color[2], priv->text_color[3]);
    g_object_set_data_full(G_OBJECT(w), "xs-key", g_strdup("rgba_color"), g_free);
    g_signal_connect(w, "color-set", G_CALLBACK(rss_color_set), p);
    w = xs_prop_add_color(GTK_BOX(page), "Time color", "Published-time color", priv->time_color[0], priv->time_color[1], priv->time_color[2], priv->time_color[3]);
    g_object_set_data_full(G_OBJECT(w), "xs-key", g_strdup("time_color"), g_free);
    g_signal_connect(w, "color-set", G_CALLBACK(rss_color_set), p);
    w = xs_prop_add_color(GTK_BOX(page), "Back color", "Only with the default theme", priv->background_color[0], priv->background_color[1], priv->background_color[2], priv->background_color[3]);
    g_object_set_data_full(G_OBJECT(w), "xs-key", g_strdup("background_color"), g_free);
    g_signal_connect(w, "color-set", G_CALLBACK(rss_color_set), p);
    w = xs_prop_add_font(GTK_BOX(page), "Text Font", "Text font", priv->text_font);
    g_object_set_data_full(G_OBJECT(w), "xs-key", g_strdup("font"), g_free);
    g_signal_connect(w, "font-set", G_CALLBACK(rss_font_set), p);
    w = xs_prop_add_font(GTK_BOX(page), "Time Font", "Published-time font", priv->time_font);
    g_object_set_data_full(G_OBJECT(w), "xs-key", g_strdup("time_font"), g_free);
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
    rss_read_color(kf, p->name, "background_color", bc, priv->background_color);
    xs_host_api()->log("clearrss: config '%s' text=%g,%g,%g,%g", p->name,
                       priv->text_color[0], priv->text_color[1],
                       priv->text_color[2], priv->text_color[3]);
    priv->time_font = xs_host_api()->conf_str(kf, p->name, "time_font", "Sans Bold");
    priv->time_font_size = CLAMP(xs_host_api()->conf_int(kf, p->name, "time_font_size", 9), 6, 48);
    if (!g_key_file_has_key(kf, p->name, "time_font", NULL))
        g_key_file_set_string(kf, p->name, "time_font", priv->time_font);
    if (!g_key_file_has_key(kf, p->name, "time_font_size", NULL))
        g_key_file_set_integer(kf, p->name, "time_font_size", priv->time_font_size);
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
        g_free(priv->text_font); g_free(priv->time_font);
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
