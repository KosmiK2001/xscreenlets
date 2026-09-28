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
#include <librsvg/rsvg.h>
#include <json-glib/json-glib.h>
#include <string.h>
#include <math.h>

/* ------------------------------------------------------------------ */
/* константы                                                           */
/* ------------------------------------------------------------------ */

#define CW_DEFAULT_W        320
#define CW_DEFAULT_H        180
/* Высота, под которую вёрстка подобрана замерами: значки, шрифты и
 * отступы считаются от неё. Окно 320x242 повторяет пропорции
 * оригинала 132x100, но пять колонок по 62 px не заполняют такую
 * высоту, поэтому всё масштабируется целиком. */
#define CW_REF_H            169.0
#define CW_MARGIN           8.0
#define CW_GAP              4.0
#define CW_MAX_BYTES        (2U * 1024U * 1024U)
#define CW_TIMEOUT_SEC      10
#define CW_TIMEOUT_DIRECT   6
#define CW_DEFAULT_CITY     "Симферополь"
#define CW_DEFAULT_PROXY    "http://127.0.0.1:10809"
#define CW_REFRESH_DEFAULT  15
/* Что показывать в нижней полосе */
#define CW_VIEW_DAYS    0   /* 6 дней, как в родном апплете */
#define CW_VIEW_HOURS   1   /* почасовой ряд */
#define CW_VIEW_BOTH    2   /* и то и то */
#define CW_VIEW_MIN     0
#define CW_VIEW_MAX     2
#define CW_DAYS_MAX     5   /* в родном апплете пять дней */
#define CW_ICON_COUNT  49
#define CW_HOURS_DEFAULT    8
#define CW_OM_STEP          3          /* показывать каждый 3-й час */
/* Для часового вида просим 2 суток: при forecast_days=1 массив
 * hourly заканчивается в 23:00, и в 21:00 наступает шаг 3, который
 * сразу выходит за массив — полоса оставалась с одной колонкой. */
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

/* Сутки для режима «6 дней». В почасовом массиве такой структуры нет:
 * там label — это час, а не день недели. */
typedef struct {
    char   *label;      /* "Пн" */
    double  tmax, tmin;
    int     kind;
    guint   rain;       /* шанс осадков, % */
} CwDay;

typedef struct {
    char   *place;
    char   *desc;
    double  temp, feels, humidity, wind;
    int     kind;
    gboolean night;     /* сейчас ночь по месту наблюдения */
    GArray *hours;      /* CwHour */
    GArray *days;       /* CwDay  */
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
    /* Короткая метка источника для угла окна; длинные сообщения о
     * ходе дела и ошибках живут отдельно в status. */
    char      *badge;
    /* Кэш темы: 49 иконок грузятся один раз, а не каждый кадр. */
    GdkPixbuf  *icons[CW_ICON_COUNT];
    /* Границы видимой части картинки. В теме 120x120 с прозрачными
     * полями, поэтому масштабировать по размеру холста нельзя:
     * иконка получалась то мельче, то налезала на текст. */
    int         ibx[CW_ICON_COUNT], iby[CW_ICON_COUNT];
    int         ibw[CW_ICON_COUNT], ibh[CW_ICON_COUNT];
    gboolean    theme_ok;
    char       *theme_path;   /* найденный каталог, для диагностики */
    RsvgHandle *bg;           /* SVG-панель */
    gboolean    night;        /* сейчас ночь: влияет на выбор иконки */
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
    /* Что рисовать в нижней полосе: CW_VIEW_* */
    int         view;
    int         show_daytemp;
    int         use_bg;      /* 0 — дымчатое стекло, 1 — панель темы,
                                 2 — своя подложка */
    int         round_corner;/* радиус скругления окна, px */
    double      scale;      /* вёрстка от высоты окна, см. CW_REF_H */
    /* Каталог с иконками и SVG-фоном. Пусто -> ищем по умолчанию. */
    char       *theme_dir;

    int    width, height;
    char  *city_font, *temp_font, *desc_font, *hour_font;
    CwColor city_color, temp_color, desc_color, hour_color, bg_color;
    /* Кнопка цвета подложки: гасится для панели темы (use_bg==1) */
    GtkWidget *bg_color_btn;
} CwPriv;

static void cw_properties(XsPlugin *p, GtkNotebook *nb);
static void cw_start(XsPlugin *p);
static const char *cw_query(const CwPriv *priv);
static int cw_text(cairo_t *cr, PangoLayout *layout, const char *text,
                   CwColor *c, double x, double y, gboolean center);
static int cw_text_markup(cairo_t *cr, PangoLayout *layout, const char *text,
                          CwColor *c, double x, double y, gboolean center);
static int cw_text_markup_right(cairo_t *cr, PangoLayout *layout,
                                const char *text, CwColor *c, double xr,
                                double y);
static void cw_icon(cairo_t *cr, int kind, double x, double y, double s,
                    CwColor *c);
static void cw_icon_px(cairo_t *cr, CwPriv *priv, int kind, double x,
                       double y, double s);
static double cw_temp(const CwPriv *priv, double celsius);
static void cw_draw_days(cairo_t *cr, CwPriv *priv, CwWeather *cw,
                         PangoLayout *layout, double w, double h, double y);

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

/* Коды weather.com (их же отдаёт wttr.in) -> номера PNG в теме Stardock.
 * Родной апплет здесь делал str(code) и искал 113.png, которого в теме нет:
 * тема пронумерована 0..48. Карта составлена по содержимому картинок.
 * В ICON_NIGHT — варианты с луной для ясной и облачной погоды. */
static const guint8 CW_ICON_DAY[][2] = {
    {113,32},{116,28},{119,27},{122,26},{143,21},{176,8},{179,14},
    {182,7},{185,10},{200,4},{227,43},{230,15},{248,20},{260,10},
    {263,8},{266,8},{281,10},{284,12},{293,9},{296,9},{299,11},{302,12},
    {305,12},{308,12},{311,10},{314,12},{317,7},{320,7},{323,14},{326,14},
    {329,15},{332,15},{335,15},{338,15},{350,6},{353,9},{356,12},{359,12},
    {362,7},{365,7},{368,14},{371,15},{374,6},{377,6},{386,3},{389,4},
    {392,17},{395,18},
};

static const guint8 CW_ICON_NIGHT[][2] = {
    {113,31},{116,29},{119,33},{122,33},{143,21},{176,9},{179,46},
    {182,7},{185,10},{200,47},{227,43},{230,15},{248,20},{260,10},
    {263,9},{266,9},{281,10},{284,12},{293,9},{296,9},{299,11},{302,12},
    {305,12},{308,12},{311,10},{314,12},{317,7},{320,7},{323,14},{326,14},
    {329,15},{332,15},{335,15},{338,15},{350,6},{353,9},{356,12},{359,12},
    {362,7},{365,7},{368,14},{371,15},{374,6},{377,6},{386,47},{389,47},
    {392,47},{395,47},
};

/* Наша общая шкала -> код weather.com, по которому ищем картинку.
 * Без этого моста cw_icon_px() получал наш kind вместо кода, карта не
 * находила ничего и все иконки были N/A. */
static int cw_kind_to_ww(int kind)
{
    switch (kind) {
    case CW_W_CLEAR:    return 113;
    case CW_W_PARTLY:   return 116;
    case CW_W_CLOUDY:   return 119;
    case CW_W_FOG:      return 248;
    case CW_W_DRIZZLE:  return 266;
    case CW_W_RAIN:     return 293;
    case CW_W_SHOWERS:  return 353;
    case CW_W_SNOW:     return 326;
    case CW_W_THUNDER:  return 200;
    default:            return 3200;
    }
}

/* Код 3200 и всё неизвестное -> 48 (N/A). */
static int cw_icon_for(int code, gboolean night)
{
    const guint8 (*tbl)[2] = night ? CW_ICON_NIGHT : CW_ICON_DAY;
    gsize i;

    if (code == 3200)
        return 48;
    for (i = 0; i < G_N_ELEMENTS(CW_ICON_DAY); i++) {
        if (tbl[i][0] == code)
            return tbl[i][1];
    }
    return 48;
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

static void cw_day_clear(gpointer data)
{
    g_free(((CwDay *)data)->label);
}

static void cw_weather_free(CwWeather *w)
{
    if (!w)
        return;
    g_free(w->place);
    g_free(w->desc);
    if (w->hours)
        g_array_unref(w->hours);
    /* Метки дней освобождает cw_day_clear сам, из clear_func массива.
     * Свой цикл g_free поверх давал двойное освобождение и abort. */
    if (w->days)
        g_array_unref(w->days);
    g_free(w);
}

/* "YYYY-MM-DD" -> GDateTime в UTC. Даты у обоих агрегаторов без
 * времени и без зоны; важен только день недели, а он от часов
 * не зависит, поэтому полдень вместо полуночи безопасен. */
static GDateTime *cw_parse_day(const char *ymd)
{
    if (!ymd || strlen(ymd) < 10)
        return NULL;
    return g_date_time_new_utc(atoi(ymd), atoi(ymd + 5), atoi(ymd + 8),
                               12, 0, 0.0);
}

/* Русские сокращения дней недели. */
static const char *cw_weekday_ru(const GDateTime *dt)
{
    static const char *WD[7] = {"Вс","Пн","Вт","Ср","Чт","Пт","Сб"};

    return WD[g_date_time_get_day_of_week(dt) % 7];
}

static CwWeather *cw_weather_new(void)
{
    CwWeather *w = g_new0(CwWeather, 1);

    w->hours = g_array_new(FALSE, FALSE, sizeof(CwHour));
    g_array_set_clear_func(w->hours, cw_hour_clear);
    w->days = g_array_new(FALSE, FALSE, sizeof(CwDay));
    g_array_set_clear_func(w->days, cw_day_clear);
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
    /* Пока идёт работа или есть ошибка, показывать имя ответившего
     * агрегатора неправильно — снимаем бейдж, его вернёт успех. */
    if (*priv->status && *priv->badge) {
        g_free(priv->badge);
        priv->badge = g_strdup("");
    }
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
        "&daily=weather_code,temperature_2m_max,temperature_2m_min"
        "&forecast_days=%d&timezone=auto",
        lat, lon, priv->view == CW_VIEW_HOURS ? 2 : CW_DAYS_MAX);
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
    /* daily: для режима «6 дней». */
    if (json_object_has_member(obj, "daily")) {
        JsonObject *daily = json_object_get_object_member(obj, "daily");
        JsonArray *dtimes = json_object_get_array_member(daily, "time");
        JsonArray *dmax   = json_object_get_array_member(daily,
                                            "temperature_2m_max");
        JsonArray *dmin   = json_object_get_array_member(daily,
                                            "temperature_2m_min");
        JsonArray *dcodes = json_object_get_array_member(daily,
                                            "weather_code");

        if (dtimes) {
            len = (int)json_array_get_length(dtimes);
            for (i = 0; i < len && w->days->len < CW_DAYS_MAX; i++) {
                JsonNode *nt = json_array_get_element(dtimes, i);
                JsonNode *nv = dmax ? json_array_get_element(dmax, i) : NULL;
                JsonNode *nl = dmin ? json_array_get_element(dmin, i) : NULL;
                JsonNode *nc = dcodes ? json_array_get_element(dcodes, i)
                                      : NULL;
                const char *t = nt ? json_node_get_string(nt) : NULL;
                CwDay d;
                GDateTime *dt;

                if (!t)
                    continue;
                memset(&d, 0, sizeof(d));
                dt = cw_parse_day(t);
                d.label = g_strdup(dt ? cw_weekday_ru(dt) : "--");
                if (dt)
                    g_date_time_unref(dt);
                d.tmax = nv ? json_node_get_double(nv) : 0.0;
                d.tmin = nl ? json_node_get_double(nl) : 0.0;
                d.kind = nc ? cw_wmo_kind((int)json_node_get_int(nc))
                            : CW_W_N;
                g_array_append_val(w->days, d);
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
    /* weather[] — сутки. В ответе их 3, поэтому до CW_DAYS_MAX
     * не добираем, а полоса рисует сколько есть. */
    if (json_object_has_member(obj, "weather")) {
        JsonArray *days = json_object_get_array_member(obj, "weather");
        guint n = days ? json_array_get_length(days) : 0;

        for (guint k = 0; k < n && w->days->len < CW_DAYS_MAX; k++) {
            JsonNode *dn = json_array_get_element(days, k);
            JsonObject *dobj = dn && JSON_NODE_HOLDS_OBJECT(dn)
                                   ? json_node_get_object(dn) : NULL;
            CwDay d;
            GDateTime *dt;
            guint rain = 0;

            if (!dobj)
                continue;
            memset(&d, 0, sizeof(d));
            dt = cw_parse_day(json_object_has_member(dobj, "date")
                              ? json_object_get_string_member(dobj, "date")
                              : NULL);
            d.label = g_strdup(dt ? cw_weekday_ru(dt) : "--");
            if (dt)
                g_date_time_unref(dt);
            d.tmax = cw_num(dobj, "maxtempC", 0.0);
            d.tmin = cw_num(dobj, "mintempC", 0.0);
            /* иконка дня берём из полудня, как в родном апплете */
            if (json_object_has_member(dobj, "hourly")) {
                JsonArray *hh = json_object_get_array_member(dobj, "hourly");
                guint hn = hh ? json_array_get_length(hh) : 0;

                for (guint q = 0; q < hn; q++) {
                    JsonNode *h = json_array_get_element(hh, q);
                    JsonObject *ho = h && JSON_NODE_HOLDS_OBJECT(h)
                                         ? json_node_get_object(h) : NULL;
                    int r;

                    if (!ho)
                        continue;
                    r = cw_int(ho, "chanceofrain", 0);
                    if ((guint)r > rain)
                        rain = (guint)r;
                }
                if (hn > 4) {
                    JsonNode *noon = json_array_get_element(hh, 4);
                    JsonObject *no = noon && JSON_NODE_HOLDS_OBJECT(noon)
                                         ? json_node_get_object(noon) : NULL;

                    if (no)
                        d.kind = cw_ww_kind(cw_int(no, "weatherCode", -1));
                }
            }
            if (d.kind == CW_W_N)
                d.kind = CW_W_CLOUDY;
            d.rain = rain;
            g_array_append_val(w->days, d);
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
    /* Ночь влияет на выбор иконки: в теме есть варианты с луной.
     * Оба агрегатора отдают местное время наблюдения, сравниваем
     * с локальным же now. */
    {
        GDateTime *now = g_date_time_new_now_local();
        int hr = now ? g_date_time_get_hour(now) : 12;

        if (now)
            g_date_time_unref(now);
        w->night = (hr >= 20 || hr < 6);
        priv->night = w->night;
    }
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
    /* В угол окна — только имя источника: полная строка с транспортом
     * не влезала рядом с ветром и налезала на влажность. */
    g_free(priv->badge);
    priv->badge = g_strdup(source == CW_SRC_OPENMETEO ? "open-meteo"
                                                      : "wttr.in");
    cw_status(priv, "%s", "");
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

static double cw_scale = 1.0;   /* действует на время отрисовки */
static double cw_strip_gap = 3.0; /* промежуток в полосе прогноза */

static PangoFontDescription *cw_font(const char *spec)
{
    PangoFontDescription *d = pango_font_description_from_string(
        spec && spec[0] ? spec : "Sans 10");
    int pt;

    if (!d)
        d = pango_font_description_from_string("Sans 10");
    if (cw_scale > 0.01 && cw_scale != 1.0) {
        pt = pango_font_description_get_size(d) / PANGO_SCALE;
        pango_font_description_set_absolute_size(
            d, (double)(int)(pt * cw_scale) * PANGO_SCALE);
    }
    return d;
}

/* Ищем каталог темы: сначала путь из настроек, потом наш каталог рядом
 * с плагином, потом тема родного питоновского апплета — чтобы всё
 * работало даже без установки нашей копии темы. */
static char *cw_find_theme(const char *configured)
{
    char *own = NULL;
    char *found = NULL;
    const char *home = g_getenv("HOME");
    char *c1 = NULL, *c2 = NULL;
    gsize i;
    const char *cands[3];

    if (home)
        own = g_build_filename(home, "lib", "xscreenlets", "plugins",
                               "clearweather_theme", NULL);
    c1 = own;
    c2 = g_strdup("/usr/share/screenlets/ClearWeather/themes/default");

    cands[0] = (configured && configured[0]) ? configured : NULL;
    cands[1] = c1;
    cands[2] = c2;

    for (i = 0; i < G_N_ELEMENTS(cands); i++) {
        char *probe;

        if (!cands[i])
            continue;
        probe = g_build_filename(cands[i], "0.png", NULL);
        if (g_file_test(probe, G_FILE_TEST_EXISTS))
            found = g_strdup(cands[i]);
        g_free(probe);
        if (found)
            break;
    }
    g_free(c1);
    g_free(c2);
    return found;
}

/* Прямоугольник непрозрачной части картинки. Считается один раз при
 * загрузке: 49 картинок по 120x120 — это меньше миллиона пикселей. */
static void cw_icon_bounds(GdkPixbuf *pb, int idx, CwPriv *priv)
{
    int w, h, ch, stride, x, y;
    int x0 = -1, y0 = -1, x1 = -1, y1 = -1;
    const guchar *px;

    if (!pb)
        return;
    w = gdk_pixbuf_get_width(pb);
    h = gdk_pixbuf_get_height(pb);
    ch = gdk_pixbuf_get_n_channels(pb);
    stride = gdk_pixbuf_get_rowstride(pb);
    px = gdk_pixbuf_get_pixels(pb);

    for (y = 0; y < h; y++) {
        const guchar *row = px + (gsize)y * stride;

        for (x = 0; x < w; x++) {
            if (row[x * ch + ch - 1] < 8)   /* альфа */
                continue;
            if (x0 < 0 || x < x0) x0 = x;
            if (x1 < 0 || x > x1) x1 = x;
            if (y0 < 0) y0 = y;
            y1 = y;
        }
    }
    if (x0 < 0) {              /* полностью прозрачная */
        x0 = y0 = 0;
        x1 = w - 1;
        y1 = h - 1;
    }
    priv->ibx[idx] = x0;
    priv->iby[idx] = y0;
    priv->ibw[idx] = x1 - x0 + 1;
    priv->ibh[idx] = y1 - y0 + 1;
}

/* Загружает 49 иконок и SVG-фон один раз. Тема не нашлась — апплет
 * продолжает рисовать символы кодом, как раньше. */
/* Родные SVG темы описаны только через width/height, без viewBox.
 * librsvg в таком случае рисует документ в его собственном размере
 * (132x100) и размер окна апплета полностью игнорирует — фон не
 * масштабируется. Поэтому перед загрузкой дописываем viewBox из
 * width/height.
 *
 * Дальше preserveAspectRatio. Панель сделана под холст 132x100, а окно
 * 320x169, и пропорции не совпадают. С "none" фон заполняет окно, но
 * рисунок тянется по двум осям, облака выходят деформированными, а
 * нижняя полоса панели (day-bg.svg, 116x9) превращается в серую
 * полосу поперёк апплета. С "slice" картинка не искажается, а лишнее
 * по меньшей стороне обрезается — вместе с этой полосой. */
static double cw_svg_attr(const char *tag, const char *name)
{
    const char *p = strstr(tag, name);
    char *end = NULL;
    double v;

    if (!p)
        return 0.0;
    p += strlen(name);
    while (*p == ' ' || *p == '=' || *p == '\n' || *p == '\t')
        p++;
    if (*p == '"')
        p++;
    v = g_ascii_strtod(p, &end);
    if (end == p)
        return 0.0;
    return v;
}

/* Возвращает границы корневого элемента <svg ...>. Файл начинается с
 * <?xml version="1.0"?>, и первый '>' в нём — конец XML-декларации, а
 * не конец тега: правка по нему вставляла viewBox внутрь <?xml, XML
 * не разбирался, rsvg_handle_new_from_data возвращал NULL, и фона не
 * было вовсе. Поэтому ищем именно '<svg' и идём до '>' оттуда, не
 * считая '>' внутри значений атрибутов. */
static gboolean cw_svg_root(const char *data, const char **beg,
                            const char **end)
{
    const char *p = data;
    const char *q;
    char quote = 0;

    for (;;) {
        p = strchr(p, '<');
        if (!p)
            return FALSE;
        if (strncmp(p, "<svg", 4) == 0) {
            *beg = p;
            break;
        }
        /* пропускаем <?xml ?>, <!-- -->, <!DOCTYPE ...> */
        q = p + 1;
        if (*q == '?') {
            p = strstr(p, "?>");
            if (!p)
                return FALSE;
            p += 2;
        } else if (strncmp(p, "<!--", 4) == 0) {
            p = strstr(p, "-->");
            if (!p)
                return FALSE;
            p += 3;
        } else {
            p = strchr(p, '>');
            if (!p)
                return FALSE;
            p++;
        }
    }
    for (q = *beg; *q; q++) {
        if (quote) {
            if (*q == quote)
                quote = 0;
            continue;
        }
        if (*q == '"' || *q == '"')
            quote = *q;
        else if (*q == '>') {
            *end = q + 1;
            return TRUE;
        }
    }
    return FALSE;
}

static RsvgHandle *cw_svg_load(const char *path)
{
    gchar *data = NULL;
    gsize len = 0;
    GString *out;
    RsvgHandle *h;
    char *tag, *ins;
    const char *beg, *end;
    double wv, hv;
    gsize at;

    if (!g_file_get_contents(path, &data, &len, NULL))
        return NULL;
    if (!g_utf8_validate(data, len, NULL) ||
        !cw_svg_root(data, &beg, &end)) {
        g_free(data);
        return NULL;
    }
    tag = g_strndup(beg, (gsize)(end - beg));
    wv = cw_svg_attr(tag, "width");
    hv = cw_svg_attr(tag, "height");
    if (wv <= 0.0)
        wv = 132.0;
    if (hv <= 0.0)
        hv = 100.0;

    /* вставляем прямо после имени корневого элемента */
    at = (gsize)(beg - data) + 4;
    out = g_string_new(NULL);
    g_string_append_len(out, data, (gssize)at);
    if (strstr(tag, "viewBox") == NULL) {
        ins = g_strdup_printf(" viewBox=\"0 0 %g %g\"", wv, hv);
        g_string_append(out, ins);
        g_free(ins);
    }
    if (strstr(tag, "preserveAspectRatio") == NULL)
        g_string_append(out, " preserveAspectRatio=\"xMidYMid slice\"");
    g_string_append(out, data + at);

    h = rsvg_handle_new_from_data((const guint8 *)out->str, out->len, NULL);
    g_string_free(out, TRUE);
    g_free(tag);
    g_free(data);
    return h;
}

static void cw_theme_load(CwPriv *priv)
{
    gsize i;

    if (priv->theme_ok)
        return;
    priv->theme_ok = TRUE;

    priv->theme_path = cw_find_theme(priv->theme_dir);
    if (!priv->theme_path) {
        cw_log(priv->plugin, "каталог темы не найден, рисую символами");
        return;
    }
    cw_log(priv->plugin, "тема: %s", priv->theme_path);

    for (i = 0; i < CW_ICON_COUNT; i++) {
        char *name = g_strdup_printf("%u.png", (guint)i);
        char *f = g_build_filename(priv->theme_path, name, NULL);

        priv->icons[i] = gdk_pixbuf_new_from_file(f, NULL);
        g_free(f);
        g_free(name);
        cw_icon_bounds(priv->icons[i], i, priv);
    }
    {
        char *svg = g_build_filename(priv->theme_path, "weather-bg.svg", NULL);

        if (g_file_test(svg, G_FILE_TEST_EXISTS))
            priv->bg = cw_svg_load(svg);
        g_free(svg);
    }
    if (!priv->bg) {
        char *svg = g_build_filename(priv->theme_path,
                                     "weather-bg-mini.svg", NULL);

        if (g_file_test(svg, G_FILE_TEST_EXISTS))
            priv->bg = cw_svg_load(svg);
        g_free(svg);
    }
}

static void cw_theme_free(CwPriv *priv)
{
    gsize i;

    for (i = 0; i < CW_ICON_COUNT; i++)
        g_clear_object(&priv->icons[i]);
    g_clear_object(&priv->bg);
    g_clear_pointer(&priv->theme_path, g_free);
}

/* Полоса на 6 дней — как в родном апплете: день недели, иконка
 * (ночная у первого, дневные дальше — ровно так же у него), затем
 * максимум и минимум одной строкой. Раскладка по ширине окна: колонки
 * отступом от левого края, как в оригинале, но считаются от w, а не
 * от жёстких 24 пикселей, иначе на 320 не помещается. */
static void cw_draw_days(cairo_t *cr, CwPriv *priv, CwWeather *cw,
                         PangoLayout *layout, double w, double h, double y)
{
    int n = (int)MIN(cw->days->len, (guint)CW_DAYS_MAX);
    double sc = w / 132.0;
    int i, th;
    CwColor faded = priv->hour_color;

    if (n <= 0)
        return;
    faded.a *= 0.85;

    /* Полоса дней по координатам оригинала (ClearWeatherScreenlet.py,
     * холст 132x100, шаг 24 по x):
     *   полоса под дни   translate(14, 60), day-bg 116x9
     *   дни недели       та же точка, кегль 6
     *   иконки дней      translate(14, 68), размер 22x22
     *   макс/мин         translate(16, 90), кегль 4
     * Ничего не растягиваем и не выравниваем по краям окна: оригинал
     * держит шаг 24 и начинает с x=14, и в этом его вид. */
    for (i = 0; i < n; i++) {
        CwDay *dh = &g_array_index(cw->days, CwDay, i);
        double x = 14.0 * sc + 24.0 * sc * i;
        char buf[32];
        PangoFontDescription *fd;

        fd = cw_font(priv->desc_font);
        pango_layout_set_font_description(layout, fd);
        pango_font_description_free(fd);
        g_snprintf(buf, sizeof(buf), "%s", dh->label ? dh->label : "--");
        th = cw_text(cr, layout, buf, &faded, x, y, FALSE);

        /* у первого дня родной апплет рисует ночную иконку, дальше
         * дневные — так и делаем */
        cw_icon_px(cr, priv, dh->kind, x, y + (8.0 * sc), 22.0 * sc);

        if (priv->show_daytemp) {
            /* В оригинале '<b>high</b>low' слитно, без разделителя */
            g_snprintf(buf, sizeof(buf), "<b>%.0f\u00b0</b>%.0f\u00b0",
                       cw_temp(priv, dh->tmax), cw_temp(priv, dh->tmin));
            fd = cw_font(priv->hour_font);
            pango_layout_set_font_description(layout, fd);
            pango_font_description_free(fd);
            cw_text_markup(cr, layout, buf, &priv->hour_color,
                           x + 2.0 * sc, y + (30.0 * sc), FALSE);
        }
    }
    (void)h;
}

/* Рисует иконку погоды: если тема загрузилась — настоящий PNG из набора
 * Stardock, иначе рисуем символ кодом, как раньше. Тот же рисователь
 * работает и в шапке, и в нижней полосе, и для 6-дневного режима. */
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

static void cw_icon_px(cairo_t *cr, CwPriv *priv, int kind, double x,
                       double y, double s)
{
    int idx = cw_icon_for(cw_kind_to_ww(kind), priv->night);

    if (priv->icons[idx]) {
        int bw = priv->ibw[idx], bh = priv->ibh[idx];
        double sc = s / (double)(bw > bh ? bw : bh);

        cairo_save(cr);
        /* Без cairo_scale паттерн рисуется 1:1, то есть «иконка на 17
         * пикселей» занимала бы все 120. Масштабируем по ВИДИМОЙ части,
         * сдвигая так, чтобы она попала в (x, y, s, s) — иначе иконка
         * либо налезала на текст, либо была мельче соседних. */
        cairo_translate(cr, x - priv->ibx[idx] * sc, y - priv->iby[idx] * sc);
        cairo_scale(cr, sc, sc);
        gdk_cairo_set_source_pixbuf(cr, priv->icons[idx], 0, 0);
        cairo_pattern_set_filter(cairo_get_source(cr), CAIRO_FILTER_GOOD);
        cairo_paint(cr);
        cairo_restore(cr);
        return;
    }
    cw_icon(cr, kind, x, y, s, &priv->temp_color);
}

/* Панель из weather-bg.svg. Родной апплет рисует её librsvg через
 * theme.render(); мы делаем то же самоим. */
/* «Тёмное дымчатое стекло» родного апплета. Там подложка собирается
 * композитом: чёрная база с альфой 0.8, weather-bg.svg, поверх
 * скруглённый прямоугольник-светлец, и снова weather-bg.svg. Сама
 * панель сделана под холст 132x100 и несёт изометрический поднос
 * (inkscape:persp3d), поэтому на окне 320x169 она и растягивается
 * криво, и обрезается не по делу — в неё вписаны ровно те четыре
 * элемента, что у оригинала. Здесь эффект рисуется кодом и от размера
 * окна не зависит. */
static void cw_smoky_glass(cairo_t *cr, double w, double h, double r)
{
    cairo_pattern_t *g;
    cairo_matrix_t m;

    cairo_save(cr);
    cairo_new_sub_path(cr);
    cairo_arc(cr, w - r, r, r, -M_PI / 2, 0);
    cairo_arc(cr, w - r, h - r, r, 0, M_PI / 2);
    cairo_arc(cr, r, h - r, r, M_PI / 2, M_PI);
    cairo_arc(cr, r, r, r, M_PI, 3 * M_PI / 2);
    cairo_close_path(cr);
    cairo_clip(cr);

    /* корпус: сверху чуть светлее, снизу почти чёрный */
    g = cairo_pattern_create_linear(0.0, 0.0, 0.0, h);
    cairo_pattern_add_color_stop_rgba(g, 0.0, 0.13, 0.13, 0.15, 0.93);
    cairo_pattern_add_color_stop_rgba(g, 0.45, 0.07, 0.07, 0.09, 0.94);
    cairo_pattern_add_color_stop_rgba(g, 1.0, 0.04, 0.04, 0.05, 0.95);
    cairo_set_source(cr, g);
    cairo_paint(cr);
    cairo_pattern_destroy(g);

    /* блик по верхней кромке — стекло «светится» сверху */
    g = cairo_pattern_create_linear(0.0, 0.0, 0.0, h * 0.34);
    cairo_pattern_add_color_stop_rgba(g, 0.0, 1.0, 1.0, 1.0, 0.16);
    cairo_pattern_add_color_stop_rgba(g, 0.45, 1.0, 1.0, 1.0, 0.05);
    cairo_pattern_add_color_stop_rgba(g, 1.0, 1.0, 1.0, 1.0, 0.0);
    cairo_set_source(cr, g);
    cairo_paint(cr);
    cairo_pattern_destroy(g);

    /* косой отблеск, как у стекла под углом */
    cairo_matrix_init_rotate(&m, -0.28);
    cairo_pattern_set_matrix(g = cairo_pattern_create_linear(0.0, 0.0, w * 0.75, h), &m);
    cairo_pattern_add_color_stop_rgba(g, 0.00, 1.0, 1.0, 1.0, 0.00);
    cairo_pattern_add_color_stop_rgba(g, 0.42, 1.0, 1.0, 1.0, 0.045);
    cairo_pattern_add_color_stop_rgba(g, 0.52, 1.0, 1.0, 1.0, 0.00);
    cairo_set_source(cr, g);
    cairo_paint(cr);
    cairo_pattern_destroy(g);

    /* мягкое затемнение у нижней кромки, чтобы панель «садилась» */
    g = cairo_pattern_create_linear(0.0, h * 0.72, 0.0, h);
    cairo_pattern_add_color_stop_rgba(g, 0.0, 0.0, 0.0, 0.0, 0.0);
    cairo_pattern_add_color_stop_rgba(g, 1.0, 0.0, 0.0, 0.0, 0.30);
    cairo_set_source(cr, g);
    cairo_paint(cr);
    cairo_pattern_destroy(g);

    /* рамка: светлая сверху, тёмная снизу — объём */
    cairo_new_sub_path(cr);
    cairo_arc(cr, w - r, r, r, -M_PI / 2, 0);
    cairo_arc(cr, w - r, h - r, r, 0, M_PI / 2);
    cairo_arc(cr, r, h - r, r, M_PI / 2, M_PI);
    cairo_arc(cr, r, r, r, M_PI, 3 * M_PI / 2);
    cairo_close_path(cr);
    g = cairo_pattern_create_linear(0.0, 0.0, 0.0, h);
    cairo_pattern_add_color_stop_rgba(g, 0.0, 1.0, 1.0, 1.0, 0.30);
    cairo_pattern_add_color_stop_rgba(g, 0.5, 1.0, 1.0, 1.0, 0.10);
    cairo_pattern_add_color_stop_rgba(g, 1.0, 0.0, 0.0, 0.0, 0.25);
    cairo_set_source(cr, g);
    cairo_set_line_width(cr, 1.0);
    cairo_stroke(cr);
    cairo_pattern_destroy(g);
    cairo_restore(cr);
}

/* Оригинальная подложка ClearWeather, как в ClearWeatherScreenlet.py:
 *   set_source_rgba(0,0,0,0.8)
 *   render('weather-bg')
 *   draw_rounded_rectangle(11.5, 18.5, 8, 120, 80)
 *   render('weather-bg')
 * Двойной прогон и есть источник «дымчатости»: полупрозрачные слои
 * панели складываются вдвое, и на чёрной базе получается глубина. Один
 * прогон даёт плоскую заливку — ровно то, что выглядело не тем.
 * Панель сделана под холст 132x100, поэтому её пропорции и положение
 * прямоугольника пересчитываем в окно. */
static void cw_native_bg(cairo_t *cr, CwPriv *priv, double w, double h)
{
    double sx = w / 132.0, sy = h / 100.0;
    double rx = 11.5 * sx, ry = 18.5 * sy, rr = 8.0 * sx;
    double rw = 120.0 * sx, rh = 80.0 * sy;
    RsvgRectangle vp = {0.0, 0.0, (double)w, (double)h};

    /* База (0,0,0,0.8) — как в оригинале: сквозь неё видно рабочий
     * стол, и в этом «дымчатое стекло». Окно у нас и правда создано с
     * RGBA-visual, ядро чистит его через OPERATOR_CLEAR и альфу не
     * затирает, так что прозрачность терялась только здесь.
     *
     * Заливка под панелью ломала стекло: окно становилось непрозрачным
     * и чёрным. Теперь базы нет, прозрачность даёт сама панель. */
    /* База нулевая: прозрачность обеспечивает сама панель, своими
     * градиентами со stop-opacity. Заливка под ней превращала окно в
     * непрозрачный чёрный прямоугольник. */
    cairo_set_source_rgba(cr, 0.0, 0.0, 0.0, 0.0);
    cairo_paint(cr);

    if (priv->bg) {
        rsvg_handle_render_document(priv->bg, cr, &vp, NULL);

        /* скруглённый прямоугольник между прогонами */
        cairo_save(cr);
        cairo_new_sub_path(cr);
        cairo_arc(cr, rx + rw - rr, ry + rr, rr, -M_PI / 2, 0);
        cairo_arc(cr, rx + rw - rr, ry + rh - rr, rr, 0, M_PI / 2);
        cairo_arc(cr, rx + rr, ry + rh - rr, rr, M_PI / 2, M_PI);
        cairo_arc(cr, rx + rr, ry + rr, rr, M_PI, 3 * M_PI / 2);
        cairo_close_path(cr);
        /* Заливка плиты. В ClearWeatherScreenlet.py.background_color =
         * (0,0,0,0.8), и цвет попадает в плиту именно здесь: перед
         * draw_rounded_rectangle стоит set_source_rgba(*background_color),
         * а тот заливает текущим источником. Ключевое отличие от нашей
         * прежней заливки: затемняется только плита (11.5,18.5,120,80),
         * а не всё окно — поэтому сверху, где висит крупная иконка,
         * и по полям остаётся видно рабочий стол. Отсюда и стекло.
         *
         * Замеры на соседних окнах (фон 16): оригинал даёт медиану 13
         * и 25-й перцентиль 2, то есть тени настоящие; при нашей
         * заливке 0.55/0.10 выходило 26 и 11 — тени были вымыты, и
         * стекло читалось как молочное. */
        cairo_set_source_rgba(cr, 0.0, 0.0, 0.0, 0.8);
        cairo_fill_preserve(cr);
        cairo_set_source_rgba(cr, 0.80, 0.80, 0.85, 0.22);
        cairo_set_line_width(cr, 1.0);
        cairo_stroke(cr);
        cairo_restore(cr);

        /* второй прогон — слои панели ложатся вдвое */
        rsvg_handle_render_document(priv->bg, cr, &vp, NULL);
    }
}

static void cw_draw_bg(cairo_t *cr, CwPriv *priv, double w, double h)
{
    double r = priv->round_corner > 0 ? (double)priv->round_corner : 0.0;

    cairo_save(cr);
    /* Скруглённый путь строится всегда: по нему клипуется и подложка,
     * и панель темы, а рамка обводится поверх. Раньше при включённом
     * фоне был ранний return, и окно выходило обычным прямоугольником
     * без скруглений. */
    cairo_new_sub_path(cr);
    cairo_arc(cr, w - r, r, r, -M_PI / 2, 0);
    cairo_arc(cr, w - r, h - r, r, 0, M_PI / 2);
    cairo_arc(cr, r, h - r, r, M_PI / 2, M_PI);
    cairo_arc(cr, r, r, r, M_PI, 3 * M_PI / 2);
    cairo_close_path(cr);

    if (priv->use_bg == 1) {
        /* Панель темы. Фон и прозрачность здесь жёстко нулевые: панель
         * рисует всё сама, и любая подложка под ней только забивает
         * альфу. Раньше под панель клалась база (0,0,0,0.20), из-за
         * чего окно читалось непрозрачным. Теперь сквозь панель видно
         * рабочий стол, и настраивать тут нечего. */
        cairo_clip(cr);
        cw_native_bg(cr, priv, w, h);
    } else if (priv->use_bg == 2) {
        /* Своя подложка — настраиваемый ARGB */
        cairo_set_source_rgba(cr, priv->bg_color.r, priv->bg_color.g,
                              priv->bg_color.b, priv->bg_color.a);
        cairo_fill_preserve(cr);
    } else {
        /* «Тёмное дымчатое стекло» — как в родном апплете */
        cairo_restore(cr);
        cw_smoky_glass(cr, w, h, r);
        return;
    }
    if (r <= 0.0)
        cairo_new_sub_path(cr);
    cairo_set_source_rgba(cr, 0.45, 0.45, 0.50, 0.75);
    cairo_set_line_width(cr, 1.0);
    cairo_stroke(cr);
    cairo_restore(cr);
}


/* Рисует текст и ВОЗВРАЩАЕТ его фактическую высоту в пикселях.
 * Высота нужна, чтобы складывать строки друг под другом: при фиксированных
 * смещениях крупный шрифт температуры налезал на город. */
/* Выравнивание по правому краю: PANGO_ALIGN_RIGHT внутри layout
 * работает только относительно ширины самого layout, поэтому ширину
 * текста меряем сами и рисуем от правого края окна. */
/* Текст с pango-разметкой. Нужен для пары макс/мин: в родном
 * апплете это '<b>high</b>low' — жирный максимум и обычный минимум
 * без разделителя, поэтому они и разного цвета. */
static int cw_text_markup(cairo_t *cr, PangoLayout *layout, const char *text,
                          CwColor *c, double x, double y, gboolean center)
{
    int tw = 0, th = 0;

    pango_layout_set_markup(layout, text ? text : "", -1);
    pango_layout_get_pixel_size(layout, &tw, &th);
    cairo_set_source_rgba(cr, c->r, c->g, c->b, c->a);
    pango_layout_set_alignment(layout, PANGO_ALIGN_LEFT);
    cairo_move_to(cr, center ? x - tw * 0.5 : x, y);
    pango_cairo_show_layout(cr, layout);
    return th;
}

/* То же, что cw_text_markup, но прижато к правому краю по x. */
static int cw_text_markup_right(cairo_t *cr, PangoLayout *layout,
                                const char *text, CwColor *c, double xr,
                                double y)
{
    int tw = 0, th = 0;

    pango_layout_set_markup(layout, text ? text : "", -1);
    pango_layout_get_pixel_size(layout, &tw, &th);
    cairo_set_source_rgba(cr, c->r, c->g, c->b, c->a);
    pango_layout_set_alignment(layout, PANGO_ALIGN_LEFT);
    cairo_move_to(cr, xr - tw, y);
    pango_cairo_show_layout(cr, layout);
    return th;
}

/* Нижняя граница видимой части значка. Рамка 72 px, а рисунок
 * кончается выше: поля в картинках темы прозрачные. Считать низ
 * шапки по рамке — значит тащить за собой пустое поле. */
static double cw_icon_visible_bottom(CwPriv *priv, int idx, double size)
{
    int bw, bh, base;

    if (idx < 0 || idx >= CW_ICON_COUNT || !priv->icons[idx])
        return size;
    bw = priv->ibw[idx];
    bh = priv->ibh[idx];
    if (bw <= 0 || bh <= 0)
        return size;
    base = bw > bh ? bw : bh;
    return (priv->iby[idx] + bh) * size / (double)base;
}

/* Ширина строки в пикселях — чтобы решить, влезает ли она в окно. */
static int cw_text_width(cairo_t *cr, PangoLayout *layout, const char *font,
                         const char *text, CwColor c)
{
    PangoFontDescription *fd = cw_font(font);
    int tw = 0, th = 0;

    pango_layout_set_font_description(layout, fd);
    pango_font_description_free(fd);
    pango_layout_set_text(layout, text ? text : "", -1);
    pango_layout_get_pixel_size(layout, &tw, &th);
    cairo_set_source_rgba(cr, c.r, c.g, c.b, c.a);
    return tw;
}

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
    pango_layout_set_alignment(layout, PANGO_ALIGN_LEFT);
    /* Настоящее центрирование по x. PANGO_ALIGN_CENTER внутри layout
     * бесполезен: ширина layout равна ширине текста, центрировать не
     * по чему, и текст начинался прямо от точки — колонки сдвигались. */
    cairo_move_to(cr, center ? x - tw * 0.5 : x, y);
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
    double y, icon_s, x, text_x, wind_y = 0.0, cw_strip_h;
    int i, shown, th, th2;

    if (!priv)
        return;

    /* Вся вёрстка масштабируется одним множителем от ширины, ровно
     * как в оригинале: там холст 132x100 и ctx.scale на всё. Координаты
     * ниже взяты из кода ClearWeatherScreenlet.py как есть и умножаются
     * на этот множитель. Считать от высоты 169 было ошибкой: кегли
     * выходили в 1.7 раза мельче родовых, и крупная иконка переставала
     * быть доминантой. */
    cw_scale = w / 132.0;
    priv->scale = cw_scale;

    surf = cairo_image_surface_create(CAIRO_FORMAT_ARGB32, w, h);
    g = cairo_create(surf);
    cairo_set_operator(g, CAIRO_OPERATOR_SOURCE);
    cairo_set_source_rgba(g, 0, 0, 0, 0);
    cairo_paint(g);
    cairo_set_operator(g, CAIRO_OPERATOR_OVER);

    /* Собственная рамка апплета рисуется только когда фоном
     * владеет сам апплет. При панели темы рамку даёт она сама, и
     * вместе со своей получались две рамки одна в другой: рамка
     * апплета по краю окна и рамка панели внутри неё. */
    if (priv->use_bg != 1) {
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
    }

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
    /* --- шапка по образцу родного апплета ---
     *
     * Оригинал: крупная иконка слева (масштаб 0.6 от 120), а справа
     * столбец, прижатый к правому краю. Слов нет вообще — только
     * числа и название города. */
    cw_theme_load(priv);
    cw_draw_bg(g, priv, w, h);

    /* Значок шапки — от ширины окна. Окно 320x242 повторяет
     * пропорции оригинала 132x100, и при жёстких 72 px шапка занимала
     * бы меньше трети высоты, а снизу оставалась дыра. */
    /* Шапка по координатам оригинала, умноженным на cw_scale.
     *
     * Из ClearWeatherScreenlet.py, логические координаты холста 132x100:
     *   иконка       translate(-2, 0); scale(.6, .6) из 120x120 -> 72px
     *   температура  draw_text(temp, x=90, y=25, кегль 14)
     *   город        draw_text(where, x=-5, y=50, кегль 6, вправо)
     *
     * Отступов тут нет намеренно: в оригинале иконка нависает выше
     * пластины, а пластину потом перекрывает полоса дней. Раньше я
     * «прижимал» элементы к углам и разводил их логикой — отсюда
     * рваный ритм, который и читался как чужеродно. */
    icon_s = 72.0 * cw_scale;
    cw_icon_px(g, priv, cw->kind, -2.0 * cw_scale, 0.0, icon_s);

    {
        PangoFontDescription *fd;

        fd = cw_font(priv->temp_font);
        pango_layout_set_font_description(layout, fd);
        pango_font_description_free(fd);
        g_snprintf(buf, sizeof(buf), "%.0f\u00b0", cw_temp(priv, cw->temp));
        th = cw_text(g, layout, buf, &priv->temp_color,
                     90.0 * cw_scale, 25.0 * cw_scale, FALSE);

        fd = cw_font(priv->city_font);
        pango_layout_set_font_description(layout, fd);
        pango_font_description_free(fd);
        /* в оригинале город выровнен вправо по рамке шириной 132,
         * начинающейся с x=-5, то есть правый край текста на 127 */
        th2 = cw_text_right(g, layout, cw->place ? cw->place : priv->city,
                            &priv->city_color, 127.0 * cw_scale,
                            50.0 * cw_scale);
    }

    /* Низ пластины задаёт полоса дней, а не шапка: в оригинале они
     * перекрываются. Ничего не добавляем, только считаем, где начнётся
     * полоса. */
    y = 60.0 * cw_scale;
    cw_strip_h = h - y;

    /* --- почасовой прогноз ---
     *
     * Пока идёт запрос или есть ошибка, вместо прогноза показывается
     * сообщение: длинные тексты («город не найден», «пробую дальше»)
     * не влезали в угол и налезали на строку ветра. */
    if (priv->status && priv->status[0]) {
        CwColor warn = priv->desc_color;
        const char *nl;

        warn.a *= 0.9;
        /* многострочное сообщение рисуем построчно, \n не обрабатывается */
        nl = priv->status;
        for (const char *line = nl; line; ) {
            const char *eol = strchr(line, '\n');

            if (eol) {
                char *one = g_strndup(line, eol - line);

                cw_text(g, layout, one, &warn, CW_MARGIN, y, FALSE);
                g_free(one);
                line = eol + 1;
            } else {
                cw_text(g, layout, line, &warn, CW_MARGIN, y, FALSE);
                line = NULL;
            }
            y += 14;
        }
    } else if (priv->view == CW_VIEW_DAYS) {
        cw_draw_days(g, priv, cw, layout, w, cw_strip_h, y);
    } else if (cw->hours->len > 0) {
        int n = (int)MIN(cw->hours->len, MAX(priv->hours_shown, 1));
        double col = (w - 2 * CW_MARGIN) / n;
        /* Зазор 3 px отсюда и получить нельзя: видимая часть картинки
         * занимает ~89% от запрошенного размера, поля прозрачные. При
         * isz = col-3 видимая ширина выходила 32 px и зазор 6 px.
         * Поэтому берём колонку с небольшим добавом — рамка шире
         * колонки, но прозрачные поля не дают значкам соприкасаться. */
        double isz = CLAMP(col + 1.5, 12.0, 40.0);

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
            /* Высота часа и высота температуры — разные: если считать
             * обе от y, значок встаёт НАД нижним краем температуры. */
            th2 = cw_text(g, layout, buf, &priv->hour_color, x,
                          y + th + 2, TRUE);
            cw_icon_px(g, priv, hh->kind, x - isz / 2.0,
                       y + th + 2 + th2 + 5, isz);
            shown++;
        }
    }

    /* Бейдж источника убран: в родном апплете его нет, и на нашей
     * шапке он занимал правый верхний угол, где стоит температура. */


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

static void cw_on_view(GtkComboBox *c, gpointer data)
{
    XsPlugin *p = data;
    CwPriv *priv = p ? p->priv : NULL;
    int v;

    if (!priv)
        return;
    v = gtk_combo_box_get_active(c);
    if (v < 0 || v == priv->view)
        return;
    priv->view = v;
    xs_host_api()->conf_set_int(priv->kf, p->name, "view", v);
    cw_save(priv);
    /* Число запрошенных дней у open-meteo зависит от режима, поэтому
     * данные перезапрашиваем, а не просто перерисовываем. */
    cw_start(p);
}

static void cw_on_round_corner(GtkSpinButton *sp, gpointer data)
{
    XsPlugin *p = data;
    CwPriv *priv = p ? p->priv : NULL;

    if (!priv)
        return;
    priv->round_corner = gtk_spin_button_get_value_as_int(sp);
    xs_host_api()->conf_set_int(priv->kf, p->name, "round_corner",
                                priv->round_corner);
    cw_save(priv);
    if (p->win)
        gtk_widget_queue_draw(p->win);
}

static void cw_on_use_bg(GtkComboBox *cb, gpointer data)
{
    XsPlugin *p = data;
    CwPriv *priv = p ? p->priv : NULL;
    int v;

    if (!priv)
        return;
    v = gtk_combo_box_get_active(cb);
    if (v < 0)
        return;
    priv->use_bg = v;
    xs_host_api()->conf_set_int(priv->kf, p->name, "use_bg", v);
    /* У панели темы фон и прозрачность жёстко нулевые — она всё рисует
     * сама, и настраивать тут нечего. Для остальных режимов цвет фона
     * настраивается, вместе с альфой. */
    if (priv->bg_color_btn)
        gtk_widget_set_sensitive(priv->bg_color_btn, v != 1);
    cw_save(priv);
    if (p->win)
        gtk_widget_queue_draw(p->win);
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
    /* Пояснение строчно: одним длинным label окно настроек растягивало
     * на всю ширину этой строки. */
    {
        static const char *const notes[] = {
            "Название можно писать на любом языке;",
            "если не нашлось — берётся запасное.",
            "Вместо города можно указать почтовый индекс.",
            "Соединение: напрямую или через адрес ниже.",
        };
        gsize k;

        for (k = 0; k < G_N_ELEMENTS(notes); k++) {
            GtkWidget *lb = gtk_label_new(notes[k]);

            gtk_label_set_xalign(GTK_LABEL(lb), 0.0);
            gtk_box_pack_start(GTK_BOX(page), lb, FALSE, FALSE, 0);
        }
    }
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

    {
        static const char *views[] = {"6 дней (как в родном)",
                                      "По часам",
                                      "Дни и часы"};

        cb = gtk_combo_box_text_new();
        for (guint i = 0; i < G_N_ELEMENTS(views); i++)
            gtk_combo_box_text_append_text(GTK_COMBO_BOX_TEXT(cb), views[i]);
        gtk_combo_box_set_active(GTK_COMBO_BOX(cb), priv->view);
        cw_tag(cb, "view");
        g_signal_connect(cb, "changed", G_CALLBACK(cw_on_view), p);
        cw_row(grid, r++, "Вид прогноза:", cb);
    }

    {
        static const char *const bgs[] = {
            "Тёмное дымчатое стекло",
            "Панель темы (родной апплет)",
            "Своя подложка",
        };
        GtkWidget *cb = gtk_combo_box_text_new();
        gsize k;

        for (k = 0; k < G_N_ELEMENTS(bgs); k++)
            gtk_combo_box_text_append_text(GTK_COMBO_BOX_TEXT(cb), bgs[k]);
        gtk_combo_box_set_active(GTK_COMBO_BOX(cb), priv->use_bg);
        cw_tag(cb, "use_bg");
        g_signal_connect(cb, "changed", G_CALLBACK(cw_on_use_bg), p);
        cw_row(grid, r++, "Фон:", cb);
    }
    {
        /* Цвет подложки с альфой — ARGB. У режима «панель темы» он не
         * применяется, и тогда строка гасится, чтобы не вводить в
         * заблуждение, что настройка что-то меняет. */
        GtkWidget *b = cw_color_button(&priv->bg_color, "background_color");

        priv->bg_color_btn = b;
        gtk_widget_set_sensitive(b, priv->use_bg != 1);
        cw_row(grid, r++, "Цвет подложки (ARGB):", b);
    }
    {
        /* Скругление окна: радиус в пикселях, 0 — прямые углы */
        GtkWidget *sc = gtk_spin_button_new_with_range(0, 40, 1);

        gtk_spin_button_set_value(GTK_SPIN_BUTTON(sc), priv->round_corner);
        cw_tag(sc, "round_corner");
        g_signal_connect(sc, "value-changed", G_CALLBACK(cw_on_round_corner), p);
        cw_row(grid, r++, "Скругление углов, px:", sc);
    }

    entry = gtk_entry_new();
    gtk_entry_set_text(GTK_ENTRY(entry),
                       priv->theme_dir ? priv->theme_dir : "");
    gtk_entry_set_placeholder_text(GTK_ENTRY(entry),
                                   "каталог темы (пусто = искать самим)");
    cw_tag(entry, "theme_dir");
    cw_row(grid, r++, "Каталог темы:", entry);

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
    priv->badge = g_strdup("");
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
    priv->view = CLAMP(xs_host_api()->conf_int(kf, p->name, "view",
                                              CW_VIEW_HOURS),
                       CW_VIEW_MIN, CW_VIEW_MAX);
    priv->show_daytemp = xs_host_api()->conf_int(kf, p->name,
                                                 "show_daytemp", 1) ? 1 : 0;
    priv->use_bg = xs_host_api()->conf_int(kf, p->name, "use_bg", 0);
    priv->round_corner = xs_host_api()->conf_int(kf, p->name,
                                                 "round_corner", 8);
    priv->theme_dir = xs_host_api()->conf_str(kf, p->name, "theme_dir", "");
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

    /* Кегли — родовые, из ClearWeatherScreenlet.py: температура 14,
     * город 6, дни недели 6, макс/мин 4 при холсте 132x100. Домножение
     * на cw_scale (320/132 = 2.42) даёт 34/15/15/10 — ровно то, что
     * нужно в окне. Прежние умолчания 26/12/11/10 были подобраны под
     * старую раскладку и после масштабирования выходили вдвое крупнее
     * родовых, из-за чего строка макс/мин не влезала и обрезалась. */
    priv->city_font = xs_host_api()->conf_str(kf, p->name, "city_font",
                                              "Sans Bold 6");
    priv->temp_font = xs_host_api()->conf_str(kf, p->name, "temp_font",
                                              "Sans Bold 14");
    priv->desc_font = xs_host_api()->conf_str(kf, p->name, "desc_font",
                                              "Sans Bold 6");
    priv->hour_font = xs_host_api()->conf_str(kf, p->name, "hour_font",
                                              "Sans Bold 4");
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

    /* Прозрачность окна. Ядро создаёт окно с RGBA-visual и зовёт
     * gtk_widget_set_app_paintable() на самом окне, но не на drawing
     * area внутри него. Без этого GTK рисует у area собственный
     * непрозрачный фон темы, и сквозь апплета не видно ничего: окно
     * формально с альфой, а выглядит чёрным. Ставим прозрачность и
     * area, не трогая ядро. */
    if (p->win) {
        GtkWidget *area = gtk_bin_get_child(GTK_BIN(p->win));

        if (area) {
            gtk_widget_set_app_paintable(area, TRUE);
            gtk_widget_set_has_window(area, FALSE);
            if (gtk_widget_get_has_window(area))
                gtk_widget_set_visual(area, gtk_widget_get_visual(p->win));
        }
    }

    if (!p->win) {
        p->host->log("clearweather: не удалось создать окно");
        cw_weather_free(priv->weather);
        g_free(priv->city);
        g_free(priv->city_alt);
        g_free(priv->proxy_url);
        g_free(priv->status);
        g_free(priv->badge);
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
    cw_theme_free(priv);
    g_free(priv->theme_dir);
    g_free(priv->status);
    g_free(priv->badge);
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
