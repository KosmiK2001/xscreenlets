/* clearweather.c — апплет погоды.
 *
 * Два независимых источника (open-meteo, wttr.in) и переключаемая
 * транспортировка: напрямую или через прокси. И то и другое выбирается
 * в Properties. В режиме «авто» applet сам перебирает варианты и
 * запоминает тот, который сработал, чтобы не тратить таймаут на
 * заведомо закрытый путь при каждом обновлении.
 *
 * Зачем вообще прокси: wttr.in и api.open-meteo.com с этой машины
 * напрямую не отвечают (curl: таймаут), а через локальный прокси
 * отвечают за ~0.2 с. В другой сети может быть наоборот, поэтому
 * режим выбирается руками, а не зашивается.
 *
 * libsoup-3.0 + json-glib. Всё общение с сетью асинхронное; чтение тела
 * ответа уходит в поток через GTask, разбор — обратно в main loop.
 *
 * ВАЖНО про локаль: у демона LC_ALL=ru_RU.UTF-8, где десятичный
 * разделитель — запятая. Все числа в конфиг пишутся через
 * g_ascii_formatd, цвета разбираются руками (cw_color_read).
 */

#include "xs_api.h"
#include "common.h"

#include <libsoup/soup.h>
#include <json-glib/json-glib.h>
#include <string.h>
#include <math.h>

/* ------------------------------------------------------------------ */
/* константы                                                           */
/* ------------------------------------------------------------------ */

#define CW_DEFAULT_W        320
#define CW_DEFAULT_H        180
#define CW_MARGIN           8.0
#define CW_GAP              4.0
#define CW_MAX_BYTES        (2U * 1024U * 1024U)
#define CW_TIMEOUT_SEC      10
#define CW_TIMEOUT_DIRECT   6
#define CW_DEFAULT_CITY     "Симферополь"
#define CW_DEFAULT_PROXY    "http://127.0.0.1:10809"
#define CW_REFRESH_DEFAULT  15
#define CW_HOURS_DEFAULT    8
#define CW_OM_STEP          3          /* показывать каждый 3-й час */
#define CW_MAX_ATTEMPTS     6

/* источники */
enum { CW_SRC_AUTO = 0, CW_SRC_OPENMETEO, CW_SRC_WTTR };
/* транспорт */
enum { CW_TR_DIRECT = 0, CW_TR_PROXY };
/* режим прокси */
enum { CW_PM_AUTO = 0, CW_PM_DIRECT, CW_PM_PROXY };
/* единицы */
enum { CW_UNITS_METRIC = 0, CW_UNITS_IMPERIAL };

/* Общая шкала погоды. У open-meteo коды WMO, у wttr.in свои (WW),
 * поэтому обе таблицы приводим к одному перечислению. */
enum {
    CW_W_CLEAR, CW_W_PARTLY, CW_W_CLOUDY, CW_W_FOG,
    CW_W_DRIZZLE, CW_W_RAIN, CW_W_SNOW, CW_W_SHOWERS, CW_W_THUNDER, CW_W_N
};

/* ------------------------------------------------------------------ */
/* типы                                                                */
/* ------------------------------------------------------------------ */

typedef struct {
    char   *label;      /* "15" */
    double  temp;
    int     kind;
} CwHour;

typedef struct {
    char   *place;
    char   *desc;
    double  temp, feels, humidity, wind;
    int     kind;
    GArray *hours;      /* CwHour */
} CwWeather;

typedef struct { double r, g, b, a; } CwColor;

typedef struct {
    int       source;     /* CW_SRC_* */
    int       transport;  /* CW_TR_* */
    gboolean geo;         /* TRUE = шаг геокодинга, а не прогноз */
} CwAttempt;

typedef struct {
    XsPlugin *plugin;
    GKeyFile *kf;
    guint     timer_id;
    guint     generation;
    gboolean  loading;
    gboolean  alive;

    CwWeather *weather;
    char      *status;

    int         source;       /* CW_SRC_* */
    int         proxy_mode;   /* CW_PM_* */
    int         units;
    char       *proxy_url;
    char       *city;
    /* Запасное написание того же места на другом языке. Агрегаторы
     * понимают не все языки: open-meteo находит «Симферополь», но не
     * находит «Symferopol». Если основное имя не сработало, пробуем
     * это. Пустая строка — просто выключено. */
    char       *city_alt;
    int         alt_active;   /* TRUE = сейчас ищем по city_alt */
    gboolean    city_resolved;
    double      lat, lon;

    int         prefer_tr;    /* транспорт, который сработал в прошлый раз */

    CwAttempt   attempts[CW_MAX_ATTEMPTS];
    int         n_attempts;
    int         cur_attempt;
    int         cur_transport;

    guint       refresh_min;
    int         hours_shown;

    int    width, height;
    char  *city_font, *temp_font, *desc_font, *hour_font;
    CwColor city_color, temp_color, desc_color, hour_color, bg_color;
} CwPriv;

static void cw_properties(XsPlugin *p, GtkNotebook *nb);
static void cw_start(XsPlugin *p);
static const char *cw_query(const CwPriv *priv);

static void cw_log(XsPlugin *p, const char *fmt, ...)
{
    va_list ap;
    char *s;

    if (!p || !p->host)
        return;
    va_start(ap, fmt);
    s = g_strdup_vprintf(fmt, ap);
    va_end(ap);
    p->host->log("clearweather: %s", s);
    g_free(s);
}

/* ------------------------------------------------------------------ */
/* описание погоды                                                     */
/* ------------------------------------------------------------------ */

/* wttr.in в формате j1 отдаёт ЧИСЛА СТРОКАМИ: "temp_C": "16",
 * "weatherCode": "116". json_object_get_double_member() на таком
 * значении молча возвращает 0.0, и весь applet показывал нули, хотя
 * разбор объявлялся успешным. open-meteo присылает настоящие числа,
 * поэтому один и тот же код должен понимать оба вида. */
static double cw_num(JsonObject *o, const char *key, double def)
{
    JsonNode *n = o ? json_object_get_member(o, key) : NULL;

    GType t;

    if (!n)
        return def;
    /* JSON_NODE_HOLDS_INT/DOUBLE появились только в json-glib 1.12,
     * здесь их нет — тип значения смотрим через GType. */
    if (!JSON_NODE_HOLDS_VALUE(n))
        return def;
    t = json_node_get_value_type(n);
    if (t == G_TYPE_STRING) {
        const char *str = json_node_get_string(n);

        return (str && *str) ? g_ascii_strtod(str, NULL) : def;
    }
    if (t == G_TYPE_INT)
        return (double)json_node_get_int(n);
    if (t == G_TYPE_DOUBLE)
        return json_node_get_double(n);
    return def;
}

static int cw_int(JsonObject *o, const char *key, int def)
{
    return (int)lround(cw_num(o, key, (double)def));
}

static const char *cw_kind_desc(int kind)
{
    switch (kind) {
    case CW_W_CLEAR:   return "Ясно";
    case CW_W_PARTLY:  return "Переменная облачность";
    case CW_W_CLOUDY:  return "Облачно";
    case CW_W_FOG:     return "Туман";
    case CW_W_DRIZZLE: return "Морось";
    case CW_W_RAIN:    return "Дождь";
    case CW_W_SNOW:    return "Снег";
    case CW_W_SHOWERS: return "Ливень";
    case CW_W_THUNDER: return "Гроза";
    default:           return "Нет данных";
    }
}

static int cw_wmo_kind(int c)
{
    if (c == 0)                        return CW_W_CLEAR;
    if (c == 1 || c == 2)               return CW_W_PARTLY;
    if (c == 3)                        return CW_W_CLOUDY;
    if (c == 45 || c == 48)             return CW_W_FOG;
    if (c >= 51 && c <= 57)             return CW_W_DRIZZLE;
    if (c >= 61 && c <= 67)             return CW_W_RAIN;
    if (c >= 71 && c <= 77)             return CW_W_SNOW;
    if (c >= 80 && c <= 82)             return CW_W_SHOWERS;
    if (c >= 95)                       return CW_W_THUNDER;
    return CW_W_N;
}

static int cw_ww_kind(int c)
{
    if (c == 113)                      return CW_W_CLEAR;
    if (c == 116)                      return CW_W_PARTLY;
    if (c == 119)                      return CW_W_CLOUDY;
    if (c >= 143 && c <= 148)          return CW_W_FOG;
    if ((c >= 176 && c <= 179) ||
        (c >= 293 && c <= 296) ||
        (c >= 311 && c <= 312))        return CW_W_DRIZZLE;
    if (c >= 182 && c <= 185)          return CW_W_SNOW;
    if ((c >= 299 && c <= 307) ||
        (c >= 350 && c <= 357))         return CW_W_RAIN;
    if ((c >= 313 && c <= 317) ||
        (c >= 365 && c <= 366))         return CW_W_SHOWERS;
    if (c >= 359 && c <= 362)          return CW_W_SNOW;
    if (c >= 386)                      return CW_W_THUNDER;
    return CW_W_N;
}

/* ------------------------------------------------------------------ */
/* данные                                                              */
/* ------------------------------------------------------------------ */

static void cw_hour_clear(gpointer data)
{
    g_free(((CwHour *)data)->label);
}

static void cw_weather_free(CwWeather *w)
{
    if (!w)
        return;
    g_free(w->place);
    g_free(w->desc);
    if (w->hours)
        g_array_unref(w->hours);
    g_free(w);
}

static CwWeather *cw_weather_new(void)
{
    CwWeather *w = g_new0(CwWeather, 1);

    w->hours = g_array_new(FALSE, FALSE, sizeof(CwHour));
    g_array_set_clear_func(w->hours, cw_hour_clear);
    w->kind = CW_W_N;
    return w;
}

/* Похоже ли значение на почтовый индекс: цифры, пробелы, дефисы.
 * Нужно для внятной диагностики. Логика перебора от этого не зависит:
 * индексы понимает и wttr.in (российские, британские), и open-meteo
 * (американские), а непонимание одного источника закрывает второй. */
static gboolean cw_looks_like_zip(const char *s)
{
    gboolean digits = FALSE;

    if (!s || !s[0])
        return FALSE;
    for (; *s; s++) {
        if (g_ascii_isdigit(*s)) { digits = TRUE; continue; }
        if (*s == '-' || *s == ' ' || *s == '/') continue;
        return FALSE;
    }
    return digits;
}

static double cw_temp(const CwPriv *priv, double celsius)
{
    return priv->units == CW_UNITS_IMPERIAL ? celsius * 9.0 / 5.0 + 32.0
                                             : celsius;
}

static double cw_wind(const CwPriv *priv, double kmh)
{
    return priv->units == CW_UNITS_IMPERIAL ? kmh * 0.621371 : kmh;
}

/* ------------------------------------------------------------------ */
/* конфиг                                                              */
/* ------------------------------------------------------------------ */

/* Целое число по указателю. Только цифры — от локали не зависит.
 * После каждого числа запятую нужно пропустить явно: sscanf с "%lf"
 * в ru_RU ждёт запятую как десятичный разделитель и ломается. */
static gboolean cw_u32(const char **p, double *out)
{
    const char *s = *p;
    double v = 0.0;
    gboolean any = FALSE;

    while (*s == ' ')
        s++;
    while (*s >= '0' && *s <= '9') {
        v = v * 10.0 + (*s - '0');
        s++;
        any = TRUE;
    }
    if (!any)
        return FALSE;
    *out = v;
    *p = s;
    return TRUE;
}

static void cw_color_write(GKeyFile *kf, const char *sec, const char *key,
                           const CwColor *c)
{
    char buf[64];

    g_snprintf(buf, sizeof(buf), "rgba(%d,%d,%d,%d)",
               (int)lround(c->r * 255.0), (int)lround(c->g * 255.0),
               (int)lround(c->b * 255.0), (int)lround(c->a * 255.0));
    g_key_file_set_string(kf, sec, key, buf);
}

static void cw_color_read(GKeyFile *kf, const char *sec, const char *key,
                          const CwColor *def, CwColor *out)
{
    char *v = xs_host_api()->conf_str(kf, sec, key, NULL);
    const char *p = v;
    double r, g, b, a;

    *out = *def;
    if (p && g_str_has_prefix(p, "rgba(")) {
        p += 5;
        if (cw_u32(&p, &r) && *p++ == ',' &&
            cw_u32(&p, &g) && *p++ == ',' &&
            cw_u32(&p, &b) && *p++ == ',' &&
            cw_u32(&p, &a)) {
            out->r = CLAMP(r, 0.0, 255.0) / 255.0;
            out->g = CLAMP(g, 0.0, 255.0) / 255.0;
            out->b = CLAMP(b, 0.0, 255.0) / 255.0;
            out->a = CLAMP(a, 0.0, 255.0) / 255.0;
            g_free(v);
            return;
        }
    }
    g_free(v);
    cw_color_write(kf, sec, key, def);   /* мусор заменяем дефолтом */
}

static void cw_save(CwPriv *priv)
{
    if (priv && priv->kf)
        xs_core_plugin_conf_flush(priv->plugin->name);
}

static void cw_status(CwPriv *priv, const char *fmt, ...)
{
    va_list ap;
    char *text;

    if (!priv || !priv->alive)
        return;
    va_start(ap, fmt);
    text = g_strdup_vprintf(fmt, ap);
    va_end(ap);
    g_free(priv->status);
    priv->status = text;
    if (priv->plugin && priv->plugin->win)
        gtk_widget_queue_draw(priv->plugin->win);
}

/* ------------------------------------------------------------------ */
/* сеть                                                                */
/* ------------------------------------------------------------------ */

typedef struct {
    CwPriv       *priv;
    guint         generation;
    int           source;
    int           transport;
    gboolean      geo;          /* TRUE = геокодинг, FALSE = прогноз */
    SoupMessage  *message;
    SoupSession  *session;
    GCancellable *cancellable;
    GInputStream *stream;
} CwJob;

static void cw_job_free(CwJob *job)
{
    if (!job)
        return;
    g_clear_object(&job->cancellable);
    g_clear_object(&job->message);
    g_clear_object(&job->session);
    g_clear_object(&job->stream);
    g_free(job);
}

/* Тело ответа читаем в потоке: сеть блокирующая. */
static void cw_read_body(GTask *task, gpointer source, gpointer data,
                          GCancellable *cancellable)
{
    CwJob *job = data;
    GInputStream *stream = job ? job->stream : NULL;
    GByteArray *body;

    (void)source;
    /* Поток лежит в job, а НЕ приходит аргументом: g_task_set_task_data()
     * подменяет значение, которое g_task_run_in_thread передаёт в data,
     * и туда мы кладём job. Если читать data как GInputStream, получим
     * мусор и "пустой ответ" на любом запросе. */
    if (!stream) {
        g_task_return_new_error(task, G_IO_ERROR, G_IO_ERROR_FAILED,
                                "поток не передан в задачу");
        return;
    }
    GError *error = NULL;
    guchar buf[8192];
    /* Именно gssize, а НЕ gsize: g_input_stream_read возвращает -1 при
     * ошибке, а беззнаковый тип превращает это в 4294967295. Проверка
     * `> 0` тогда проходит, и g_byte_array_append() падает с
     * "adding 4294967295 to array would overflow". Воспроизводится на
     * недоступном хосте, где поток успевает создаться, а чтение — нет. */
    gssize got;
    gsize total = 0;

    body = g_byte_array_new();
    while ((got = g_input_stream_read(stream, buf, sizeof(buf),
                                      cancellable, &error)) > 0) {
        if (total + (gsize)got > CW_MAX_BYTES) {
            g_set_error_literal(&error, G_IO_ERROR, G_IO_ERROR_FAILED,
                                "ответ слишком велик");
            break;
        }
        g_byte_array_append(body, buf, (guint)got);
        total += (gsize)got;
    }
    if (got < 0 && !error)
        g_set_error_literal(&error, G_IO_ERROR, G_IO_ERROR_FAILED,
                            "чтение оборвано");
    g_input_stream_close(stream, NULL, NULL);
    if (error) {
        g_byte_array_unref(body);
        g_task_return_error(task, error);
        return;
    }
    g_task_return_pointer(task, g_byte_array_free_to_bytes(body),
                          (GDestroyNotify)g_bytes_unref);
}

static void cw_apply_result(CwPriv *priv, int source, const char *text);

static void cw_try_next(CwPriv *priv, const char *why);

static void cw_body_done(GObject *src, GAsyncResult *res, gpointer data)
{
    CwJob *job = data;

    (void)src;
    CwPriv *priv = job->priv;
    GError *error = NULL;
    GBytes *bytes = g_task_propagate_pointer(G_TASK(res), &error);
    gsize len = 0;
    const char *body = NULL;

    if (job->generation != priv->generation || !priv->alive) {
        g_clear_error(&error);
        cw_job_free(job);
        return;
    }
    if (bytes)
        body = g_bytes_get_data(bytes, &len);
    /* GBytes не гарантирует завершающий ноль, а json-glib с длиной -1
     * читает по strlen. Без g_strndup разбор уезжал за пределы буфера и
     * молча давал нули во всех полях. */
    if (!body || !len) {
        cw_log(priv->plugin, "пустой ответ");
        g_clear_error(&error);
        cw_try_next(priv, "пустой ответ");
        cw_job_free(job);
        return;
    }
    cw_log(priv->plugin, "получено %lu байт", (unsigned long)len);
    {
        char *text = g_strndup(body, len);

        cw_apply_result(priv, job->source, text);
        g_free(text);
    }
    cw_job_free(job);
}

static void cw_send_done(GObject *src, GAsyncResult *res, gpointer data)
{
    CwJob *job = data;
    CwPriv *priv = job->priv;
    GError *error = NULL;
    GInputStream *stream;
    GTask *task;
    guint status;

    if (job->generation != priv->generation || !priv->alive) {
        g_clear_error(&error);
        cw_job_free(job);
        return;
    }
    stream = soup_session_send_finish(SOUP_SESSION(src), res, &error);
    if (!stream) {
        cw_log(priv->plugin, "ошибка сети: %s",
               error ? error->message : "неизвестно");
        cw_try_next(priv, error ? error->message : "сетевая ошибка");
        g_clear_error(&error);
        cw_job_free(job);
        return;
    }
    status = soup_message_get_status(job->message);
    if (status < 200 || status >= 300) {
        g_object_unref(stream);
        cw_log(priv->plugin, "HTTP %u", status);
        cw_try_next(priv, "HTTP-ошибка");
        cw_job_free(job);
        return;
    }
    job->stream = stream;          /* владеем до конца чтения */
    task = g_task_new(NULL, job->cancellable, cw_body_done, job);
    g_task_set_task_data(task, job, NULL);
    g_task_run_in_thread(task, cw_read_body);
    g_object_unref(task);
}

static void cw_get(CwPriv *priv, const char *url, gboolean geo,
                   int source, int transport)
{
    CwJob *job;
    SoupMessage *msg;
    int timeout = (transport == CW_TR_PROXY) ? CW_TIMEOUT_SEC
                                             : CW_TIMEOUT_DIRECT;

    msg = soup_message_new("GET", url);
    if (!msg) {
        cw_log(priv->plugin, "не собрался запрос: %s", url);
        cw_try_next(priv, "не собрался запрос");
        return;
    }
    cw_log(priv->plugin, "GET [%s%s] %s", geo ? "geo" : "прогноз",
           transport == CW_TR_PROXY ? " через прокси" : " напрямую", url);
    job = g_new0(CwJob, 1);
    job->priv = priv;
    job->generation = priv->generation;
    job->source = source;
    job->transport = transport;
    job->geo = geo;
    job->cancellable = g_cancellable_new();
    job->session = soup_session_new();
    soup_session_set_timeout(job->session, timeout);
    soup_session_set_user_agent(job->session,
                                "Xscreenlets-ClearWeather/0.1");
    if (transport == CW_TR_PROXY && priv->proxy_url && priv->proxy_url[0]) {
        /* libsoup 3.x: SoupProxy удалён. g_proxy_resolver_new() не
         * публичный API, но g_simple_proxy_resolver_new() гонит всё
         * через один адрес — ровно то, что нужно. */
        GProxyResolver *r = g_simple_proxy_resolver_new(priv->proxy_url, NULL);
        soup_session_set_proxy_resolver(job->session, r);
        g_object_unref(r);
    }
    job->message = msg;
    priv->loading = TRUE;
    soup_session_send_async(job->session, msg, G_PRIORITY_DEFAULT,
                            job->cancellable, cw_send_done, job);
}

/* ------------------------------------------------------------------ */
/* разбор JSON                                                         */
/* ------------------------------------------------------------------ */


/* open-meteo: results[] -> широта/долгота/название */
static gboolean cw_parse_geo(CwPriv *priv, const char *text)
{
    JsonParser *parser = json_parser_new();
    JsonNode *root;
    JsonObject *obj, *first;
    JsonArray  *results;
    GError *error = NULL;
    JsonNode *node;
    gboolean ok = FALSE;

    if (!json_parser_load_from_data(parser, text, -1, &error)) {
        g_clear_error(&error);
        g_object_unref(parser);
        return FALSE;
    }
    root = json_parser_get_root(parser);
    obj = (root && JSON_NODE_HOLDS_OBJECT(root))
              ? json_node_get_object(root) : NULL;
    if (obj && json_object_has_member(obj, "results")) {
        node = json_object_get_member(obj, "results");
        results = JSON_NODE_HOLDS_ARRAY(node) ? json_node_get_array(node)
                                              : NULL;
        if (results && json_array_get_length(results) > 0) {
            first = json_array_get_object_element(results, 0);
            if (first &&
                json_object_has_member(first, "latitude") &&
                json_object_has_member(first, "longitude")) {
                priv->lat = json_object_get_double_member(first, "latitude");
                priv->lon = json_object_get_double_member(first, "longitude");
                priv->city_resolved = TRUE;
                ok = TRUE;
            }
        }
    }
    g_object_unref(parser);
    return ok;
}

static const char *cw_om_geo_url(const CwPriv *priv)
{
    static char url[512];

    g_snprintf(url, sizeof(url),
               "https://geocoding-api.open-meteo.com/v1/search"
               "?name=%s&count=1&language=ru&format=json",
               g_uri_escape_string(cw_query(priv), NULL, FALSE));
    return url;
}

static char *cw_om_forecast_url(const CwPriv *priv)
{
    char lat[32], lon[32];
    char *url;

    /* g_ascii_formatd, а НЕ snprintf("%.4f"): у демона
     * LC_ALL=ru_RU.UTF-8, где десятичный разделитель — запятая, и в
     * запрос уходило latitude=44,9572. API отвечал HTTP 400. */
    g_ascii_formatd(lat, sizeof(lat), "%.4f", priv->lat);
    g_ascii_formatd(lon, sizeof(lon), "%.4f", priv->lon);
    url = g_strdup_printf(
        "https://api.open-meteo.com/v1/forecast"
        "?latitude=%s&longitude=%s"
        "&current=temperature_2m,apparent_temperature,relative_humidity_2m,"
        "weather_code,wind_speed_10m"
        "&hourly=temperature_2m,weather_code"
        "&forecast_days=1&timezone=auto",
        lat, lon);
    return url;
}

/* Что сейчас ищем: основное название, а если перебираем — запасное. */
static const char *cw_query(const CwPriv *priv)
{
    if (priv->alt_active && priv->city_alt && priv->city_alt[0])
        return priv->city_alt;
    return priv->city;
}

static char *cw_wttr_url(const CwPriv *priv)
{
    return g_strdup_printf("https://wttr.in/%s?format=j1",
                           g_uri_escape_string(cw_query(priv), NULL, FALSE));
}


static gboolean cw_parse_openmeteo(CwPriv *priv, const char *text,
                                   CwWeather **out)
{
    JsonParser *parser = json_parser_new();
    JsonNode *root;
    JsonObject *obj, *cur, *hourly;
    JsonArray *htimes, *htemps, *hcodes;
    CwWeather *w;
    GError *error = NULL;
    int start = 0, len, i, shown = 0, want;

    if (!json_parser_load_from_data(parser, text, -1, &error)) {
        g_clear_error(&error);
        g_object_unref(parser);
        return FALSE;
    }
    root = json_parser_get_root(parser);
    obj = (root && JSON_NODE_HOLDS_OBJECT(root))
              ? json_node_get_object(root) : NULL;
    if (!obj || !json_object_has_member(obj, "current")) {
        g_object_unref(parser);
        return FALSE;
    }
    cur = json_object_get_object_member(obj, "current");
    if (!json_object_has_member(cur, "temperature_2m")) {
        g_object_unref(parser);
        return FALSE;
    }
    w = cw_weather_new();
    w->temp   = json_object_get_double_member(cur, "temperature_2m");
    w->feels  = json_object_has_member(cur, "apparent_temperature")
                    ? json_object_get_double_member(cur, "apparent_temperature")
                    : w->temp;
    w->humidity = json_object_has_member(cur, "relative_humidity_2m")
                    ? json_object_get_double_member(cur, "relative_humidity_2m")
                    : 0.0;
    w->wind = json_object_has_member(cur, "wind_speed_10m")
                  ? json_object_get_double_member(cur, "wind_speed_10m")
                  : 0.0;
    w->kind = cw_wmo_kind(json_object_has_member(cur, "weather_code")
                          ? (int)json_object_get_int_member(cur, "weather_code")
                          : -1);
    w->desc = g_strdup(cw_kind_desc(w->kind));
    w->place = g_strdup(cw_query(priv));

    /* hourly: время вида "YYYY-MM-DDTHH:MM" */
    if (json_object_has_member(obj, "hourly")) {
        hourly = json_object_get_object_member(obj, "hourly");
        htimes  = json_object_get_array_member(hourly, "time");
        htemps  = json_object_get_array_member(hourly, "temperature_2m");
        hcodes  = json_object_get_array_member(hourly, "weather_code");
        if (htimes) {
            GDateTime *cur_dt = g_date_time_new_now_local();
            char *today = cur_dt ? g_date_time_format(cur_dt, "%Y-%m-%d")
                                 : NULL;
            int cur_hour = cur_dt ? g_date_time_get_hour(cur_dt) : 0;

            if (cur_dt)
                g_date_time_unref(cur_dt);
            len = (int)json_array_get_length(htimes);
            start = 0;
            for (i = 0; i < len; i++) {
                JsonNode *n = json_array_get_element(htimes, i);
                const char *t = n ? json_node_get_string(n) : NULL;

                if (!t || !today || !g_str_has_prefix(t, today))
                    continue;
                if (atoi(t + 11) <= cur_hour)
                    start = i;
            }
            g_free(today);
            want = CLAMP(priv->hours_shown, 1, 12);
            for (i = start; i < len && shown < want; i += CW_OM_STEP) {
                JsonNode *nt = json_array_get_element(htimes, i);
                JsonNode *nv = htemps ? json_array_get_element(htemps, i)
                                      : NULL;
                JsonNode *nc = hcodes ? json_array_get_element(hcodes, i)
                                      : NULL;
                const char *t = nt ? json_node_get_string(nt) : NULL;
                CwHour h;

                if (!t)
                    continue;
                memset(&h, 0, sizeof(h));
                h.label = g_strdup(t + 11);   /* "HH:MM" -> часы */
                if (h.label[2] == ':')
                    h.label[2] = '\0';
                h.temp = nv ? json_node_get_double(nv) : 0.0;
                h.kind = nc ? cw_wmo_kind((int)json_node_get_int(nc))
                            : CW_W_N;
                g_array_append_val(w->hours, h);
                shown++;
            }
        }
    }
    g_object_unref(parser);
    *out = w;
    return TRUE;
}

/* wttr.in отдаёт время суток как "0", "300", "600", "1200" — это
 * часы-минуты без ведущих нулей, а не готовые строки. Без приведения к
 * "00"/"03"/"12" в прогнозе торчат числа 0, 300, 600, 900. */
static char *cw_hour_label(const char *raw)
{
    char buf[8];
    size_t n;

    if (!raw || !raw[0])
        return g_strdup("--");
    for (n = 0; raw[n] && n < sizeof(buf) - 1; n++)
        buf[n] = raw[n];
    buf[n] = '\0';
    if (n == 0 || n > 4)
        return g_strdup("--");
    /* добиваем слева нулями до четырёх знаков, потом берём часы */
    if (n < 4) {
        memmove(buf + (4 - n), buf, n + 1);
        memset(buf, '0', 4 - n);
    }
    if (!g_ascii_isdigit(buf[0]) || !g_ascii_isdigit(buf[1]))
        return g_strdup("--");
    buf[2] = '\0';
    return g_strdup(buf);
}

static gboolean cw_parse_wttr(CwPriv *priv, const char *text,
                              CwWeather **out)
{
    JsonParser *parser = json_parser_new();
    JsonNode *root;
    JsonObject *obj, *cur, *day;
    JsonNode   *cur_node, *day_node;
    JsonArray  *cond, *days, *hours;
    CwWeather *w;
    GError *error = NULL;
    int i, len, shown = 0, want;

    if (!json_parser_load_from_data(parser, text, -1, &error)) {
        g_clear_error(&error);
        g_object_unref(parser);
        return FALSE;
    }
    root = json_parser_get_root(parser);
    obj = (root && JSON_NODE_HOLDS_OBJECT(root))
              ? json_node_get_object(root) : NULL;
    if (!obj || !json_object_has_member(obj, "current_condition")) {
        g_object_unref(parser);
        return FALSE;
    }
    cond = json_object_get_array_member(obj, "current_condition");
    if (!cond || json_array_get_length(cond) < 1) {
        g_object_unref(parser);
        return FALSE;
    }
    cur_node = json_array_get_element(cond, 0);
    cur = cur_node ? json_node_get_object(cur_node) : NULL;
    if (!cur) {
        g_object_unref(parser);
        return FALSE;
    }
    w = cw_weather_new();
    w->temp   = cw_num(cur, "temp_C", 0.0);
    w->feels  = cw_num(cur, "FeelsLikeC", w->temp);
    w->humidity = cw_num(cur, "humidity", 0.0);
    w->wind = cw_num(cur, "windspeedKmph", 0.0);
    w->kind = cw_ww_kind(cw_int(cur, "weatherCode", -1));
    if (json_object_has_member(cur, "weatherDesc")) {
        JsonNode *dn = json_object_get_member(cur, "weatherDesc");
        JsonArray *da = JSON_NODE_HOLDS_ARRAY(dn) ? json_node_get_array(dn)
                                                  : NULL;
        JsonNode *e = da ? json_array_get_element(da, 0) : NULL;
        const char *s = e ? json_node_get_string(e) : NULL;

        w->desc = g_strdup(s ? s : cw_kind_desc(w->kind));
    } else {
        w->desc = g_strdup(cw_kind_desc(w->kind));
    }
    /* wttr.in знает название города в nearest_area */
    w->place = g_strdup(cw_query(priv));
    if (json_object_has_member(obj, "nearest_area")) {
        JsonNode *an = json_object_get_member(obj, "nearest_area");
        JsonArray *aa = JSON_NODE_HOLDS_ARRAY(an) ? json_node_get_array(an)
                                                  : NULL;
        JsonObject *ao = aa ? json_array_get_object_element(aa, 0) : NULL;

        if (ao && json_object_has_member(ao, "areaName")) {
            JsonNode *an2 = json_object_get_member(ao, "areaName");
            JsonArray *av = JSON_NODE_HOLDS_ARRAY(an2)
                                ? json_node_get_array(an2) : NULL;
            JsonNode *e2 = av ? json_array_get_element(av, 0) : NULL;
            /* areaName[0] — ОБЪЕКТ {"value": "..."}, а не строка.
             * json_node_get_string() на объекте вернул NULL, и в окне
             * оставался исходный запрос, то есть почтовый индекс. */
            const char *s = NULL;

            if (e2 && JSON_NODE_HOLDS_OBJECT(e2)) {
                JsonObject *ao2 = json_node_get_object(e2);

                if (json_object_has_member(ao2, "value"))
                    s = json_object_get_string_member(ao2, "value");
            }
            if (s && s[0]) {
                g_free(w->place);
                w->place = g_strdup(s);
            }
        }
    }
    /* почасовой прогноз: у wttr.in он трёхчасовой, 8 точек на сутки */
    days = json_object_get_array_member(obj, "weather");
    if (days && json_array_get_length(days) > 0) {
        day_node = json_array_get_element(days, 0);
        day = day_node ? json_node_get_object(day_node) : NULL;
        if (day && json_object_has_member(day, "hourly")) {
            hours = json_object_get_array_member(day, "hourly");
            len = hours ? (int)json_array_get_length(hours) : 0;
            want = CLAMP(priv->hours_shown, 1, len ? len : 1);
            for (i = 0; i < len && shown < want; i++) {
                JsonObject *ho = json_array_get_object_element(hours, i);
                CwHour h;

                if (!ho)
                    continue;
                memset(&h, 0, sizeof(h));
                h.label = cw_hour_label(
                    json_object_has_member(ho, "time")
                        ? json_object_get_string_member(ho, "time") : NULL);
                h.temp = cw_num(ho, "tempC", 0.0);
                h.kind = cw_ww_kind(cw_int(ho, "weatherCode", -1));
                g_array_append_val(w->hours, h);
                shown++;
            }
        }
    }
    g_object_unref(parser);
    *out = w;
    return TRUE;
}

/* ------------------------------------------------------------------ */
/* перебор попыток                                                    */
/* ------------------------------------------------------------------ */

static void cw_build_attempts(CwPriv *priv)
{
    int sources[2], n_src = 0, transports[2], n_tr = 0;
    int i, j;

    sources[n_src++] = (priv->source == CW_SRC_AUTO) ? CW_SRC_OPENMETEO
                                                     : priv->source;
    if (priv->source == CW_SRC_AUTO)
        sources[n_src++] = CW_SRC_WTTR;

    if (priv->proxy_mode == CW_PM_PROXY) {
        transports[n_tr++] = CW_TR_PROXY;
    } else if (priv->proxy_mode == CW_PM_DIRECT) {
        transports[n_tr++] = CW_TR_DIRECT;
    } else {
        /* В «авто» сначала пробуем тот транспорт, который сработал в
         * прошлый раз: иначе каждый цикл оплачиваем таймаутом по
         * заведомо закрытому пути. */
        transports[n_tr++] = priv->prefer_tr;
        transports[n_tr++] = (priv->prefer_tr == CW_TR_PROXY)
                                 ? CW_TR_DIRECT : CW_TR_PROXY;
    }
    priv->n_attempts = 0;
    for (i = 0; i < n_src; i++) {
        for (j = 0; j < n_tr; j++) {
            if (priv->n_attempts >= CW_MAX_ATTEMPTS)
                break;
            priv->attempts[priv->n_attempts].source = sources[i];
            priv->attempts[priv->n_attempts].transport = transports[j];
            priv->attempts[priv->n_attempts].geo =
                (sources[i] == CW_SRC_OPENMETEO) && !priv->city_resolved;
            priv->n_attempts++;
        }
    }
}

static void cw_run_attempt(XsPlugin *p)
{
    CwPriv *priv = p->priv;
    CwAttempt a;
    int source, transport;

    if (priv->cur_attempt >= priv->n_attempts) {
        priv->loading = FALSE;
        /* Основное название не подошло ни одному агрегатору. Если
         * запасное ещё не пробовали — переключаемся на него и начинаем
         * заново: угадать написание по-английски за пользователя
         * невозможно, но можно дать ему один шанс вписать его руками. */
        if (!priv->alt_active && priv->city_alt && priv->city_alt[0]) {
            priv->alt_active = TRUE;
            priv->cur_attempt = 0;
            cw_log(priv->plugin, "«%s» не нашёлся, пробую «%s»",
                   priv->city, priv->city_alt);
            cw_status(priv, "«%s» не нашлось, пробую «%s»…",
                      priv->city, priv->city_alt);
            cw_build_attempts(priv);
            cw_run_attempt(p);
            return;
        }
        priv->alt_active = FALSE;
        if (cw_looks_like_zip(priv->city))
            cw_status(priv, "Индекс «%s» не найден", priv->city);
        else
            cw_status(priv, "«%s» не найден. Агрегаторы ждут название\n"
                            "на английском — есть поле «Запасное название»",
                      priv->city);
        cw_log(priv->plugin, "итог: %s не найден", priv->city);
        return;
    }
    a = priv->attempts[priv->cur_attempt];
    source = a.source;
    transport = a.transport;
    priv->cur_transport = transport;

    if (source == CW_SRC_OPENMETEO) {
        if (a.geo) {
            cw_get(priv, cw_om_geo_url(priv), TRUE, source, transport);
            return;
        }
        {
            char *url = cw_om_forecast_url(priv);
            cw_get(priv, url, FALSE, source, transport);
            g_free(url);
        }
        return;
    }
    {
        char *url = cw_wttr_url(priv);
        cw_get(priv, url, FALSE, source, transport);
        g_free(url);
    }
}

static void cw_try_next(CwPriv *priv, const char *why)
{
    XsPlugin *p;

    if (!priv || !priv->alive)
        return;
    p = priv->plugin;
    priv->cur_attempt++;
    priv->loading = FALSE;
    if (priv->cur_attempt < priv->n_attempts) {
        cw_status(priv, "Пробую дальше (%s не сработал)…", why);
        cw_run_attempt(p);
    } else {
        cw_status(priv, "Нет данных: все варианты не ответили");
    }
}

static void cw_apply_result(CwPriv *priv, int source, const char *text)
{
    CwWeather *w = NULL;
    gboolean ok;

    if (priv->cur_attempt < priv->n_attempts) {
        priv->prefer_tr = priv->attempts[priv->cur_attempt].transport;
    }
    if (priv->cur_attempt < priv->n_attempts &&
        priv->attempts[priv->cur_attempt].geo) {
        /* это был запрос геокодинга */
        ok = cw_parse_geo(priv, text);
        cw_log(priv->plugin, "геокодинг: %s (%.4f, %.4f)", ok ? "ок" : "не найден",
               priv->lat, priv->lon);
        if (ok) {
            /* Сбрасываем флаг у ТЕКУЩЕЙ попытки: он был выставлен при
             * построении списка, когда город ещё не был найден. Иначе
             * следующий вызов cw_run_attempt() снова пойдёт в геокодинг
             * и applet зациклится на нём. */
            if (priv->cur_attempt < priv->n_attempts)
                priv->attempts[priv->cur_attempt].geo = FALSE;
            xs_host_api()->conf_set_dbl(priv->kf, priv->plugin->name,
                                       "geo_lat", priv->lat);
            xs_host_api()->conf_set_dbl(priv->kf, priv->plugin->name,
                                       "geo_lon", priv->lon);
            /* Запомнить, ПО ЧЕМУ искали: координаты без этого не имеют
             * смысла — сменится город, а в кэше останутся чужие. */
            xs_host_api()->conf_set_str(priv->kf, priv->plugin->name,
                                        "geo_query", cw_query(priv));
            cw_save(priv);
            cw_status(priv, "Город найден, беру прогноз…");
            cw_run_attempt(priv->plugin);   /* тот же индекс, уже без гео */
            return;
        }
        cw_try_next(priv, "город не найден");
        return;
    }
    if (source == CW_SRC_OPENMETEO)
        ok = cw_parse_openmeteo(priv, text, &w);
    else
        ok = cw_parse_wttr(priv, text, &w);
    cw_log(priv->plugin, "разбор %s: %s", source == CW_SRC_OPENMETEO
                                   ? "open-meteo" : "wttr.in",
           ok ? "ок" : "не разобрался");
    if (!ok || !w) {
        cw_try_next(priv, "непонятный ответ");
        return;
    }
    cw_weather_free(priv->weather);
    priv->weather = w;
    priv->loading = FALSE;
    priv->alt_active = FALSE;
    cw_status(priv, "%s · %s",
              priv->cur_transport == CW_TR_PROXY ? "через прокси"
                                                 : "напрямую",
              source == CW_SRC_OPENMETEO ? "open-meteo" : "wttr.in");
    if (priv->plugin && priv->plugin->win)
        gtk_widget_queue_draw(priv->plugin->win);
}

static void cw_start(XsPlugin *p)
{
    CwPriv *priv = p ? p->priv : NULL;

    if (!priv || !priv->alive)
        return;
    priv->generation++;
    priv->cur_attempt = 0;
    cw_build_attempts(priv);
    cw_status(priv, "Обновляю…");
    cw_run_attempt(p);
}

static gboolean cw_timer(gpointer data)
{
    CwPriv *priv = data;

    if (!priv || !priv->alive)
        return G_SOURCE_REMOVE;
    if (!priv->loading)
        cw_start(priv->plugin);
    return G_SOURCE_CONTINUE;
}

/* ------------------------------------------------------------------ */
/* отрисовка                                                           */
/* ------------------------------------------------------------------ */

static PangoFontDescription *cw_font(const char *spec)
{
    PangoFontDescription *d = pango_font_description_from_string(
        spec && spec[0] ? spec : "Sans 10");

    if (!d)
        d = pango_font_description_from_string("Sans 10");
    return d;
}

static void cw_icon(cairo_t *cr, int kind, double x, double y, double s,
                    CwColor *c)
{
    cairo_save(cr);
    cairo_set_line_width(cr, 1.6);
    cairo_set_source_rgba(cr, c->r, c->g, c->b, c->a);

    /* солнце */
    if (kind == CW_W_CLEAR || kind == CW_W_PARTLY) {
        double cx = x + s * 0.5, cy = y + s * 0.5, r = s * 0.22;
        int i;

        cairo_arc(cr, cx, cy, r, 0, 2 * M_PI);
        cairo_fill_preserve(cr);
        cairo_stroke(cr);
        for (i = 0; i < 8; i++) {
            double a = i * M_PI / 4.0;

            cairo_move_to(cr, cx + cos(a) * r * 1.5, cy + sin(a) * r * 1.5);
            cairo_line_to(cr, cx + cos(a) * r * 2.1, cy + sin(a) * r * 2.1);
        }
        cairo_stroke(cr);
    }
    /* облако */
    if (kind != CW_W_CLEAR) {
        double bx = x + s * 0.10, by = y + s * 0.42, bw = s * 0.80, bh = s * 0.34;

        cairo_new_sub_path(cr);
        cairo_arc(cr, bx + bw * 0.28, by + bh * 0.55, bh * 0.52, M_PI, 0);
        cairo_arc(cr, bx + bw * 0.60, by + bh * 0.30, bh * 0.68, M_PI * 1.15,
                  M_PI * 1.85);
        cairo_arc(cr, bx + bw * 0.86, by + bh * 0.55, bh * 0.50, M_PI * 1.6,
                  M_PI * 2.0);
        cairo_line_to(cr, bx + bw * 0.12, by + bh);
        cairo_close_path(cr);
        cairo_fill_preserve(cr);
        cairo_stroke(cr);
    }
    /* осадки */
    if (kind == CW_W_RAIN || kind == CW_W_SHOWERS || kind == CW_W_DRIZZLE) {
        int i;

        for (i = 0; i < 3; i++) {
            double dx = x + s * (0.28 + i * 0.20);

            cairo_move_to(cr, dx, y + s * 0.80);
            cairo_line_to(cr, dx - s * 0.06, y + s * 0.95);
        }
        cairo_stroke(cr);
    }
    if (kind == CW_W_SNOW) {
        int i;

        for (i = 0; i < 3; i++) {
            double dx = x + s * (0.28 + i * 0.20), dy = y + s * 0.88;

            cairo_arc(cr, dx, dy, s * 0.05, 0, 2 * M_PI);
        }
        cairo_fill(cr);
    }
    /* молния */
    if (kind == CW_W_THUNDER) {
        cairo_move_to(cr, x + s * 0.52, y + s * 0.78);
        cairo_line_to(cr, x + s * 0.44, y + s * 0.90);
        cairo_line_to(cr, x + s * 0.51, y + s * 0.90);
        cairo_line_to(cr, x + s * 0.45, y + s * 1.02);
        cairo_stroke(cr);
    }
    cairo_restore(cr);
}

/* Рисует текст и ВОЗВРАЩАЕТ его фактическую высоту в пикселях.
 * Высота нужна, чтобы складывать строки друг под другом: при фиксированных
 * смещениях крупный шрифт температуры налезал на город. */
/* Выравнивание по правому краю: PANGO_ALIGN_RIGHT внутри layout
 * работает только относительно ширины самого layout, поэтому ширину
 * текста меряем сами и рисуем от правого края окна. */
static int cw_text_right(cairo_t *cr, PangoLayout *layout, const char *text,
                         CwColor *c, double right_x, double y)
{
    int tw = 0, th = 0;

    pango_layout_set_text(layout, text ? text : "", -1);
    pango_layout_get_pixel_size(layout, &tw, &th);
    cairo_set_source_rgba(cr, c->r, c->g, c->b, c->a);
    pango_layout_set_alignment(layout, PANGO_ALIGN_LEFT);
    cairo_move_to(cr, right_x - (double)tw, y);
    pango_cairo_show_layout(cr, layout);
    return th;
}

static int cw_text(cairo_t *cr, PangoLayout *layout, const char *text,
                   CwColor *c, double x, double y, gboolean center)
{
    int tw = 0, th = 0;

    pango_layout_set_text(layout, text ? text : "", -1);
    pango_layout_get_pixel_size(layout, &tw, &th);
    cairo_set_source_rgba(cr, c->r, c->g, c->b, c->a);
    pango_layout_set_alignment(layout,
                               center ? PANGO_ALIGN_CENTER : PANGO_ALIGN_LEFT);
    cairo_move_to(cr, x, y);
    pango_cairo_show_layout(cr, layout);
    return th;
}

static void cw_draw(XsPlugin *p, cairo_t *cr, int w, int h)
{
    CwPriv *priv = p ? p->priv : NULL;
    cairo_surface_t *surf;
    cairo_t *g;
    PangoLayout *layout;
    CwWeather *cw;
    char buf[256];
    double y, icon_s, x, text_x;
    int i, shown, th, th2;

    if (!priv)
        return;

    surf = cairo_image_surface_create(CAIRO_FORMAT_ARGB32, w, h);
    g = cairo_create(surf);
    cairo_set_operator(g, CAIRO_OPERATOR_SOURCE);
    cairo_set_source_rgba(g, 0, 0, 0, 0);
    cairo_paint(g);
    cairo_set_operator(g, CAIRO_OPERATOR_OVER);

    /* скруглённый фон */
    cairo_new_sub_path(g);
    cairo_arc(g, w - 3.5, 3.5, 3.5, -M_PI / 2, 0);
    cairo_arc(g, w - 3.5, h - 3.5, 3.5, 0, M_PI / 2);
    cairo_arc(g, 3.5, h - 3.5, 3.5, M_PI / 2, M_PI);
    cairo_arc(g, 3.5, 3.5, 3.5, M_PI, M_PI * 1.5);
    cairo_close_path(g);
    cairo_set_source_rgba(g, priv->bg_color.r, priv->bg_color.g,
                          priv->bg_color.b, priv->bg_color.a);
    cairo_fill_preserve(g);
    cairo_set_source_rgba(g, 0.45, 0.45, 0.45, 0.9);
    cairo_set_line_width(g, 1.0);
    cairo_stroke(g);

    layout = pango_cairo_create_layout(g);
    cw = priv->weather;

    if (!cw) {
        PangoFontDescription *fd = cw_font(priv->desc_font);

        pango_layout_set_font_description(layout, fd);
        pango_font_description_free(fd);
        cw_text(g, layout, priv->status ? priv->status : "Загрузка…",
                &priv->desc_color, CW_MARGIN, h / 2 - 8, FALSE);
        g_object_unref(layout);
        cairo_set_operator(cr, CAIRO_OPERATOR_SOURCE);
        cairo_set_source_surface(cr, surf, 0, 0);
        cairo_paint(cr);
        cairo_destroy(g);
        cairo_surface_destroy(surf);
        return;
    }

    /* --- шапка: значок, температура, город ---
     *
     * Каждая строка ставится по фактической высоте предыдущей. Раньше
     * позиции были константами (город на CW_MARGIN+20, описание на
     * CW_MARGIN+40), а температура в 26pt занимает ~35 пикселей, и
     * город попадал прямо на неё. */
    icon_s = 40.0;
    text_x = CW_MARGIN + icon_s + CW_GAP;
    cw_icon(g, cw->kind, CW_MARGIN, CW_MARGIN, icon_s, &priv->temp_color);

    {
        PangoFontDescription *fd = cw_font(priv->temp_font);

        pango_layout_set_font_description(layout, fd);
        pango_font_description_free(fd);
    }
    g_snprintf(buf, sizeof(buf), "%.0f°", cw_temp(priv, cw->temp));
    th = cw_text(g, layout, buf, &priv->temp_color, text_x,
                 CW_MARGIN - 2, FALSE);

    /* Город уводим в правый верхний угол, в одну строку с температурой:
     * так шапка занимает высоту значка, а не высоту значка плюс строка. */
    {
        PangoFontDescription *fd = cw_font(priv->city_font);

        pango_layout_set_font_description(layout, fd);
        pango_font_description_free(fd);
    }
    th2 = cw_text_right(g, layout, cw->place ? cw->place : priv->city,
                        &priv->city_color, w - CW_MARGIN, CW_MARGIN - 2);

    /* низ шапки = максимум из значка, температуры и города */
    y = MAX(MAX(CW_MARGIN + icon_s, (CW_MARGIN - 2) + th), CW_MARGIN - 2 + th2)
        + CW_GAP;

    {
        PangoFontDescription *fd = cw_font(priv->desc_font);

        pango_layout_set_font_description(layout, fd);
        pango_font_description_free(fd);
    }
    th = cw_text(g, layout, cw->desc ? cw->desc : "", &priv->desc_color,
                 CW_MARGIN, y, FALSE);
    y += th + 2;
    g_snprintf(buf, sizeof(buf), "ветер %.0f %s   влажность %.0f%%",
               cw_wind(priv, cw->wind),
               priv->units == CW_UNITS_IMPERIAL ? "mph" : "км/ч",
               cw->humidity);
    th = cw_text(g, layout, buf, &priv->desc_color, CW_MARGIN, y, FALSE);
    y += th + CW_GAP + 4;

    /* --- почасовой прогноз --- */
    if (cw->hours->len > 0) {
        int n = (int)MIN(cw->hours->len, MAX(priv->hours_shown, 1));
        double col = (w - 2 * CW_MARGIN) / n;

        shown = 0;
        {
            PangoFontDescription *fd = cw_font(priv->hour_font);

            pango_layout_set_font_description(layout, fd);
            pango_font_description_free(fd);
        }
        for (i = 0; i < (int)cw->hours->len && shown < n; i++) {
            CwHour *hh = &g_array_index(cw->hours, CwHour, i);

            x = CW_MARGIN + col * shown + col / 2;
            /* Час — просто число, знак градуса тут ставить некуда:
             * температура идёт следующей строкой. */
            g_strlcpy(buf, hh->label, sizeof(buf));
            th = cw_text(g, layout, buf, &priv->hour_color, x, y, TRUE);
            g_snprintf(buf, sizeof(buf), "%.0f°", cw_temp(priv, hh->temp));
            th = cw_text(g, layout, buf, &priv->hour_color, x,
                         y + th + 2, TRUE);
            cw_icon(g, hh->kind, x - 7, y + th + 6, 14.0, &priv->hour_color);
            shown++;
        }
    }

    if (priv->status && priv->status[0]) {
        PangoFontDescription *fd = cw_font("Sans 7");
        int tw = 0, th = 0;
        char st[160];

        pango_layout_set_font_description(layout, fd);
        pango_font_description_free(fd);
        g_strlcpy(st, priv->status, sizeof(st));
        pango_layout_set_text(layout, st, -1);
        pango_layout_get_pixel_size(layout, &tw, &th);
        /* Выравнивание по правому краю: при левом текст начинался с
         * w-CW_MARGIN и целиком уезжал за границу окна. */
        pango_layout_set_alignment(layout, PANGO_ALIGN_RIGHT);
        cairo_set_source_rgba(g, priv->desc_color.r, priv->desc_color.g,
                              priv->desc_color.b, priv->desc_color.a * 0.8);
        cairo_move_to(g, w - CW_MARGIN, 1);
        pango_cairo_show_layout(g, layout);
    }

    g_object_unref(layout);
    /* Кадр собирали на отдельной поверхности, а на окно переносим
     * через НАСТОЯЩИЙ cr. Если красить surf сам в себя, результат
     * теряется и окно остаётся пустым.
     *
     * Порядок обязателен: сначала set_source_surface + paint, и только
     * потом destroy. Наоборот получается use-after-free, и cairo падает
     * с assert внутри cairo_pattern_create_for_surface. */
    cairo_set_operator(cr, CAIRO_OPERATOR_SOURCE);
    cairo_set_source_surface(cr, surf, 0, 0);
    cairo_paint(cr);
    cairo_destroy(g);
    cairo_surface_destroy(surf);
}

/* ------------------------------------------------------------------ */
/* настройки                                                           */
/* ------------------------------------------------------------------ */

static const char *cw_src_names[] = { "Авто", "open-meteo", "wttr.in" };
static const char *cw_pm_names[]  = { "Авто", "Напрямую", "Через прокси" };
static const char *cw_un_names[]   = { "Метрические", "Имперские" };

static GtkWidget *cw_row(GtkWidget *grid, int r, const char *label,
                         GtkWidget *widget)
{
    GtkWidget *l = gtk_label_new(label);

    gtk_widget_set_halign(l, GTK_ALIGN_START);
    gtk_grid_attach(GTK_GRID(grid), l, 0, r, 1, 1);
    gtk_widget_set_hexpand(widget, TRUE);
    gtk_grid_attach(GTK_GRID(grid), widget, 1, r, 1, 1);
    return widget;
}

static GtkWidget *cw_combo(const char **items, int n, int active)
{
    GtkWidget *cb = gtk_combo_box_text_new();
    int i;

    for (i = 0; i < n; i++)
        gtk_combo_box_text_append_text(GTK_COMBO_BOX_TEXT(cb), items[i]);
    gtk_combo_box_set_active(GTK_COMBO_BOX(cb), CLAMP(active, 0, n - 1));
    return cb;
}

static void cw_tag(GtkWidget *w, const char *key)
{
    g_object_set_data_full(G_OBJECT(w), "cw-key", g_strdup(key), g_free);
}

static const char *cw_key_of(GtkWidget *w)
{
    return g_object_get_data(G_OBJECT(w), "cw-key");
}

static void cw_save_size(CwPriv *priv, GtkWidget *spin)
{
    priv->width  = gtk_spin_button_get_value_as_int(GTK_SPIN_BUTTON(spin));
    xs_host_api()->conf_set_int(priv->kf, priv->plugin->name,
                                "window_width", priv->width);
}

static void cw_save_height(CwPriv *priv, GtkWidget *spin)
{
    priv->height = gtk_spin_button_get_value_as_int(GTK_SPIN_BUTTON(spin));
    xs_host_api()->conf_set_int(priv->kf, priv->plugin->name,
                                "window_height", priv->height);
}

static void cw_on_combo(GtkComboBox *cb, gpointer data)
{
    XsPlugin *p = data;
    CwPriv *priv = p ? p->priv : NULL;
    const char *key = cw_key_of(GTK_WIDGET(cb));
    int val = gtk_combo_box_get_active(cb);

    if (!priv || !key)
        return;
    if (strcmp(key, "source") == 0) {
        priv->source = val;
        xs_host_api()->conf_set_int(priv->kf, p->name, "source", val);
    } else if (strcmp(key, "proxy_mode") == 0) {
        priv->proxy_mode = val;
        xs_host_api()->conf_set_int(priv->kf, p->name, "proxy_mode", val);
    } else if (strcmp(key, "units") == 0) {
        priv->units = val;
        xs_host_api()->conf_set_int(priv->kf, p->name, "units", val);
    }
    cw_save(priv);
    cw_start(p);
}

static void cw_on_city(GtkEntry *e, gpointer data)
{
    XsPlugin *p = data;
    CwPriv *priv = p ? p->priv : NULL;
    const char *text = gtk_entry_get_text(e);

    if (!priv || !text)
        return;
    if (g_strcmp0(text, priv->city) == 0)
        return;
    g_free(priv->city);
    priv->city = g_strdup(text);
    xs_host_api()->conf_set_str(priv->kf, p->name, "city", priv->city);
    /* город другой — координаты прежние больше не подходят */
    priv->city_resolved = FALSE;
    g_key_file_remove_key(priv->kf, p->name, "geo_lat", NULL);
    g_key_file_remove_key(priv->kf, p->name, "geo_lon", NULL);
    g_key_file_remove_key(priv->kf, p->name, "geo_query", NULL);
    cw_save(priv);
    cw_start(p);
}

static void cw_on_city_alt(GtkEntry *e, gpointer data)
{
    XsPlugin *p = data;
    CwPriv *priv = p ? p->priv : NULL;
    const char *text = gtk_entry_get_text(e);

    if (!priv || !text)
        return;
    if (g_strcmp0(text, priv->city_alt) == 0)
        return;
    g_free(priv->city_alt);
    priv->city_alt = g_strdup(text);
    xs_host_api()->conf_set_str(priv->kf, p->name, "city_alt", priv->city_alt);
    cw_save(priv);
    /* Сбрасываем флаг перебора: сначала проверяем основное название. */
    priv->alt_active = FALSE;
    cw_start(p);
}

static void cw_on_proxy(GtkEntry *e, gpointer data)
{
    XsPlugin *p = data;
    CwPriv *priv = p ? p->priv : NULL;
    const char *text = gtk_entry_get_text(e);

    if (!priv || !text)
        return;
    if (g_strcmp0(text, priv->proxy_url) == 0)
        return;
    g_free(priv->proxy_url);
    priv->proxy_url = g_strdup(text);
    xs_host_api()->conf_set_str(priv->kf, p->name, "proxy_url",
                                priv->proxy_url);
    cw_save(priv);
    cw_start(p);
}

static void cw_on_refresh(GtkSpinButton *s, gpointer data)
{
    XsPlugin *p = data;
    CwPriv *priv = p ? p->priv : NULL;

    if (!priv)
        return;
    priv->refresh_min = (guint)gtk_spin_button_get_value_as_int(s);
    xs_host_api()->conf_set_int(priv->kf, p->name, "refresh_min",
                                (int)priv->refresh_min);
    cw_save(priv);
    if (priv->timer_id)
        g_source_remove(priv->timer_id);
    priv->timer_id = g_timeout_add_seconds(60 * MAX(priv->refresh_min, 1),
                                           cw_timer, priv);
}

static void cw_on_hours(GtkSpinButton *s, gpointer data)
{
    XsPlugin *p = data;
    CwPriv *priv = p ? p->priv : NULL;

    if (!priv)
        return;
    priv->hours_shown = gtk_spin_button_get_value_as_int(s);
    xs_host_api()->conf_set_int(priv->kf, p->name, "hours_shown",
                                priv->hours_shown);
    cw_save(priv);
    if (p->win)
        gtk_widget_queue_draw(p->win);
}

static void cw_on_size(GtkSpinButton *s, gpointer data)
{
    XsPlugin *p = data;
    CwPriv *priv = p ? p->priv : NULL;

    if (!priv || !p->win)
        return;
    if (cw_key_of(GTK_WIDGET(s)) &&
        strcmp(cw_key_of(GTK_WIDGET(s)), "h") == 0)
        cw_save_height(priv, GTK_WIDGET(s));
    else
        cw_save_size(priv, GTK_WIDGET(s));
    xs_host_api()->resize(p, priv->width, priv->height);
    xs_host_api()->invalidate(p);
}

static void cw_on_color(GtkColorButton *b, gpointer data)
{
    XsPlugin *p = data;
    CwPriv *priv = p ? p->priv : NULL;
    const char *key = cw_key_of(GTK_WIDGET(b));
    GdkRGBA rgba;
    CwColor *target = NULL;

    if (!priv || !key)
        return;
    if (strcmp(key, "city_color") == 0)        target = &priv->city_color;
    else if (strcmp(key, "temp_color") == 0)   target = &priv->temp_color;
    else if (strcmp(key, "desc_color") == 0)   target = &priv->desc_color;
    else if (strcmp(key, "hour_color") == 0)   target = &priv->hour_color;
    else if (strcmp(key, "bg_color") == 0)     target = &priv->bg_color;
    if (!target)
        return;
    gtk_color_chooser_get_rgba(GTK_COLOR_CHOOSER(b), &rgba);
    target->r = rgba.red;
    target->g = rgba.green;
    target->b = rgba.blue;
    target->a = rgba.alpha;
    cw_color_write(priv->kf, p->name, key, target);
    cw_save(priv);
    if (p->win)
        gtk_widget_queue_draw(p->win);
}

static void cw_on_font(GtkFontButton *b, gpointer data)
{
    XsPlugin *p = data;
    CwPriv *priv = p ? p->priv : NULL;
    const char *key = cw_key_of(GTK_WIDGET(b));
    char *fname;

    if (!priv || !key)
        return;
    fname = gtk_font_chooser_get_font(GTK_FONT_CHOOSER(b));  /* transfer full */
    if (!fname)
        return;
    if (strcmp(key, "city_font") == 0) {
        g_free(priv->city_font);
        priv->city_font = g_strdup(fname);
    } else if (strcmp(key, "temp_font") == 0) {
        g_free(priv->temp_font);
        priv->temp_font = g_strdup(fname);
    } else if (strcmp(key, "desc_font") == 0) {
        g_free(priv->desc_font);
        priv->desc_font = g_strdup(fname);
    } else if (strcmp(key, "hour_font") == 0) {
        g_free(priv->hour_font);
        priv->hour_font = g_strdup(fname);
    }
    g_key_file_set_string(priv->kf, p->name, key, fname);
    g_free(fname);
    cw_save(priv);
    if (p->win)
        gtk_widget_queue_draw(p->win);
}

static GtkWidget *cw_color_button(const CwColor *c, const char *key)
{
    GdkRGBA rgba = { c->r, c->g, c->b, c->a };
    GtkWidget *b = gtk_color_button_new_with_rgba(&rgba);

    cw_tag(b, key);
    g_signal_connect(b, "color-set", G_CALLBACK(cw_on_color), NULL);
    return b;
}

static GtkWidget *cw_font_button(const char *font, const char *key)
{
    GtkWidget *b = gtk_font_button_new_with_font(font ? font : "Sans 10");

    cw_tag(b, key);
    g_signal_connect(b, "font-set", G_CALLBACK(cw_on_font), NULL);
    return b;
}

static void cw_properties(XsPlugin *p, GtkNotebook *nb)
{
    CwPriv *priv = p ? p->priv : NULL;
    GtkWidget *page, *grid;
    GtkWidget *cb, *entry, *spin;
    int r = 0;

    if (!priv)
        return;
    page = gtk_box_new(GTK_ORIENTATION_VERTICAL, 6);
    gtk_container_set_border_width(GTK_CONTAINER(page), 8);
    grid = gtk_grid_new();
    gtk_grid_set_column_spacing(GTK_GRID(grid), 8);
    gtk_grid_set_row_spacing(GTK_GRID(grid), 5);
    gtk_box_pack_start(GTK_BOX(page), grid, FALSE, FALSE, 0);
    gtk_box_pack_start(GTK_BOX(page),
                       gtk_label_new("Название можно писать на любом языке; если не "
                                    "нашлось — берётся запасное. Вместо "
                                    "города можно указать почтовый "
                                    "индекс. Соединение: напрямую или "
                                    "через адрес ниже."),
                       FALSE, FALSE, 0);
    gtk_notebook_append_page(nb, page, gtk_label_new("Погода"));

    entry = gtk_entry_new();
    gtk_entry_set_text(GTK_ENTRY(entry), priv->city ? priv->city : "");
    cw_tag(entry, "city");
    g_signal_connect(entry, "activate", G_CALLBACK(cw_on_city), p);
    g_signal_connect(entry, "changed", G_CALLBACK(cw_on_city), p);
    cw_row(grid, r++, "Город или индекс:", entry);

    entry = gtk_entry_new();
    gtk_entry_set_text(GTK_ENTRY(entry), priv->city_alt ? priv->city_alt : "");
    cw_tag(entry, "city_alt");
    g_signal_connect(entry, "activate", G_CALLBACK(cw_on_city_alt), p);
    cw_row(grid, r++, "Запасное название:", entry);

    cb = cw_combo(cw_src_names, G_N_ELEMENTS(cw_src_names), priv->source);
    cw_tag(cb, "source");
    g_signal_connect(cb, "changed", G_CALLBACK(cw_on_combo), p);
    cw_row(grid, r++, "Источник:", cb);

    cb = cw_combo(cw_pm_names, G_N_ELEMENTS(cw_pm_names), priv->proxy_mode);
    cw_tag(cb, "proxy_mode");
    g_signal_connect(cb, "changed", G_CALLBACK(cw_on_combo), p);
    cw_row(grid, r++, "Соединение:", cb);

    entry = gtk_entry_new();
    gtk_entry_set_text(GTK_ENTRY(entry), priv->proxy_url ? priv->proxy_url : "");
    cw_tag(entry, "proxy_url");
    g_signal_connect(entry, "activate", G_CALLBACK(cw_on_proxy), p);
    cw_row(grid, r++, "Прокси:", entry);

    cb = cw_combo(cw_un_names, G_N_ELEMENTS(cw_un_names), priv->units);
    cw_tag(cb, "units");
    g_signal_connect(cb, "changed", G_CALLBACK(cw_on_combo), p);
    cw_row(grid, r++, "Единицы:", cb);

    spin = gtk_spin_button_new_with_range(5, 180, 5);
    gtk_spin_button_set_value(GTK_SPIN_BUTTON(spin), priv->refresh_min);
    g_signal_connect(spin, "value-changed", G_CALLBACK(cw_on_refresh), p);
    cw_row(grid, r++, "Обновлять, мин:", spin);

    spin = gtk_spin_button_new_with_range(3, 12, 1);
    gtk_spin_button_set_value(GTK_SPIN_BUTTON(spin), priv->hours_shown);
    g_signal_connect(spin, "value-changed", G_CALLBACK(cw_on_hours), p);
    cw_row(grid, r++, "Часов в прогнозе:", spin);

    spin = gtk_spin_button_new_with_range(160, 800, 10);
    gtk_spin_button_set_value(GTK_SPIN_BUTTON(spin), priv->width);
    cw_tag(spin, "w");
    g_signal_connect(spin, "value-changed", G_CALLBACK(cw_on_size), p);
    cw_row(grid, r++, "Ширина:", spin);

    spin = gtk_spin_button_new_with_range(120, 600, 10);
    gtk_spin_button_set_value(GTK_SPIN_BUTTON(spin), priv->height);
    cw_tag(spin, "h");
    g_signal_connect(spin, "value-changed", G_CALLBACK(cw_on_size), p);
    cw_row(grid, r++, "Высота:", spin);

    cw_row(grid, r++, "Цвет города:", cw_color_button(&priv->city_color,
                                                      "city_color"));
    cw_row(grid, r++, "Цвет температуры:",
           cw_color_button(&priv->temp_color, "temp_color"));
    cw_row(grid, r++, "Цвет описания:",
           cw_color_button(&priv->desc_color, "desc_color"));
    cw_row(grid, r++, "Цвет часов:", cw_color_button(&priv->hour_color,
                                                    "hour_color"));
    cw_row(grid, r++, "Цвет фона:", cw_color_button(&priv->bg_color,
                                                   "bg_color"));

    cw_row(grid, r++, "Шрифт города:", cw_font_button(priv->city_font,
                                                     "city_font"));
    cw_row(grid, r++, "Шрифт температуры:",
           cw_font_button(priv->temp_font, "temp_font"));
    cw_row(grid, r++, "Шрифт описания:", cw_font_button(priv->desc_font,
                                                        "desc_font"));
    cw_row(grid, r++, "Шрифт часов:", cw_font_button(priv->hour_font,
                                                     "hour_font"));
}

/* ------------------------------------------------------------------ */
/* контекстное меню                                                    */
/* ------------------------------------------------------------------ */

static void cw_menu_cmd(XsPlugin *p, const char *cmd)
{
    CwPriv *priv = p ? p->priv : NULL;

    if (!priv)
        return;
    if (strcmp(cmd, "refresh") == 0) {
        cw_start(p);
    } else if (strcmp(cmd, "direct") == 0) {
        priv->proxy_mode = CW_PM_DIRECT;
        xs_host_api()->conf_set_int(priv->kf, p->name, "proxy_mode",
                                    CW_PM_DIRECT);
        cw_save(priv);
        cw_start(p);
    } else if (strcmp(cmd, "proxy") == 0) {
        priv->proxy_mode = CW_PM_PROXY;
        xs_host_api()->conf_set_int(priv->kf, p->name, "proxy_mode",
                                    CW_PM_PROXY);
        cw_save(priv);
        cw_start(p);
    }
}

static void cw_menu(XsPlugin *p, GtkMenu *m)
{
    GtkWidget *item;

    (void)p;

    item = gtk_menu_item_new_with_label("Обновить сейчас");
    g_signal_connect_swapped(item, "activate",
                             G_CALLBACK(cw_menu_cmd), (gpointer)"refresh");
    gtk_menu_shell_append(GTK_MENU_SHELL(m), item);

    item = gtk_menu_item_new_with_label("Соединение: напрямую");
    g_signal_connect_swapped(item, "activate",
                             G_CALLBACK(cw_menu_cmd), (gpointer)"direct");
    gtk_menu_shell_append(GTK_MENU_SHELL(m), item);

    item = gtk_menu_item_new_with_label("Соединение: через прокси");
    g_signal_connect_swapped(item, "activate",
                             G_CALLBACK(cw_menu_cmd), (gpointer)"proxy");
    gtk_menu_shell_append(GTK_MENU_SHELL(m), item);
    gtk_widget_show_all(item);
}

/* ------------------------------------------------------------------ */
/* точки входа                                                         */
/* ------------------------------------------------------------------ */

static int cw_init(XsPlugin *p, GKeyFile *kf)
{
    static const CwColor def_city  = { 0.85, 0.85, 0.85, 1.0 };
    static const CwColor def_temp  = { 1.00, 0.85, 0.30, 1.0 };
    static const CwColor def_desc  = { 0.78, 0.80, 0.84, 1.0 };
    static const CwColor def_hour  = { 0.90, 0.90, 0.92, 1.0 };
    static const CwColor def_bg    = { 0.05, 0.06, 0.08, 0.85 };
    CwPriv *priv;
    int x, y;

    if (!p || !kf)
        return -1;
    priv = g_new0(CwPriv, 1);
    priv->plugin = p;
    priv->kf = kf;
    priv->alive = TRUE;
    priv->prefer_tr = CW_TR_DIRECT;

    priv->city = xs_host_api()->conf_str(kf, p->name, "city",
                                         CW_DEFAULT_CITY);
    priv->city_alt = xs_host_api()->conf_str(kf, p->name, "city_alt", "");
    priv->proxy_url = xs_host_api()->conf_str(kf, p->name, "proxy_url",
                                              CW_DEFAULT_PROXY);
    priv->source = CLAMP(xs_host_api()->conf_int(kf, p->name, "source",
                                                 CW_SRC_AUTO),
                         CW_SRC_AUTO, CW_SRC_WTTR);
    priv->proxy_mode = CLAMP(xs_host_api()->conf_int(kf, p->name,
                                                     "proxy_mode", CW_PM_AUTO),
                             CW_PM_AUTO, CW_PM_PROXY);
    priv->units = CLAMP(xs_host_api()->conf_int(kf, p->name, "units",
                                                CW_UNITS_METRIC),
                        CW_UNITS_METRIC, CW_UNITS_IMPERIAL);
    priv->refresh_min = (guint)CLAMP(xs_host_api()->conf_int(
                                          kf, p->name, "refresh_min",
                                          CW_REFRESH_DEFAULT), 5, 180);
    priv->hours_shown = CLAMP(xs_host_api()->conf_int(kf, p->name,
                                                      "hours_shown",
                                                      CW_HOURS_DEFAULT),
                              3, 12);
    priv->lat  = xs_host_api()->conf_dbl(kf, p->name, "geo_lat", 0.0);
    priv->lon  = xs_host_api()->conf_dbl(kf, p->name, "geo_lon", 0.0);
    priv->city_resolved = g_key_file_has_key(kf, p->name, "geo_lat", NULL) &&
                          g_key_file_has_key(kf, p->name, "geo_lon", NULL);
    if (priv->city_resolved) {
        /* Кэш координат привязан к запросу, по которому их получили. */
        char *gq = xs_host_api()->conf_str(kf, p->name, "geo_query", NULL);
        gboolean match = FALSE;

        if (gq && (g_strcmp0(gq, priv->city) == 0 ||
                   g_strcmp0(gq, priv->city_alt) == 0))
            match = TRUE;
        g_free(gq);
        if (!match)
            priv->city_resolved = FALSE;
    }

    priv->width  = CLAMP(xs_host_api()->conf_int(kf, p->name, "window_width",
                                                 CW_DEFAULT_W), 160, 800);
    priv->height = CLAMP(xs_host_api()->conf_int(kf, p->name, "window_height",
                                                 CW_DEFAULT_H), 120, 600);

    priv->city_font = xs_host_api()->conf_str(kf, p->name, "city_font",
                                              "Sans Bold 10");
    priv->temp_font = xs_host_api()->conf_str(kf, p->name, "temp_font",
                                              "Sans Bold 26");
    priv->desc_font = xs_host_api()->conf_str(kf, p->name, "desc_font",
                                              "Sans 9");
    priv->hour_font = xs_host_api()->conf_str(kf, p->name, "hour_font",
                                              "Sans 8");
    cw_color_read(kf, p->name, "city_color", &def_city, &priv->city_color);
    cw_color_read(kf, p->name, "temp_color", &def_temp, &priv->temp_color);
    cw_color_read(kf, p->name, "desc_color", &def_desc, &priv->desc_color);
    cw_color_read(kf, p->name, "hour_color", &def_hour, &priv->hour_color);
    cw_color_read(kf, p->name, "background_color", &def_bg, &priv->bg_color);

    x = xs_host_api()->conf_int(kf, p->name, "x", 900);
    y = xs_host_api()->conf_int(kf, p->name, "y", 120);
    xs_core_plugin_conf_flush(p->name);

    p->priv = priv;
    p->win = xs_host_api()->make_window(p, x, y, priv->width, priv->height);
    if (!p->win) {
        p->host->log("clearweather: не удалось создать окно");
        cw_weather_free(priv->weather);
        g_free(priv->city);
        g_free(priv->city_alt);
        g_free(priv->proxy_url);
        g_free(priv->status);
        g_free(priv->city_font);
        g_free(priv->temp_font);
        g_free(priv->desc_font);
        g_free(priv->hour_font);
        g_free(priv);
        p->priv = NULL;
        return -1;
    }
    priv->status = g_strdup("Загрузка…");
    cw_start(p);
    priv->timer_id = g_timeout_add_seconds(60 * MAX(priv->refresh_min, 1),
                                           cw_timer, priv);
    return 0;
}

static void cw_shutdown(XsPlugin *p)
{
    CwPriv *priv = p ? p->priv : NULL;

    if (!priv)
        return;
    priv->alive = FALSE;
    priv->generation++;          /* всё в полёте отсеется по generation */
    if (priv->timer_id) {
        g_source_remove(priv->timer_id);
        priv->timer_id = 0;
    }
    cw_weather_free(priv->weather);
    g_free(priv->status);
    g_free(priv->city);
    g_free(priv->city_alt);
    g_free(priv->proxy_url);
    g_free(priv->city_font);
    g_free(priv->temp_font);
    g_free(priv->desc_font);
    g_free(priv->hour_font);
    g_free(priv);
    p->priv = NULL;
}

static const XsPluginOps cw_ops = {
    .init = cw_init,
    .draw = cw_draw,
    .tick = NULL,
    .button = NULL,
    .motion = NULL,
    .shutdown = cw_shutdown,
    .menu = cw_menu,
    .menu_cmd = cw_menu_cmd,
    .properties = cw_properties,
};

static XsPluginDesc cw_desc = {
    "clearweather",
    XS_API_VERSION,
    &cw_ops,
    "Погода: open-meteo и wttr.in, прямо или через прокси",
    "kms",
    "0.1"
};

XS_PLUGIN_EXPORT(&cw_desc)
