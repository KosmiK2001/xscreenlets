/* network_monitor.c — сетевой applet: скорости up/down по выбранному
 * интерфейсу с историей в графике.
 *
 * Каркас (окно, округление, подгонка координат, серии, Properties)
 * написан по образцу disk_monitor. Его исходники здесь не
 * используются и не меняются: это отдельный плагин со своим core.
 */
#include <gtk/gtk.h>
#include <gio/gio.h>
#include <limits.h>
#include <math.h>
#include <stdlib.h>
#include <string.h>
#include "xs_api.h"
#include "common.h"
#include "network_monitor_core.h"

#include "../core/i18n.h"
#define NM_DEFAULT_WIDTH 217
#define NM_DEFAULT_HEIGHT 106
#define NM_DEFAULT_FONT "Sans 8"
#define NM_NETDEV "/proc/net/dev"
/* График занимает всё окно: подписи и значения рисуются поверх него,
 * поэтому для них ничего не резервируется. */
#define NM_GRAPH_TOP 0
/* Отступы между окном и графиком. Считаются по РЕАЛЬНОЙ высоте шрифта
 * того элемента, который выбран «снаружи» (а если таких несколько — по
 * наибольшей), плюс эти поля сверху и снизу, чтобы текст не липнул ни к
 * рамке графика, ни к рамке окна. Пользователь просил 1-2px на край. */
#define NM_MARGIN_TOP    2
#define NM_MARGIN_BOTTOM 2
/* Зазор между подписью («Down:») и числом, когда их координаты
 * сведены близко. Число уезжает вправо, но не дальше этой границы,
 * иначе Label pos. снова начнёт двигать число за собой. */
#define NM_VALUE_GAP 6

/* Ширины элементов диалога.
 *
 * NM_DIALOG_ENTRY — ширина полей ввода координат. Три цифры: у плагина
 * есть NM_MAX_POS (2047), а спин с полем на 5 знаков занимал бы вдвое
 * больше места, чем нужно, и растягивал секцию.
 *
 * NM_DIALOG_COMBO — ширина выпадающего списка Placement. Подобрана по
 * самой длинной подписи «Outside bottom»: при меньшей ширине GTK
 * обрезает её по «Outside bott…», и вариант перестаёт читаться.
 *
 * NM_DIALOG_FONT — кнопка шрифта. Меньше 180 не влезает «Sans Bold 10»
 * вместе с иконкой выбора, а поле метки занимает рядом ещё 10 символов.
 *
 * NM_DIALOG_LABEL — поле подписи, 10 символов: «Total down:» ровно
 * десять, а «Downloaded:» уже тринадцать и такой блок в секцию не
 * помещается. */
#define NM_DIALOG_ENTRY  58
#define NM_DIALOG_COMBO 132
#define NM_DIALOG_FONT  180
#define NM_DIALOG_LABEL  92
/* Нижний предел строки: нулевая высота схлопнула бы отступ, и график
 * наехал бы на текст. */
#define NM_ROW_H_MIN 8
#define NM_BORDER_PATH_INSET 0.5
/* Сколько значений усреднять: 1 = мгновенная скорость (поведение conky
 * по умолчанию), больше — сглаживает всплески. */
#define NM_RATE_SMOOTH_MIN 1
#define NM_RATE_SMOOTH_MAX 14
#define NM_MIN_WINDOW_WIDTH 160
#define NM_MIN_WINDOW_HEIGHT 60
/* Зазор между половинами в режиме Split: без него две заливки смыкаются
 * в одну фигуру и границы между download и upload не видно. */
#define NM_SPLIT_GAP 8

/* Текст рисуется поверх заливки графика и несёт тёмную тень. */
#define NM_TEXT_SHADOW_RADIUS 3
#define NM_TEXT_SHADOW_ALPHA 0.9

/* Где рисуется подпись серии: внутри графика (как в disk_monitor) или
 * над ним, на отдельной строке (как в conky с alignr). */
typedef enum {
    NM_LABEL_INSIDE = 0,
    /* Снаружи элемент уходит в поле между окном и графиком. Сверху это
     * подписи серий, снизу — сводки; но выбор не ограничен этим, верх и
     * низ доступны любому элементу. */
    NM_LABEL_OUTSIDE_TOP,
    NM_LABEL_OUTSIDE_BOTTOM,
} NmLabelPlacement;

#define NM_LABEL_OUTSIDE NM_LABEL_OUTSIDE_TOP   /* старое имя в конфиге */


/* Индекс комбо Placement -> значение. Вынесено отдельно, потому что
 * именно тут легко оставить одну ветку на старом булевом виде и
 * получить «снаружи» вместо «снизу» только для сводок. */
static NmLabelPlacement nm_placement_from_combo(gint active)
{
    if (active == 1)
        return NM_LABEL_OUTSIDE_TOP;
    if (active == 2)
        return NM_LABEL_OUTSIDE_BOTTOM;
    return NM_LABEL_INSIDE;
}

/* Номер строки элемента внутри полосы, в которую он ушёл.
 *
 * Слот считается ПО ФАЗАМ, а не по позиции внутри фазы:
 *
 *   фаза 0 — заголовок
 *   фаза 1 — подписи серий
 *   фаза 2 — сводки
 *
 * Элементы одной фазы делят одну строку, потому что у каждого своя
 * координата X: сводки Total down и Total up ставятся на один ряд и
 * раздвигаются по горизонтали. Считать слот по индексу нельзя — тогда
 * вторая сводка уезжала на строку ниже первой, и строки выглядели
 * разной высоты, хотя заданы одним шрифтом. */
typedef enum { NM_BAND_HEADER = 0, NM_BAND_LABEL, NM_BAND_TOTAL } NmBandPhase;

static int nm_band_slot(NmBandPhase phase,
                        const NmLabelPlacement *header_placement,
                        const NmLabelPlacement *label_placement,
                        NmLabelPlacement want, int rows)
{
    int slot = 0;
    int i;

    /* Слот — это число фаз, стоящих ВЫШЕ этой в той же полосе. Порядок
     * фаз: заголовок, подписи серий, сводки.
     *
     * Раньше здесь стоял ранний return, как только найдена подпись серии:
     * при заголовке сверху И подписи сверху сводка получала слот 1, тот
     * же, что и подпись, и накладывалась на неё. Теперь считаются все
     * фазы: заголовок даёт +1, наличие подписей даёт +1. */
    if (phase > NM_BAND_HEADER && header_placement && *header_placement == want)
        slot++;
    if (phase > NM_BAND_LABEL && label_placement)
        for (i = 0; i < NM_SERIES_MAX; i++)
            if (label_placement[i] == want) {
                slot++;
                break;
            }
    /* Слотов может оказаться больше, чем влезает в полосу: если высоты
     * не хватило, элементы делят доступные строки, иначе один просто
     * исчезнет — тише и хуже, чем показать вдвоем то, что влезло. */
    if (rows > 0 && slot >= rows)
        slot = rows > 1 ? rows - 1 : 0;
    return slot;
}

static const char *nm_placement_to_string(NmLabelPlacement pl)
{
    switch (pl) {
    case NM_LABEL_OUTSIDE_TOP:    return "top";
    case NM_LABEL_OUTSIDE_BOTTOM: return "bottom";
    default:                      return "inside";
    }
}

/* Разбор значения ключа *_placement: inside | outside | top | bottom.
 * «outside» без уточнения — это верх, как раньше. */
static NmLabelPlacement nm_placement_from_string(const char *v)
{
    if (!v)
        return NM_LABEL_INSIDE;
    if (g_ascii_strcasecmp(v, "top") == 0 ||
        g_ascii_strcasecmp(v, "outside") == 0)
        return NM_LABEL_OUTSIDE_TOP;
    if (g_ascii_strcasecmp(v, "bottom") == 0)
        return NM_LABEL_OUTSIDE_BOTTOM;
    return NM_LABEL_INSIDE;
}


typedef struct {
    XsPlugin *plugin;
    GKeyFile *kf;
    char *ifname;          /* выбранный интерфейс */
    char *ip;              /* IPv4, NULL если адресов нет */
    char *font;
    char *label_font;
    char *series_font[NM_SERIES_MAX];
    char *series_label[NM_SERIES_MAX];
    int series_x[NM_SERIES_MAX], series_y[NM_SERIES_MAX];
    int series_label_x[NM_SERIES_MAX], series_label_y[NM_SERIES_MAX];
    NmLabelPlacement label_placement[NM_SERIES_MAX];
    /* У заголовка и сводок тоже есть выбор inside/outside. Раньше они
     * всегда рисовались в графике, и настройки у них не было вовсе. */
    NmLabelPlacement header_placement;
    NmLabelPlacement total_placement[NM_SERIES_MAX];
    int width, height;
    int corner_radius;   /* скругление графика */
    int window_radius;   /* скругление окна, не меньше corner_radius */
    int shape_radius, shape_w, shape_h;
    int design_width, design_height;
    guint update_ms;
    int rate_smooth;
    NmGraphMode graph_mode;
    /* Предел шкалы в КиБ/с: 0 = брать максимум истории. Ручная
     * константа, как в conky (95 для 100 Мбит, 47-59 для софта). */
    gint64 graph_max_kib;
    int header_x, header_y;   /* имя интерфейса + IP */
    gdouble graph_bg[4], window_bg[4], border[4], text_color[4];
    /* Сводка настраивается ОТДЕЛЬНО для каждого направления: у down и up
     * своя подпись, свой шрифт, свой цвет и своя позиция. Общая строка
     * "Total: rx / tx" не позволяла поставить подписи так, как это
     * сделано в конфигах conky. */
    /* Сводка (overall) — те же два элемента, что и у серии: ПОДПИСЬ и
     * ЗНАЧЕНИЕ, каждое со своим текстом, шрифтом, цветом и позицией.
     *
     * Раньше это была одна строка с форматом «Total up: %s», собранная
     * функцией nm_format_label(). Пришлось бы писать формат с %s, и
     * подпись с числом нельзя было поставить в разные точки: число
     * всегда ехало в подписи по шаблону. Теперь пользователь сам
     * позиционирует подпись и число независимо, как у серий. */
    char *total_label[NM_SERIES_MAX];      /* «Total up:» */
    char *total_value_font[NM_SERIES_MAX]; /* шрифт числа */
    char *total_label_font[NM_SERIES_MAX]; /* шрифт подписи */
    char *total_value[NM_SERIES_MAX];      /* «120.2 GiB» */
    int total_x[NM_SERIES_MAX], total_y[NM_SERIES_MAX];        /* подпись */
    int total_value_x[NM_SERIES_MAX], total_value_y[NM_SERIES_MAX];
    gdouble total_color[NM_SERIES_MAX][4];         /* цвет подписи */
    gdouble total_value_color[NM_SERIES_MAX][4];  /* цвет числа */
    gdouble total_text_color[NM_SERIES_MAX][4];   /* общий текст */
    /* У каждой серии три независимых цвета: заливка графика, цвет
     * подписи («Down:») и цвет числа скорости. Раньше подпись и число
     * брали цвет заливки, и разделить их было нельзя.
     *
     * Значение по умолчанию у label и value — цвет заливки, поэтому при
     * отсутствии ключей в конфиге вид не меняется. */
    gdouble series_color[NM_SERIES_MAX][4];
    gdouble series_label_color[NM_SERIES_MAX][4];
    gdouble series_value_color[NM_SERIES_MAX][4];
    gdouble series_text_color[NM_SERIES_MAX][4];
    /* Шрифты подписи и числа — свои, как у сводки. Раньше был один
     * series<N>_font на оба. */
    char *series_label_font[NM_SERIES_MAX];
    char *series_value_font[NM_SERIES_MAX];
    /* Заголовок разделён надвое: имя интерфейса и адрес рисуются
     * независимо, у каждого свой цвет и своя координата X. По умолчанию
     * оба берут общий text_color. */
    gdouble header_ifname_color[4];
    gdouble header_ip_color[4];
    int header_ifname_x;
    /* Ширина IP прижата к правому краю: X задаётся автоматически, если
     * он не задан пользователем (0 = считать от правого края). */
    gboolean header_ip_x_auto;
    /* история: 0 = download (rx), 1 = upload (tx) */
    guint64 history[NM_SERIES_MAX][NM_HISTORY_MAX];
    guint head[NM_SERIES_MAX], count[NM_SERIES_MAX];
    guint64 prev_bytes[NM_SERIES_MAX];
    gboolean prev_valid;
    gint64 prev_time_us;
    guint64 current_rate[NM_SERIES_MAX];
    /* Сглаживание живёт в собственном кольце на 14 отсчётов, а не в
     * истории графика на 256: усреднение по всей истории забило бы
     * свежие значения. */
    NmSmoothRing smooth[NM_SERIES_MAX];
    guint64 total_bytes[NM_SERIES_MAX];
    gboolean sample_valid;
    cairo_surface_t *cache;
    int cache_width, cache_height;
    /* Y-спины серий: на них держится скрытие при Placement = снаружи, и
     * после gtk_widget_show_all() скрытие надо применить заново. */
    GtkWidget *series_y_spin[NM_SERIES_MAX];
} PrivData;

/* Умолчания взяты из настроенного экземпляра wan0, чтобы новый applet
 * появлялся уже настроенным, а не «как придумано в коде».
 *
 * Раньше заливка была download голубая, upload зелёная — цвета
 * read/write из disk_monitor, чтобы два applet'а читались одинаково.
 * Теперь цвет серии задаёт пользователь, и дефолт такой, каким
 * получился applet после настройки. */
static const gdouble nm_graph_bg_default[4] = {0.050980, 0.058824,
                                                0.078431, 0.850980};
static const gdouble nm_window_bg_default[4] = {0.0, 0.0, 0.0, 0.6};
static const gdouble nm_border_default[4] = {0.549020, 0.580392,
                                             0.619608, 1.0};
static const gdouble nm_text_default[4] = {1, 1, 1, 1};
static const gdouble nm_rx_default[4] = {0.25, 0.85, 0.35, 1.0};
static const gdouble nm_tx_default[4] = {0.784314, 0.0, 0.313725, 1.0};

/* Текст серии, её подпись и её число — раньше все трое по умолчанию
 * наследовали цвет серии, то есть копировали предыдущее поле. Теперь у
 * каждого своё значение: иначе настройки из настроенного экземпляра
 * невозможно было бы перенести в умолчания, потому что каскад подставлял
 * бы своё. */
static const gdouble nm_series_text_default[NM_SERIES_MAX][4] = {
    {0.109804, 0.443137, 0.847059, 1.0},   /* 28,113,216 */
    {1.0, 1.0, 1.0, 1.0}                    /* 255,255,255 */
};
static const gdouble nm_series_label_default[NM_SERIES_MAX][4] = {
    {0.870588, 0.866667, 0.854902, 1.0},   /* 222,221,218 */
    {0.870588, 0.866667, 0.854902, 1.0}
};
static const gdouble nm_series_value_default[NM_SERIES_MAX][4] = {
    {0.862745, 0.541176, 0.866667, 1.0},   /* 220,138,221 */
    {0.341176, 0.890196, 0.537255, 1.0}    /* 87,227,137 */
};
/* Сводки: подпись, число. Свои цвета у обоих и у каждой серии. */
static const gdouble nm_total_color_default[NM_SERIES_MAX][4] = {
    {0.752941, 0.749020, 0.737255, 1.0},   /* 192,191,188 */
    {0.752941, 0.749020, 0.737255, 1.0}
};
static const gdouble nm_total_value_default[NM_SERIES_MAX][4] = {
    {1.0, 0.0, 0.0, 1.0},
    {1.0, 0.0, 0.0, 1.0}
};
/* IP в шапке — отдельно от общего цвета текста. */
static const gdouble nm_header_ip_default[4] = {0.960784, 0.760784,
                                                 0.066667, 1.0};

static void nm_shutdown(XsPlugin *p);
static void nm_rebuild(PrivData *priv);

/* Живые экземпляры для обработчиков Properties: имя плагина -> priv.
 * Объявлено здесь, потому что nm_shutdown (ниже) тоже пользуется таблицей. */
static GHashTable *nm_instances;

static char *nm_color_text(const gdouble rgba[4])
{
    return nm_format_rgba(rgba);
}

static void nm_read_color(PrivData *priv, const char *key,
                          const gdouble fallback[4], gdouble out[4])
{
    char *text = xs_host_api()->conf_str(priv->kf, priv->plugin->name, key,
                                         nm_color_text(fallback));
    if (!nm_parse_rgba(text, out))
        memcpy(out, fallback, sizeof(gdouble) * 4);
    g_free(text);
}

/* ------------------------------------------------------------------ */
/* Данные                                                              */
/* ------------------------------------------------------------------ */

static void nm_sample(PrivData *priv)
{
    gint64 now = g_get_monotonic_time();
    char *text = NULL;
    NmNetSample sample;
    gboolean have_rates;
    gint64 elapsed = priv->prev_valid
                         ? (now - priv->prev_time_us) : 0;

    priv->sample_valid = FALSE;
    if (!priv->ifname || !*priv->ifname)
        return;
    if (!g_file_get_contents(NM_NETDEV, &text, NULL, NULL))
        return;
    if (!nm_parse_netdev(text, priv->ifname, &sample) || !sample.valid) {
        g_free(text);
        return;
    }
    g_free(text);

    have_rates = priv->prev_valid && elapsed > 0;
    if (have_rates) {
        guint i;
        gboolean reset_baseline = FALSE;

        for (i = 0; i < NM_SERIES_MAX; i++) {
            guint64 now_bytes = (i == 0) ? sample.rx_bytes : sample.tx_bytes;
            gint64 rate = nm_rate_bytes_per_second(priv->prev_bytes[i],
                                                   now_bytes,
                                                   priv->prev_time_us, now);
            if (rate < 0) {
                /* Оборот 32-битного счётчика: baseline сбрасывается, но
                 * в кольцо кладётся 0, а не 4 гигабайта. */
                reset_baseline = TRUE;
                priv->current_rate[i] = 0;
                nm_push_history(priv->history[i], &priv->head[i],
                                &priv->count[i], 0, now);
            } else {
                priv->current_rate[i] = (guint64)rate;
                nm_push_history(priv->history[i], &priv->head[i],
                                &priv->count[i], priv->current_rate[i], now);
            }
            nm_rate_smooth_push(&priv->smooth[i], rate);
            priv->total_bytes[i] = now_bytes;
            priv->prev_bytes[i] = now_bytes;
        }
        priv->sample_valid = TRUE;
        if (reset_baseline) {
            /* Оборот на одном направлении обнуляет baseline обоих: они
             * приходят из одной строки /proc/net/dev. */
            priv->prev_bytes[0] = sample.rx_bytes;
            priv->prev_bytes[1] = sample.tx_bytes;
        }
        if (priv->rate_smooth > 1) {
            guint i;
            for (i = 0; i < NM_SERIES_MAX; i++)
                priv->current_rate[i] = (guint64)
                    nm_rate_smoothed(&priv->smooth[i],
                                     (guint) priv->rate_smooth);
        }
    } else {
        /* Первое чтение только создаёт baseline: класть его в историю
         * нельзя, иначе первый график начинается с полной высоты. */
        priv->prev_bytes[0] = sample.rx_bytes;
        priv->prev_bytes[1] = sample.tx_bytes;
        priv->total_bytes[0] = sample.rx_bytes;
        priv->total_bytes[1] = sample.tx_bytes;
    }
    priv->prev_time_us = now;
    priv->prev_valid = TRUE;
}

/* ------------------------------------------------------------------ */
/* Отрисовка                                                           */
/* ------------------------------------------------------------------ */

/* (x, y) — координаты в окне. clip_h вместе с clip_y задаёт область
 * отсечения: элемент виден только внутри неё, но его собственные
 * координаты остаются абсолютными.
 *
 * Раньше отсечение задавалось одним height, а y считался абсолютным, и
 * nm_show_text клампил y по height как если бы тот был пределом окна.
 * Для нижней полосы это давало y = 105 → 13, то есть сводка улетала на
 * верх. Теперь предел считается как clip_y + clip_h. */
static void nm_show_text(cairo_t *cr, PangoLayout *layout,
                         const char *font_name, int x, int y,
                         const char *text, const gdouble color[4],
                         int width, int clip_y, int clip_h,
                         gboolean nm_clip_is_graph)
{
    PangoFontDescription *font = pango_font_description_from_string(font_name);
    int text_w = 0;

    pango_layout_set_font_description(layout, font);
    pango_font_description_free(font);
    pango_layout_set_text(layout, text, -1);
    pango_layout_get_pixel_size(layout, &text_w, NULL);
    /* Подгоняем по ОРЕОЛУ, а не по глифам: тень идёт на
     * NM_TEXT_SHADOW_RADIUS px во все стороны, и подгонка по text_w
     * оставляет значение формально внутри окна, когда его ореол уже
     * пересекает рамку. */
    x = nm_fit_text_coordinate(x, text_w + 2 * NM_TEXT_SHADOW_RADIUS, width);
    /* Отсечение по полосе навязывалось ВСЕМ элементам, включая те, что
     * рисуются внутри графика. Для числа скорости это означало, что его
     * координата Y не работала вовсе: значения 2, 10, 16 и 20 давали на
     * экране одно и то же положение, и подпись оказывалась на той же
     * строке. Ровно тот баг, который пользователь описывал как
     * «Position не работает на сами числа».
     *
     * Причина — что clip_y для элемента ВНУТРИ графика равен graph_y,
     * то есть началу области графика. Отсюда обе границы:
     *   y < graph_y  → текст поднимался к началу графика;
     *   y > graph_y + graph_h → опускался за нижний край.
     * Обе срабатывали на нормальных значениях координат, потому что
     * series<N>_y задаётся пользователем относительно окна, а не
     * относительно области графика.
     *
     * Смысл отсечения — не дать тексту вылезти за ОКНО, то есть спасти
     * элемент снаружи. Элемент внутри графика уже в окне, и его
     * собственная координата должна уважаться как есть. Поэтому для
     * внутренних элементов отсечение по полосе не применяется вовсе:
     * ограничение задаёт вызывающий, а nm_show_text лишь подгоняет
     * значение в пределы переданного прямоугольника.
     *
     * nm_show_text вызывается из двух мест: элементы внутри графика
     * передают clip_y = graph_y, элементы внешних полос — 0 (верхняя)
     * или graph_y + graph_h (нижняя). Отличать их по clip_h нельзя,
     * поэтому признак «внутри графика» передаётся флагом. */
    if (clip_h > 0) {
        int clip_bottom = clip_y + clip_h - 1 - NM_TEXT_SHADOW_RADIUS;
        if (y > clip_bottom)
            y = clip_bottom;
        if (!nm_clip_is_graph) {
            /* Нижняя граница внешней полосы: не дать ореолу пересечь
             * край окна. */
            if (y < clip_y + NM_TEXT_SHADOW_RADIUS)
                y = clip_y + NM_TEXT_SHADOW_RADIUS;
        }
    }
    {
        static const int offsets[8][2] = {{-1, -1}, {0, -1}, {1, -1}, {-1, 0},
                                          {1, 0}, {-1, 1}, {0, 1}, {1, 1}};
        int i;
        cairo_set_source_rgba(cr, 0.0, 0.0, 0.0, NM_TEXT_SHADOW_ALPHA);
        for (i = 0; i < 8; i++) {
            cairo_move_to(cr, x + offsets[i][0] * NM_TEXT_SHADOW_RADIUS,
                          y + offsets[i][1] * NM_TEXT_SHADOW_RADIUS);
            pango_cairo_show_layout(cr, layout);
        }
    }
    cairo_set_source_rgba(cr, color[0], color[1], color[2], color[3]);
    cairo_move_to(cr, x, y);
    pango_cairo_show_layout(cr, layout);
}

static void nm_draw_series(cairo_t *cr, const guint64 *history, guint head,
                           guint count, guint columns, int plot_width,
                           guint64 scale_max, double x, double y, int height,
                           const gdouble color[4])
{
    guint i;
    double origin = x + nm_history_origin_x(plot_width, columns);
    double baseline = y + height;
    gdouble fill[4];

    nm_fill_rgba(color, color[3], fill);
    cairo_set_source_rgba(cr, fill[0], fill[1], fill[2], fill[3]);
    if (columns == 0)
        return;
    /* Область замыкается двумя вертикалями и базовой линией: один
     * cairo_close_path соединил бы последнюю точку с первой и прочертил
     * диагональ через весь график. */
    {
        double right_x = origin + (double)(columns - 1);
        double first_y = y + height - height * (double)
            MIN(nm_history_value(history, head, count, 0), scale_max) /
            (double)scale_max;
        cairo_new_sub_path(cr);
        cairo_move_to(cr, right_x, baseline);
        cairo_line_to(cr, right_x, first_y);
        for (i = 0; i < columns; i++) {
            guint64 value = nm_history_value(history, head, count, i);
            double point_x = origin + (double)(columns - 1 - i);
            double point_y = y + height - height * (double)
                MIN(value, scale_max) / (double)scale_max;
            cairo_line_to(cr, point_x, point_y);
        }
        cairo_line_to(cr, origin, baseline);
        cairo_close_path(cr);
    }
    cairo_fill(cr);
}

/* Округлённый контур окна как путь (не заливка). */
static void nm_rounded_path(cairo_t *cr, int width, int height, int radius,
                            double border_inset)
{
    const double inset = 1.0;
    double w = width - 2 * inset, h = height - 2 * inset;
    double r = nm_corner_radius_value(radius);

    if (w <= 0 || h <= 0) {
        cairo_rectangle(cr, 0, 0, width, height);
        return;
    }
    if (!nm_corner_radius_is_rounded(r)) {
        cairo_rectangle(cr, inset, inset, w, h);
        return;
    }
    r = MIN(r, MIN(w, h) / 2.0);
    if (r > border_inset)
        r -= border_inset;
    else
        r = 0.0;
    cairo_new_sub_path(cr);
    cairo_arc(cr, inset + w - r, inset + r, r, -G_PI / 2.0, 0.0);
    cairo_arc(cr, inset + w - r, inset + h - r, r, 0.0, G_PI / 2.0);
    cairo_arc(cr, inset + r, inset + h - r, r, G_PI / 2.0, G_PI);
    cairo_arc(cr, inset + r, inset + r, r, G_PI, 1.5 * G_PI);
    cairo_close_path(cr);
}

/* Скруглённый прямоугольник с явным положением. Нужен для области
 * графика внутри окна: nm_rounded_path() всегда строит контур от (0,0),
 * а график может начинаться ниже. Через cairo_translate был бы двойной
 * сдвиг, поэтому положение задаётся сразу в координатах. */
static void nm_rounded_path_at(cairo_t *cr, double x, double y,
                               double w, double h, double radius,
                               double inset)
{
    double x0 = x + inset, y0 = y + inset;
    double rw = w - 2.0 * inset, rh = h - 2.0 * inset;
    double x1 = x0 + rw, y1 = y0 + rh;
    double r;

    if (!nm_corner_radius_is_rounded(radius))
        radius = 0.0;
    r = (radius > 0.0) ? MIN(radius, MIN(rw, rh) / 2.0) : 0.0;
    if (r <= 0.0) {
        cairo_rectangle(cr, x0, y0, rw, rh);
        return;
    }
    cairo_new_sub_path(cr);
    cairo_arc(cr, x1 - r, y0 + r, r, -G_PI / 2.0, 0.0);
    cairo_arc(cr, x1 - r, y1 - r, r, 0.0, G_PI / 2.0);
    cairo_arc(cr, x0 + r, y1 - r, r, G_PI / 2.0, G_PI);
    cairo_arc(cr, x0 + r, y0 + r, r, G_PI, 1.5 * G_PI);
    cairo_close_path(cr);
}

/* Высота строки по имени шрифта, в пикселях. Шрифты хранятся строками
 * ("Sans 8"), а не PangoFontDescription, поэтому разбираем их здесь же.
 *
 * Именно pango_layout_get_pixel_size, а не pango_layout_get_extents:
 * extents отдаёт логические единицы (для «Sans 8» на этой машине
 * logical.height = 13312 при PANGO_SCALE = 1024). Без деления на
 * PANGO_SCALE отступ становился 13316 px, съедал окно целиком, и весь
 * текст пропадал. Проверено пробой: 13312 / 1024 = 13 px — ровно то,
 * что даёт get_pixel_size. */
static int nm_row_height(PangoLayout *layout, const char *font_name)
{
    PangoFontDescription *fd;
    int h = 0;

    if (!font_name || !*font_name)
        return 0;
    fd = pango_font_description_from_string(font_name);
    if (!fd)
        return 0;
    pango_layout_set_font_description(layout, fd);
    pango_font_description_free(fd);
    /* С текстом, состоящим из одного символа, layout честно отдаёт
     * метрики строки по этому шрифту. */
    pango_layout_set_text(layout, "0", -1);
    pango_layout_get_pixel_size(layout, NULL, &h);
    pango_layout_set_text(layout, "", -1);
    return MAX(0, h);
}

static cairo_surface_t *nm_render(PrivData *priv, int width, int height)
{
    cairo_surface_t *surface = cairo_image_surface_create(CAIRO_FORMAT_ARGB32,
                                                          width, height);
    cairo_t *cr = cairo_create(surface);
    PangoLayout *layout = pango_cairo_create_layout(cr);
    guint columns;
    guint64 scale_max[NM_SERIES_MAX] = {1, 1};
    guint64 observed_max = 0;
    gboolean split = (priv->graph_mode == NM_GRAPH_SPLIT);
    guint i;
    int graph_y, graph_h;
    int top_band, bot_band, top_rows, bot_rows, top_step, bot_step;
    int window_radius;
    int dw = priv->design_width > 0 ? priv->design_width : width;
    int dh = priv->design_height > 0 ? priv->design_height : height;
    char *rate_text[NM_SERIES_MAX];
    char *total_text[NM_SERIES_MAX];
    char *header_name;
    char *header_ip;

    pango_layout_set_font_description(layout, NULL);

    /* Отступы между окном и графиком. Элемент, выбранный «снаружи»,
     * уходит из графика в поле между рамкой окна и рамкой графика: в
     * верхнюю полосу или в нижнюю, по выбору пользователя.
     *
     * Полосы независимы: если сверху только заголовок, а снизу две
     * сводки, то top_band и bot_band считаются по своим элементам.
     * Высота берётся по РЕАЛЬНОМУ шрифту (nm_row_height), потому что
     * pango_layout_get_extents отдаёт логические единицы, а не пиксели.
     * Плюс NM_MARGIN_TOP/BOTTOM, чтобы текст не липнул к рамкам. */
    /* Раскладка полос. Элемент, выбранный «снаружи», уходит из графика
     * в поле между рамкой окна и рамкой графика — наверх или вниз.
     *
     * Шаг строки ОБЩИЙ для двух вещей: для высоты полосы и для сдвига
     * между строками. Раньше высота считалась как NM_ROW_H_MIN * rows
     * (8 px на строку), а сдвиг — по реальному шрифту плюс margin
     * (15 px). Нижние строки уезжали за полосу и обрезались: в нижней
     * полосе оставалась одна строка вместо трёх.
     *
     * Высота берётся по РЕАЛЬНОМУ шрифту (nm_row_height), потому что
     * pango_layout_get_extents отдаёт логические единицы, а не пиксели.
     * Плюс NM_MARGIN_TOP/BOTTOM, чтобы текст не липнул к рамкам. */
    /* Число СТРОК в каждой полосе. Строка — это не элемент, а фаза:
     * заголовок, подписи серий, сводки. Элементы одной фазы делят одну
     * строку и раздвигаются по X, поэтому сводки Total down и Total up
     * стоят рядом, а не друг под другом.
     *
     * Считать по элементам нельзя: две сводки дали бы две строки, и
     * вторая уехала бы вниз — строки выглядели бы разной высоты при
     * одном шрифте. */
    /* Число строк в полосе. Считается по фазам, но если все totals
     * в нижней полосе — они делят одну строку, и bot_rows = 1. */
    top_rows = 0;
    bot_rows = 0;
    top_step = 0;
    bot_step = 0;
    if (priv->header_placement == NM_LABEL_OUTSIDE_TOP) {
        top_rows++;
        top_step = MAX(top_step, nm_row_height(layout, priv->label_font));
    }
    if (priv->header_placement == NM_LABEL_OUTSIDE_BOTTOM) {
        bot_rows++;
        bot_step = MAX(bot_step, nm_row_height(layout, priv->label_font));
    }
    /* Подписи серий — одна ФАЗА: Down и Up делят одну строку и
     * раздвигаются по горизонтали (у каждой свой series<N>_label_x).
     * Раньше каждая серия увеличивала счётчик строк, и две подписи
     * давали две строки — вторая пустовала, полоса была выше нужного.
     * Шрифт берётся максимальным из обеих. */
    {
        int labels_top = 0, labels_bottom = 0;

        for (i = 0; i < NM_SERIES_MAX; i++) {
            /* Шрифтов у серии два, и строка должна помещать оба
             * элемента: берём максимум из высоты подписи и числа. */
            int h = MAX(nm_row_height(layout,
                                      priv->series_label_font[i]),
                        nm_row_height(layout,
                                      priv->series_value_font[i]));

            if (priv->label_placement[i] == NM_LABEL_OUTSIDE_TOP) {
                labels_top = 1;
                top_step = MAX(top_step, h);
            } else if (priv->label_placement[i] == NM_LABEL_OUTSIDE_BOTTOM) {
                labels_bottom = 1;
                bot_step = MAX(bot_step, h);
            }
        }
        top_rows += labels_top;
        bot_rows += labels_bottom;
    }
    /* Сводки — отдельная ФАЗА: они занимают свою строку, даже если
     * заголовок или подписи серий уже стоят в этой полосе. Раньше стояло
     * «если полоса пуста, занять одну строку», из-за чего при подписи
     * снизу и сводках снизу полоса считалась в одну строку вместо двух —
     * и нижний текст уезжал за границу окна.
     *
     * Сами сводки между собой строку НЕ делят: у каждой свой X
     * (total<N>_x), они раздвигаются по горизонтали на одном уровне. */
    {
        int totals_top = 0, totals_bottom = 0;

        for (i = 0; i < NM_SERIES_MAX; i++) {
            int h = nm_row_height(layout, priv->total_label_font[i]);

            if (priv->total_placement[i] == NM_LABEL_OUTSIDE_TOP) {
                totals_top = 1;
                top_step = MAX(top_step, h);
            } else if (priv->total_placement[i] == NM_LABEL_OUTSIDE_BOTTOM) {
                totals_bottom = 1;
                bot_step = MAX(bot_step, h);
            }
        }
        top_rows += totals_top;
        bot_rows += totals_bottom;
    }
    top_step = MAX(top_step, NM_ROW_H_MIN);
    bot_step = MAX(bot_step, NM_ROW_H_MIN);
    top_band = top_rows ? top_step * top_rows + NM_MARGIN_TOP
                        + NM_MARGIN_BOTTOM : 0;
    bot_band = bot_rows ? bot_step * bot_rows + NM_MARGIN_TOP
                        + NM_MARGIN_BOTTOM : 0;

    graph_y = NM_GRAPH_TOP + top_band;
    graph_h = height - graph_y - bot_band;

    /* Полосы НЕ сжимаются под нужную высоту окна.
     *
     * Раньше при нехватке места overflow вычитался сначала из верхней
     * полосы: если она меньше overflow, она обнулялась, а остаток
     * терялся — нижняя полоса оставалась рассчитана на полную высоту,
     * а её текст уезжал за нижнюю границу окна. Второй симптом: слоты
     * считались от полной полосы, поэтому строка пропадала из вида
     * целиком.
     *
     * Текст важнее графика: график сжимается, полосы остаются. Если
     * окно совсем мало и места нет вовсе, полосы делят недостающее
     * поровну, но каждая сохраняет хотя бы одну строку — иначе
     * элементы просто исчезают, а это тише и хуже, чем обрезка. */
    if (graph_h < 1) {
        int need = -graph_h;
        int cut_top = 0, cut_bot = 0;

        if (top_rows > 0)
            cut_top = MIN(need, (top_band - NM_ROW_H_MIN) * top_rows
                               / MAX(1, top_rows));
        if (need > 0 && bot_rows > 0)
            cut_bot = MIN(need - cut_top, (bot_band - NM_ROW_H_MIN) * bot_rows
                                        / MAX(1, bot_rows));
        top_band = MAX(top_band - cut_top, top_rows ? NM_ROW_H_MIN : 0);
        bot_band = MAX(bot_band - cut_bot, bot_rows ? NM_ROW_H_MIN : 0);
        if (top_band + bot_band > height - 1) {
            /* Совсем мало места: делим пропорционально числу строк. */
            int room = MAX(0, height - 1);
            int total_rows = top_rows + bot_rows;

            if (total_rows > 0) {
                top_band = room * top_rows / total_rows;
                bot_band = room - top_band;
            }
        }
        graph_y = NM_GRAPH_TOP + top_band;
        /* График должен оставаться выше двух: ниже этого значение
     * становится отрицательным в nm_draw_series и в cairo_rectangle,
     * где передаётся graph_h - 2 (рама занимает по 1 px сверху и снизу).
     *
     * Раньше здесь стоял MAX(1, ...), и при сильном сжатии график
     * получал высоту 1, то есть graph_h - 2 = -1. cairo это молча
     * принимает, но nm_rounded_path_at с rh = -1 даёт отрицательный
     * радиус дуги — контур и клип становились мусором. Сценарий
     * достижим легально: NM_MIN_WINDOW_HEIGHT = 60, а шесть внешних
     * элементов по NM_ROW_H_MIN требуют заметно больше. */
        graph_h = MAX(3, height - graph_y - bot_band);
    }

    /* Скругление окна не меньше скругления графика. */
    window_radius = MAX(priv->window_radius, priv->corner_radius);
    {
        /* Клип по округлённому контуру ОКНА. Скругление окна не меньше
         * скругления графика (window_radius >= corner_radius), иначе
         * рамка окна срезала бы углы графика по диагонали. */
        const double radius = nm_corner_radius_value(window_radius);
        if (nm_corner_radius_is_rounded(radius)) {
            double r = MIN(radius, MIN(width, height) / 2.0);
            cairo_new_sub_path(cr);
            cairo_arc(cr, width - r, r, r, -G_PI / 2.0, 0.0);
            cairo_arc(cr, width - r, height - r, r, 0.0, G_PI / 2.0);
            cairo_arc(cr, r, height - r, r, G_PI / 2.0, G_PI);
            cairo_arc(cr, r, r, r, G_PI, 1.5 * G_PI);
            cairo_close_path(cr);
            cairo_clip(cr);
        }
    }
    cairo_set_operator(cr, CAIRO_OPERATOR_SOURCE);
    cairo_set_source_rgba(cr, 0.0, 0.0, 0.0, 0.0);
    cairo_paint(cr);
    cairo_set_operator(cr, CAIRO_OPERATOR_OVER);

    columns = nm_history_columns(width - 4,
                                 MAX(priv->count[0], priv->count[1]));
    /* Предел шкалы ОДИН на обе серии, в любом режиме. Общая шкала — это
     * и есть смысл графика: 11 Мбайт должны выглядеть выше, чем 100
     * килобайт. Раздельные максимумы (как у conky) превращали бы upload
     * в размазанную полосу у нижнего края, и сравнивать направления
     * на глаз было бы нельзя. Порядок: ручная константа из настроек
     * (0 = брать максимум наблюдённой истории). */
    for (i = 0; i < columns; i++) {
        guint64 rx = nm_history_value(priv->history[0], priv->head[0],
                                      priv->count[0], i);
        guint64 tx = nm_history_value(priv->history[1], priv->head[1],
                                      priv->count[1], i);
        if (rx > observed_max) observed_max = rx;
        if (tx > observed_max) observed_max = tx;
    }
    {
        guint64 cap = priv->graph_max_kib > 0
                          ? (guint64) priv->graph_max_kib * 1024
                          : MAX((guint64)1, observed_max);
        for (i = 0; i < NM_SERIES_MAX; i++)
            scale_max[i] = cap;
    }


    /* Окно заливается своим фоном (там, где подписи), график — своим.
     * Когда внешних подписей нет, обе заливки совпадают и разницы не
     * видно: ровно как раньше. */
    cairo_save(cr);
    nm_rounded_path(cr, width, height, window_radius, 0.0);
    cairo_set_source_rgba(cr, priv->window_bg[0], priv->window_bg[1],
                          priv->window_bg[2], priv->window_bg[3]);
    cairo_fill(cr);
    cairo_restore(cr);
    /* Заливка графика ограничена ЕГО областью. Раньше она шла на всё
     * окно, и когда появились отступы, чёрная заливка затирала внешние
     * элементы: отступы были, но выглядели как часть графика. */
    cairo_save(cr);
    nm_rounded_path_at(cr, 0, graph_y, width, graph_h,
                       priv->corner_radius, 0.0);
    cairo_set_source_rgba(cr, priv->graph_bg[0], priv->graph_bg[1],
                          priv->graph_bg[2], priv->graph_bg[3]);
    cairo_fill(cr);
    cairo_restore(cr);
    cairo_save(cr);
    cairo_rectangle(cr, 2, graph_y + 1, width - 4, graph_h - 2);
    cairo_clip(cr);
    /* Контур ГРАФИКА (высота graph_h, положение graph_y), а не окна:
     * клип по окну накрывал отступы, куда заливкам не место. Серии
     * рисуются в абсолютных координатах от graph_y. */
    nm_rounded_path_at(cr, 0, graph_y, width, graph_h,
                       priv->corner_radius, 0.0);
    cairo_clip(cr);
    if (split) {
        /* Две половины с зазором между ними. Без зазора заливки смыкались
         * в одну пешку, и было не видно, где кончается download и
         * начинается upload, — а это ровно то, ради чего режим раздельный.
         * Зазор занимает NM_SPLIT_GAP пикселей и рисуется разделителем в
         * цвет рамки. */
        const int gap = MIN(NM_SPLIT_GAP, MAX(0, (width - 4) / 4));
        const int plot_w = MAX(1, (width - 4 - gap) / 2);
        const double left_x = 2.0;
        const double right_x = left_x + plot_w + gap;

        nm_draw_series(cr, priv->history[0], priv->head[0], priv->count[0],
                       nm_history_columns(plot_w, priv->count[0]), plot_w,
                       scale_max[0], left_x, graph_y + 1.0, graph_h - 2.0,
                       priv->series_color[0]);
        nm_draw_series(cr, priv->history[1], priv->head[1], priv->count[1],
                       nm_history_columns(plot_w, priv->count[1]), plot_w,
                       scale_max[1], right_x, graph_y + 1.0, graph_h - 2.0,
                       priv->series_color[1]);
        if (gap > 0) {
            double mid = left_x + plot_w + gap / 2.0;

            cairo_set_source_rgba(cr, priv->border[0], priv->border[1],
                                  priv->border[2], priv->border[3]);
            cairo_set_line_width(cr, 1.0);
            cairo_move_to(cr, floor(mid), graph_y + 1.0);
            cairo_line_to(cr, floor(mid), graph_y + graph_h - 1.0);
            cairo_stroke(cr);
        }
    } else {
        /* Общий график: обе серии в одном поле, порядок по сумме, чтобы
         * более крупная ушла на задний план и не закрыла мелкую. */
        guint64 sum[NM_SERIES_MAX] = {0, 0};
        for (i = 0; i < columns; i++) {
            sum[0] += nm_history_value(priv->history[0], priv->head[0],
                                       priv->count[0], i);
            sum[1] += nm_history_value(priv->history[1], priv->head[1],
                                       priv->count[1], i);
        }
        if (sum[0] >= sum[1]) {
            nm_draw_series(cr, priv->history[0], priv->head[0], priv->count[0],
                           columns, width - 4, scale_max[0], 2.0,
                           graph_y + 1.0, graph_h - 2.0, priv->series_color[0]);
            nm_draw_series(cr, priv->history[1], priv->head[1], priv->count[1],
                           columns, width - 4, scale_max[1], 2.0,
                           graph_y + 1.0, graph_h - 2.0, priv->series_color[1]);
        } else {
            nm_draw_series(cr, priv->history[1], priv->head[1], priv->count[1],
                           columns, width - 4, scale_max[1], 2.0,
                           graph_y + 1.0, graph_h - 2.0, priv->series_color[1]);
            nm_draw_series(cr, priv->history[0], priv->head[0], priv->count[0],
                           columns, width - 4, scale_max[0], 2.0,
                           graph_y + 1.0, graph_h - 2.0, priv->series_color[0]);
        }
    }
    cairo_restore(cr);
    cairo_set_line_width(cr, 1.0);

    /* Рамка графика рисуется в ЕГО области, а не по контуру окна: когда
     * внешние подписи вынесли график в середину окна, рамка вокруг всего
     * окна смотрелась бы как рамка графика, и график остался бы без
     * собственной границы. Когда внешних подписей нет (top_band==0 && bot_band==0),
     * области совпадают и получается ровно одна рамка, как раньше. */
    if (top_band > 0 || bot_band > 0) {
        cairo_set_source_rgba(cr, priv->border[0], priv->border[1],
                              priv->border[2], priv->border[3]);
        cairo_save(cr);
        cairo_rectangle(cr, 0, graph_y, width, graph_h);
        cairo_clip(cr);
        nm_rounded_path_at(cr, 0, graph_y, width, graph_h,
                           priv->corner_radius, NM_BORDER_PATH_INSET);
        cairo_stroke(cr);
        cairo_restore(cr);
    }

    /* Рамка окна — всегда, своим радиусом. */
    cairo_set_source_rgba(cr, priv->border[0], priv->border[1],
                          priv->border[2], priv->border[3]);
    cairo_save(cr);
    nm_rounded_path(cr, width, height, window_radius, NM_BORDER_PATH_INSET);
    cairo_stroke(cr);
    cairo_restore(cr);

    /* Текст клипается контуром окна: глиф у скруглённого угла просто не
     * рисуется, без ручных границ на каждую подпись. */
    cairo_save(cr);
    nm_rounded_path(cr, width, height, window_radius, 0.0);
    cairo_clip(cr);

    for (i = 0; i < NM_SERIES_MAX; i++) {
        rate_text[i] = priv->sample_valid
                           ? nm_format_rate(priv->current_rate[i])
                           : g_strdup("N/A");
    }
    /* Шапка: имя интерфейса и адрес. Адреса может не быть (ifb, tun) —
     * тогда показываем только имя, без пустого хвоста. */
    /* Шапка разбита на два независимых элемента: имя интерфейса и
     * адрес. Раньше это была одна строка «%s: %s» с общим цветом и
     * одной координатой, поэтому ни разделить их по цвету, ни разнести
     * по краям окна было нельзя.
     *
     * Имя идёт слева, адрес — прижат к правому краю. Адреса может не
     * быть (ifb, tun без адреса): тогда рисуется только имя, без пустого
     * хвоста и без двоеточия. */
    header_name = nm_split_header(priv->ifname, priv->ip, &header_ip);
    /* Сводка (overall) — тоже два элемента, а не строка по шаблону.
     * Подпись и число рисуются независимо, у каждого свой шрифт, цвет и
     * координаты. Раньше собиралось nm_format_label("Total up: %s", …),
     * и число всегда ехало в подписи по шаблону: поставить его в другую
     * точку было нельзя в принципе.
     *
     * total<N>_value — необязательный текст перед числом, чтобы можно
     * было написать «47.6 GiB всего». */
    for (i = 0; i < NM_SERIES_MAX; i++) {
        char *num = nm_format_bytes(priv->total_bytes[i]);

        if (priv->total_value[i] && *priv->total_value[i])
            total_text[i] = g_strdup_printf("%s %s", priv->total_value[i],
                                            num);
        else
            total_text[i] = g_strdup(num);
        g_free(num);
    }

    for (i = 0; i < NM_SERIES_MAX; i++) {
        int sx = nm_scale_position(priv->series_x[i], dw, width, width - 1);
        int sy = nm_scale_position(priv->series_y[i], dh, height, height - 1);
        /* Куда ставить число. Внутри — за подписью, но не дальше, чем
         * на NM_VALUE_GAP от её конца; снаружи — на своей координате.
         * Инициализируется здесь, потому что вызывается в конце цикла. */
        int value_x = sx;
        int lx = nm_scale_position(priv->series_label_x[i], dw, width,
                                   width - 1);
        int ly = nm_scale_position(priv->series_label_y[i], dh, height,
                                   height - 1);
        char *full;

        if (priv->label_placement[i] == NM_LABEL_INSIDE) {
            /* Внутри подпись и значение — ДВА элемента с разными
             * координатами:
             *   series<N>_label_x/y  — где стоит «Down:» / «Up:»
             *   series<N>_x/y        — где стоит само число скорости
             *
             * Раньше значение рисовалось на lx + ширина_подписи + 6, то
             * ездило вместе с подписью. Отсюда два жалобных: Label pos.
             * двигал всю строку целиком, а Position не двигал ничего —
             * число к нему было привязано, но не наоборот.
             *
             * Ширина подписи нужна, чтобы число не налезало на текст,
             * когда координаты сведены близко. */
            {
                PangoFontDescription *fd =
                    pango_font_description_from_string(
                        priv->series_label_font[i]);
                int label_w = 0, value_w = 0;
                char *value_text = g_strdup(rate_text[i]);

                /* Число получает свою координату безусловно: это то,
                 * что задаёт Position, и уступать оно не должно.
                 *
                 * Раньше здесь стояло value_x = MAX(sx, lx + w + GAP).
                 * Из-за этого series<N>_x не работал, пока он меньше
                 * конца подписи: значения 10, 20 и 30 давали на экране
                 * одно и то же, потому что все подпадали под MAX и
                 * вставали на lx + w + GAP. Пользователь двигал Position X
                 * и ничего не получал.
                 *
                 * Теперь при наезде двигается ПОДПИСЬ, а не число: она
                 * второстепенна, сдвинуть её можно почти всегда, и обе
                 * настройки получают независимый смысл. */
                pango_layout_set_font_description(layout, fd);
                pango_layout_set_text(layout, priv->series_label[i], -1);
                pango_layout_get_pixel_size(layout, &label_w, NULL);
                pango_layout_set_text(layout, value_text, -1);
                pango_layout_get_pixel_size(layout, &value_w, NULL);

                value_x = sx;
                /* Наезд: сдвигается ПОДПИСЬ, число остаётся на своей
                 * координате — это и было целью правки.
                 *
                 * Условие проверяет пересечение по левому краю подписи:
                 * если подпись кончается правее начала числа, она
                 * заезжает на число, и тогда уводим её за число.
                 *
                 * Раньше здесь стояло обратное:
                 *     if (value_x + value_w + GAP > lx)
                 * то есть «правый край числа правее левого края
                 * подписи». Это верно почти всегда, даже когда
                 * подпись и число не пересекаются: подпись на 3..25, число
                 * на 51..96 — между ними 26 px свободного места, но
                 * условие всё равно срабатывало и уводило подпись за
                 * число. На экране выходило «59.4 KiB/s Down» вместо
                 * «Down: 59.4 KiB/s». */
                if (lx + label_w + NM_VALUE_GAP > value_x)
                    lx = value_x + value_w + NM_VALUE_GAP;

                nm_show_text(cr, layout, priv->series_label_font[i], lx, ly,
                             priv->series_label[i],
                             priv->series_label_color[i],
                             width, graph_y, graph_h, TRUE);
                pango_font_description_free(fd);
                g_free(value_text);
            }
            full = g_strdup(rate_text[i]);
        } else {
            /* Снаружи: подпись уходит в полосу, значение остаётся в
             * графике. Полоса сверху или снизу — по выбору. */
            int row_h = (priv->label_placement[i] == NM_LABEL_OUTSIDE_BOTTOM)
                            ? bot_step : top_step;
            int slot = nm_band_slot(NM_BAND_LABEL,
                                    &priv->header_placement,
                                    priv->label_placement,
                                    priv->label_placement[i],
                                    priv->label_placement[i]
                                    == NM_LABEL_OUTSIDE_BOTTOM
                                    ? bot_rows : top_rows);
            int row_y;

            if (priv->label_placement[i] == NM_LABEL_OUTSIDE_BOTTOM)
                row_y = graph_y + graph_h + NM_MARGIN_TOP + slot * row_h;
            else
                row_y = NM_MARGIN_TOP + slot * row_h;
            nm_show_text(cr, layout, priv->series_label_font[i],
                         lx, row_y, priv->series_label[i],
                         priv->series_label_color[i], width,
                         priv->label_placement[i] == NM_LABEL_OUTSIDE_BOTTOM
                         ? graph_y + graph_h : 0,
                         priv->label_placement[i] == NM_LABEL_OUTSIDE_BOTTOM
                         ? bot_band : top_band, FALSE);
            value_x = sx;
            full = g_strdup(rate_text[i]);
        }
        nm_show_text(cr, layout, priv->series_value_font[i], value_x, sy, full,
                     priv->series_value_color[i], width, graph_y, graph_h,
                     TRUE);
        g_free(full);
    }
    /* Заголовок: два элемента с разными цветами. Имя прижато к
     * header_x (слева), адрес — к правому краю окна.
     *
     * Заголовок в своей полосе всегда первый: слот 0. Подписи и сводки
     * сдвигаются на шаг вниз, если делят с ним полосу. */
    {
        int hx = nm_scale_position(priv->header_x, dw, width, width - 1);
        int hy = nm_scale_position(priv->header_y, dh, height, height - 1);
        int clip_y0, clip_h, row_y;
        int ip_x = 0;

        switch (priv->header_placement) {
        case NM_LABEL_OUTSIDE_TOP:
            row_y = NM_MARGIN_TOP;
            clip_y0 = 0;
            clip_h = top_band;
            break;
        case NM_LABEL_OUTSIDE_BOTTOM:
            row_y = graph_y + graph_h + NM_MARGIN_TOP;
            clip_y0 = graph_y + graph_h;
            clip_h = bot_band;
            break;
        default:
            row_y = hy;
            clip_y0 = graph_y;
            clip_h = graph_h;
            break;
        }

        /* Адрес прижимаем к правому краю: X считается от ширины окна
         * минус ширина текста, минус запас на тень. */
        if (header_ip && *header_ip) {
            PangoFontDescription *fd =
                pango_font_description_from_string(priv->label_font);
            int ip_w = 0;

            pango_layout_set_font_description(layout, fd);
            pango_layout_set_text(layout, header_ip, -1);
            pango_layout_get_pixel_size(layout, &ip_w, NULL);
            pango_font_description_free(fd);
            ip_x = nm_fit_text_coordinate(
                width - ip_w - 2 * NM_TEXT_SHADOW_RADIUS, ip_w, width);
        }

        if (header_name && *header_name)
            nm_show_text(cr, layout, priv->label_font, hx, row_y,
                         header_name, priv->header_ifname_color, width,
                         clip_y0, clip_h, FALSE);
        if (header_ip && *header_ip)
            nm_show_text(cr, layout, priv->label_font, ip_x, row_y,
                         header_ip, priv->header_ip_color, width,
                         clip_y0, clip_h, FALSE);
    }

    /* Сводка: подпись и число — два элемента, как у серии. Оба делят
     * одну полосу (фаза NM_BAND_TOTAL), но у каждого свои координаты,
     * шрифт и цвет, а наезд разрешается так же: число своё место не
     * уступает, уезжает подпись.
     *
     * Значение выводится, даже если подпись пустая, и наоборот: это два
     * независимых элемента, а не две части одной строки. */
    for (i = 0; i < NM_SERIES_MAX; i++) {
        int tx = nm_scale_position(priv->total_x[i], dw, width, width - 1);
        int ty = nm_scale_position(priv->total_y[i], dh, height, height - 1);
        int vx = nm_scale_position(priv->total_value_x[i], dw, width,
                                  width - 1);
        int vy = nm_scale_position(priv->total_value_y[i], dh, height,
                                  height - 1);
        int row_h, slot, row_y, clip_y0, clip_h;
        int lx = tx, vxp = vx;
        int label_w = 0, value_w = 0;
        gboolean clip_is_graph;

        switch (priv->total_placement[i]) {
        case NM_LABEL_OUTSIDE_TOP:
            row_h = top_step;
            slot = nm_band_slot(NM_BAND_TOTAL, &priv->header_placement,
                                priv->label_placement, NM_LABEL_OUTSIDE_TOP,
                                top_rows);
            row_y = NM_MARGIN_TOP + slot * row_h;
            clip_y0 = 0;
            clip_h = top_band;
            clip_is_graph = FALSE;
            break;
        case NM_LABEL_OUTSIDE_BOTTOM:
            row_h = bot_step;
            slot = nm_band_slot(NM_BAND_TOTAL, &priv->header_placement,
                                priv->label_placement, NM_LABEL_OUTSIDE_BOTTOM,
                                bot_rows);
            row_y = graph_y + graph_h + NM_MARGIN_TOP + slot * row_h;
            clip_y0 = graph_y + graph_h;
            clip_h = bot_band;
            clip_is_graph = FALSE;
            break;
        default:
            row_y = 0;   /* для inside каждое своё Y, см. ниже */
            clip_y0 = graph_y;
            clip_h = graph_h;
            clip_is_graph = TRUE;
            break;
        }

        /* Ширины нужны для разрешения наезда: число на своей
         * координате, подпись при пересечении уезжает за него. */
        {
            PangoFontDescription *fd = pango_font_description_from_string(
                priv->total_label_font[i]);

            pango_layout_set_font_description(layout, fd);
            pango_layout_set_text(layout,
                                  priv->total_label[i]
                                      ? priv->total_label[i] : "", -1);
            pango_layout_get_pixel_size(layout, &label_w, NULL);
            pango_font_description_free(fd);

            fd = pango_font_description_from_string(
                priv->total_value_font[i]);
            pango_layout_set_font_description(layout, fd);
            pango_layout_set_text(layout, total_text[i], -1);
            pango_layout_get_pixel_size(layout, &value_w, NULL);
            pango_font_description_free(fd);
        }
        if (lx + label_w + NM_VALUE_GAP > vxp)
            lx = vxp + value_w + NM_VALUE_GAP;

        {
            /* Для inside у каждого своя Y; для внешней полосы оба на
             * одной строке, посчитанной фазой. */
            int lrow = row_y, vrow = row_y;

            if (priv->total_placement[i] == NM_LABEL_INSIDE) {
                lrow = ty;
                vrow = vy;
            }
            if (priv->total_label[i] && *priv->total_label[i])
                nm_show_text(cr, layout, priv->total_label_font[i], lx, lrow,
                             priv->total_label[i], priv->total_color[i],
                             width, clip_y0, clip_h, clip_is_graph);
            nm_show_text(cr, layout, priv->total_value_font[i], vxp, vrow,
                         total_text[i], priv->total_value_color[i], width,
                         clip_y0, clip_h, clip_is_graph);
        }
        g_free(total_text[i]);
    }
    g_free(header_name);
    g_free(header_ip);
    for (i = 0; i < NM_SERIES_MAX; i++)
        g_free(rate_text[i]);
    cairo_restore(cr);
    g_object_unref(layout);
    cairo_destroy(cr);
    cairo_surface_mark_dirty(surface);
    return surface;
}

static void nm_rebuild(PrivData *priv)
{
    cairo_surface_t *replacement;

    if (!priv->plugin->win)
        return;
    replacement = nm_render(priv, priv->cache_width, priv->cache_height);
    if (priv->cache)
        cairo_surface_destroy(priv->cache);
    priv->cache = replacement;
}

/* ------------------------------------------------------------------ */
/* Жизненный цикл                                                      */
/* ------------------------------------------------------------------ */

/* Список интерфейсов из /proc/net/dev. lo по умолчанию скрыт: его
 * трафик — это loopback между процессами, а не сетевой обмен. */
static GPtrArray *nm_discover_interfaces(gboolean include_loopback)
{
    GPtrArray *names = g_ptr_array_new_with_free_func(g_free);
    char *text = NULL;
    NmNetSample samples[NM_MAX_IFACES];
    guint count, i;

    if (!g_file_get_contents(NM_NETDEV, &text, NULL, NULL))
        return names;
    count = nm_parse_netdev_all(text, samples, NM_MAX_IFACES);
    g_free(text);
    for (i = 0; i < count; i++) {
        if (!include_loopback && strcmp(samples[i].ifname, "lo") == 0) {
            g_free(samples[i].ifname);
            continue;
        }
        g_ptr_array_add(names, samples[i].ifname);
        samples[i].ifname = NULL;   /* перешёл во владение массива */
    }
    return names;
}

static int nm_init(XsPlugin *p, GKeyFile *kf)
{
    PrivData *priv = g_new0(PrivData, 1);
    GPtrArray *names;
    int x, y;
    gdouble opacity;
    guint i;

    p->priv = priv;
    priv->plugin = p;
    priv->kf = kf;
    priv->width = CLAMP(xs_host_api()->conf_int(kf, p->name, "window_width",
                                              NM_DEFAULT_WIDTH),
                        NM_MIN_WINDOW_WIDTH, 1600);
    priv->height = CLAMP(xs_host_api()->conf_int(kf, p->name, "window_height",
                                               NM_DEFAULT_HEIGHT),
                         NM_MIN_WINDOW_HEIGHT, 1200);
    priv->corner_radius = CLAMP(xs_host_api()->conf_int(kf, p->name,
                                                        "corner_radius", 6),
                                0, 200);
    /* Скругление окна не меньше скругления графика: иначе рамка окна
     * срезала бы скруглённые углы графика по диагонали. */
    priv->window_radius = CLAMP(xs_host_api()->conf_int(kf, p->name,
                                                        "window_radius", 8),
                                0, 200);
    priv->window_radius = MAX(priv->window_radius, priv->corner_radius);
    priv->design_width = priv->width;
    priv->design_height = priv->height;
    priv->update_ms = CLAMP(xs_host_api()->conf_int(kf, p->name, "update_ms",
                                                   1000), 100, 60000);
    priv->rate_smooth = CLAMP(xs_host_api()->conf_int(kf, p->name,
                                                     "rate_smooth", 1),
                              NM_RATE_SMOOTH_MIN, NM_RATE_SMOOTH_MAX);
    /* conf_int возвращает int: предел выше ~2 млн КиБ/с (2 Тбит/с) обрезался
     * бы, поэтому читаем через conf_dbl и округляем вниз. */
    priv->graph_max_kib = (gint64) xs_host_api()->conf_dbl(
        kf, p->name, "graph_max_kib", 0.0);
    /* Читаем СТРОКОЙ, потому что Properties пишет "split"/"combined":
     * conf_int() на таком значении молча вернул бы 0 и applet всегда был
     * бы в combined, что бы ни выбрал пользователь. */
    {
        char *mode = xs_host_api()->conf_str(kf, p->name, "graph_mode",
                                             "split");
        priv->graph_mode = (g_ascii_strcasecmp(mode, "combined") == 0)
                               ? NM_GRAPH_COMBINED : NM_GRAPH_SPLIT;
        g_free(mode);
    }
    /* Положение подписи — свой КЛЮЧ НА СЕРИЮ: properties пишет
     * series<N>_placement. Общий "label_placement" здесь ничего не давал
     * и молча сбрасывал бы обе подписи внутрь графика. */
    for (i = 0; i < NM_SERIES_MAX; i++) {
        char key[32];
        char *placement;

        g_snprintf(key, sizeof(key), "series%u_placement", i);
        placement = xs_host_api()->conf_str(kf, p->name, key, "inside");
        priv->label_placement[i] = nm_placement_from_string(placement);
        g_free(placement);

        /* У сводок свой выбор inside/outside, раньше его не было. */
        g_snprintf(key, sizeof(key), "total%u_placement", i);
        placement = xs_host_api()->conf_str(kf, p->name, key, "bottom");
        priv->total_placement[i] = nm_placement_from_string(placement);
        g_free(placement);
    }
    {
        char *placement;

        placement = xs_host_api()->conf_str(kf, p->name, "header_placement",
                                           "top");
        priv->header_placement = nm_placement_from_string(placement);
        g_free(placement);
    }

    names = nm_discover_interfaces(FALSE);
    priv->ifname = xs_host_api()->conf_str(kf, p->name, "ifname", NULL);
    if (!priv->ifname || !*priv->ifname) {
        g_free(priv->ifname);
        priv->ifname = names->len ? g_strdup(g_ptr_array_index(names, 0))
                                  : NULL;
    }
    /* Выбранного интерфейса может не быть (уехал, переименован, не
     * поднялся). Это НЕ фатально: имя остаётся в конфиге, applet
     * показывает N/A и ждёт возврата интерфейса. */
    g_ptr_array_free(names, TRUE);
    if (!priv->ifname || !*priv->ifname) {
        p->host->log("network_monitor %s: no network interfaces found",
                     p->name);
        g_free(priv);
        p->priv = NULL;
        return -1;
    }
    priv->ip = nm_interface_ipv4(priv->ifname);

    priv->font = xs_host_api()->conf_str(kf, p->name, "font", NM_DEFAULT_FONT);
    priv->label_font = xs_host_api()->conf_str(kf, p->name, "label_font",
                                               "Ubuntu Mono Bold 10");
    for (i = 0; i < NM_SERIES_MAX; i++) {
        char key[32];
        /* Подпись серии. Канонический ключ — series<N>_label, его пишет
         * диалог. Раньше здесь читались rx_label/tx_label, которых в
         * конфиге нет: пользователь правил series0_label=Down: через
         * Properties, а при старте applet подставлял дефолт «Down» и
         * молчал. Поэтому ключи разошлись, и настройка не работала
         * вообще — до первого открытия диалога. */
        g_snprintf(key, sizeof(key), "series%u_label", i);
        priv->series_label[i] = xs_host_api()->conf_str(
            kf, p->name, key, i == 0 ? "Down:" : "Up:");
        g_snprintf(key, sizeof(key), "series%u_label_font", i);
        priv->series_label_font[i] = xs_host_api()->conf_str(
            kf, p->name, key, "Sans 7");
        g_snprintf(key, sizeof(key), "series%u_value_font", i);
        priv->series_value_font[i] = xs_host_api()->conf_str(
            kf, p->name, key, "Terminus Bold 8");
        /* Три строки по вертикали, иначе подпись серии в режиме "снаружи"
         * накладывалась на строку интерфейса и обе исчезали:
         *   y=2  — имя интерфейса и IP
         *   y=12 — подпись серии (только в режиме "снаружи")
         *   y=21 — значение скорости
         * Подпись стоит в той же колонке, что и её значение. */
        g_snprintf(key, sizeof(key), "series%u_x", i);
        /* Вторая колонка сдвинута за разделитель: при ширине 420 первая
         * половина идёт от 2 до 212, зазор 8, вторая — от 220. */
        priv->series_x[i] = xs_host_api()->conf_int(
            kf, p->name, key, i == 0 ? 38 : 148);
        g_snprintf(key, sizeof(key), "series%u_y", i);
        priv->series_y[i] = xs_host_api()->conf_int(kf, p->name, key, 22);
        g_snprintf(key, sizeof(key), "series%u_label_x", i);
        priv->series_label_x[i] = xs_host_api()->conf_int(
            kf, p->name, key, i == 0 ? 4 : 112);
        g_snprintf(key, sizeof(key), "series%u_label_y", i);
        priv->series_label_y[i] = xs_host_api()->conf_int(kf, p->name, key,
                                                          22);
    }
    priv->header_x = xs_host_api()->conf_int(kf, p->name, "header_x", 8);
    priv->header_y = xs_host_api()->conf_int(kf, p->name, "header_y", 0);
    for (i = 0; i < NM_SERIES_MAX; i++) {
        char key[32];
        /* Подпись и значение разделены. Старые конфиги хранили строку
         * формата в total<N>_label («Total up: %s»); из неё берётся
         * только текст до %s, чтобы старый конфиг продолжал работать.
         * Новые ключи: total<N>_label — подпись, total<N>_value —
         * необязательный префикс значения (пусто = только число). */
        static const char *default_labels[NM_SERIES_MAX] = {
            "Total down:", "Total up:"
        };

        g_snprintf(key, sizeof(key), "total%u_label", i);
        priv->total_label[i] = xs_host_api()->conf_str(kf, p->name, key,
                                                       default_labels[i]);
        if (strstr(priv->total_label[i], "%s")) {
            /* Старый формат: отбрасываем « %s» и всё после него. */
            char *cut = strstr(priv->total_label[i], "%s");

            *cut = '\0';
            g_strchomp(priv->total_label[i]);
        }
        g_snprintf(key, sizeof(key), "total%u_value", i);
        priv->total_value[i] = xs_host_api()->conf_str(kf, p->name, key, "");
        g_snprintf(key, sizeof(key), "total%u_label_font", i);
        priv->total_label_font[i] = xs_host_api()->conf_str(
            kf, p->name, key, "Trebuchet MS 7");
        g_snprintf(key, sizeof(key), "total%u_value_font", i);
        priv->total_value_font[i] = xs_host_api()->conf_str(kf, p->name, key,
                                                            "Sans 7");
        g_snprintf(key, sizeof(key), "total%u_x", i);
        priv->total_x[i] = xs_host_api()->conf_int(
            kf, p->name, key, i == 0 ? 5 : 113);
        g_snprintf(key, sizeof(key), "total%u_y", i);
        priv->total_y[i] = xs_host_api()->conf_int(kf, p->name, key, 0);
        /* Позиция и цвета числа — свои, как у подписи. */
        /* Дефолт для числа сводки — сразу ЗА подписью, иначе старый
         * конфиг (где ключа не было, число ехало в подписи по шаблону)
         * получил бы число на подписи. Ширину подписи меряем её
         * шрифтом: «Total down:» — 52 px, «Total up:» — 45 px.
         *
         * Раньше дефолтом было жёсткое 60, и в конфиге пользователя
         * подпись стояла на x=8, то есть 8..60 — число ложилось прямо
         * на неё, и подпись исчезала. */
        /* Дефолт для числа сводки — сразу ЗА подписью, иначе старый
         * конфиг (где ключа не было, а число ехало в подписи по
         * шаблону) получил бы число прямо на подписи. Жёсткое 60
         * в первом варианте ложилось на «Total down:» с x=8.
         *
         * Ширина подписи при Sans 8: «Total down:» — 52 px,
         * «Total up:» — 45 px. Берём с запасом и добавляем
         * NM_VALUE_GAP, как при разрешении наезда. */
        g_snprintf(key, sizeof(key), "total%u_value_x", i);
        priv->total_value_x[i] = xs_host_api()->conf_int(
            kf, p->name, key, i == 0 ? 64 : 220);
        g_snprintf(key, sizeof(key), "total%u_value_y", i);
        priv->total_value_y[i] = xs_host_api()->conf_int(kf, p->name, key, 0);
    }

    nm_read_color(priv, "graph_background_color", nm_graph_bg_default,
                  priv->graph_bg);
    /* Фон окна по умолчанию тот же, что у графика: пока внешних подписей
     * нет, разницы не видно, и пользователь ничего не настраивает зря. */
    nm_read_color(priv, "window_background_color", nm_window_bg_default,
                  priv->window_bg);
    nm_read_color(priv, "border_color", nm_border_default, priv->border);
    nm_read_color(priv, "text_color", nm_text_default, priv->text_color);
    /* Цвета серий.
     *
     * Properties сохраняет выбор цвета в series<N>_color — это имя
     * стоит в коде диалога. А читались здесь rx_color и tx_color, то
     * есть ключи, которых в UI нет и которые никто не пишет. Выбор
     * цвета сохранялся в конфиг и не давал ничего: на экране оставался
     * дефолт.
     *
     * Читаем series<N>_color, а rx_color/tx_color оставлены как
     * legacy-источник: в конфигах, где серии ещё назывались по
     * направлению, цвета должны продолжать работать. Приоритет у
     * series<N>_color, потому что его пишет диалог. */
    for (i = 0; i < NM_SERIES_MAX; i++) {
        static const char *legacy_keys[NM_SERIES_MAX] = {
            "rx_color", "tx_color"
        };
        static const gdouble *defaults[NM_SERIES_MAX] = {
            nm_rx_default, nm_tx_default
        };
        char series_key[32];

        g_snprintf(series_key, sizeof(series_key), "series%d_color", i);
        /* Значение выбирается ОДИН раз: сначала пробуем
         * series<N>_color, а если его нет — legacy-ключ по направлению.
         * Проба и разбор делаются одним запросом, ключ не читается
         * дважды. */
        {
            char *probe = xs_host_api()->conf_str(priv->kf, priv->plugin->name,
                                                  series_key, NULL);
            gdouble parsed[4];

            if (!probe) {
                g_free(probe);
                probe = xs_host_api()->conf_str(priv->kf, priv->plugin->name,
                                                legacy_keys[i], NULL);
            }
            if (probe) {
                if (nm_parse_rgba(probe, parsed))
                    memcpy(priv->series_color[i], parsed, sizeof(parsed));
                else
                    memcpy(priv->series_color[i], defaults[i],
                           sizeof(gdouble) * 4);
            } else {
                memcpy(priv->series_color[i], defaults[i],
                       sizeof(gdouble) * 4);
            }
            g_free(probe);
        }
    }
    /* Цвет ТЕКСТА серии. По умолчанию он совпадает с цветом заливки
     * графика — так было раньше, и это же поведение при отсутствии
     * ключа в конфиге. Но ключ series<N>_text_color позволяет задать
     * текст отдельно: например, держать подпись «Down:» белым, а
     * заливку графика голубой.
     *
     * Ключа не было в UI, и в nm_init он не читался, так что ветка
     * обработчика была мёртвой. Теперь она живая. */
    for (i = 0; i < NM_SERIES_MAX; i++) {
        char text_key[40];

        g_snprintf(text_key, sizeof(text_key), "series%u_text_color", i);
        nm_read_color(priv, text_key, nm_series_text_default[i],
                      priv->series_text_color[i]);
    }

    /* Подпись и число — два отдельных цвета. Оба по умолчанию берут
     * общий цвет текста серии, то есть при отсутствии ключей вид не
     * меняется: подпись и число остаются того же цвета, что и раньше. */
    for (i = 0; i < NM_SERIES_MAX; i++) {
        char key[40];

        g_snprintf(key, sizeof(key), "series%u_label_color", i);
        nm_read_color(priv, key, nm_series_label_default[i],
                      priv->series_label_color[i]);
        g_snprintf(key, sizeof(key), "series%u_value_color", i);
        nm_read_color(priv, key, nm_series_value_default[i],
                      priv->series_value_color[i]);
    }

    /* Сводка — два элемента, у каждого свои цвета. Общий текст
     * (total<N>_text_color) остаётся значением по умолчанию для обоих. */
    for (i = 0; i < NM_SERIES_MAX; i++) {
        char key[40];

        g_snprintf(key, sizeof(key), "total%u_text_color", i);
        nm_read_color(priv, key, nm_series_text_default[i],
                      priv->total_text_color[i]);
        g_snprintf(key, sizeof(key), "total%u_color", i);
        nm_read_color(priv, key, nm_total_color_default[i],
                      priv->total_color[i]);
        g_snprintf(key, sizeof(key), "total%u_value_color", i);
        nm_read_color(priv, key, nm_total_value_default[i],
                      priv->total_value_color[i]);
    }

    /* Имя интерфейса и адрес — два элемента, у каждого свой цвет. */
    nm_read_color(priv, "header_ifname_color", priv->text_color,
                  priv->header_ifname_color);
    nm_read_color(priv, "header_ip_color", nm_header_ip_default,
                  priv->header_ip_color);

    g_key_file_set_string(kf, p->name, "ifname", priv->ifname);
    g_key_file_set_string(kf, p->name, "font", priv->font);
    g_key_file_set_string(kf, p->name, "rx_label", priv->series_label[0]);
    g_key_file_set_string(kf, p->name, "tx_label", priv->series_label[1]);
    for (i = 0; i < NM_SERIES_MAX; i++) {
        char key[32];
        g_snprintf(key, sizeof(key), "total%u_label", i);
        g_key_file_set_string(kf, p->name, key, priv->total_label[i]);
        g_snprintf(key, sizeof(key), "total%u_label_font", i);
        g_key_file_set_string(kf, p->name, key, priv->total_label_font[i]);
        g_snprintf(key, sizeof(key), "total%u_x", i);
        g_key_file_set_integer(kf, p->name, key, priv->total_x[i]);
        g_snprintf(key, sizeof(key), "total%u_y", i);
        g_key_file_set_integer(kf, p->name, key, priv->total_y[i]);
    }
    g_key_file_set_integer(kf, p->name, "window_width", priv->width);
    g_key_file_set_integer(kf, p->name, "window_height", priv->height);
    g_key_file_set_integer(kf, p->name, "update_ms", priv->update_ms);
    g_key_file_set_string(kf, p->name, "graph_mode",
                          (priv->graph_mode == NM_GRAPH_SPLIT) ? "split"
                                                              : "combined");
    for (i = 0; i < NM_SERIES_MAX; i++) {
        char key[32];
        g_snprintf(key, sizeof(key), "series%u_placement", i);
        g_key_file_set_string(kf, p->name, key,
                              nm_placement_to_string(priv->label_placement[i]));
    }
    g_key_file_set_string(kf, p->name, "header_placement",
                          nm_placement_to_string(priv->header_placement));
    for (i = 0; i < NM_SERIES_MAX; i++) {
        char tkey[32];

        g_snprintf(tkey, sizeof(tkey), "total%u_placement", i);
        g_key_file_set_string(kf, p->name, tkey,
                              nm_placement_to_string(priv->total_placement[i]));
    }
    g_key_file_set_integer(kf, p->name, "rate_smooth", priv->rate_smooth);
    g_key_file_set_integer(kf, p->name, "corner_radius",
                           priv->corner_radius);
    g_key_file_set_integer(kf, p->name, "window_radius",
                           priv->window_radius);
    xs_core_plugin_conf_flush(p->name);

    x = xs_host_api()->conf_int(kf, p->name, "x", 80);
    y = xs_host_api()->conf_int(kf, p->name, "y", 80);
    opacity = xs_host_api()->conf_dbl(kf, p->name, "opacity", 1.0);
    p->win = xs_host_api()->make_window(p, x, y, priv->width, priv->height);
    if (!p->win) {
        p->host->log("network_monitor: failed to create window");
        nm_shutdown(p);
        return -1;
    }
    priv->cache_width = priv->width;
    priv->cache_height = priv->height;
    nm_sample(priv);
    priv->cache = nm_render(priv, priv->width, priv->height);
    xs_host_api()->set_opacity(p, CLAMP(opacity, 0.1, 1.0));
    xs_host_api()->set_tick(p, priv->update_ms);
    return 0;
}

static guint nm_tick(XsPlugin *p)
{
    PrivData *priv = p ? p->priv : NULL;

    if (!priv || !p->win)
        return 0;
    nm_sample(priv);
    nm_rebuild(priv);
    xs_host_api()->invalidate(p);
    gtk_widget_queue_draw(p->win);
    return priv->update_ms;
}

/* Скруглённый контур как ВИДИМАЯ фигура окна. cairo-клип в nm_render()
 * ограничивает только рисование плагина; само X-окно остаётся
 * прямоугольным, пока не задан shape — именно поэтому углы были
 * квадратными на экране. NULL снимает shape (corner_radius=0). */
static void nm_apply_shape(XsPlugin *p, int w, int h)
{
    PrivData *priv = p ? p->priv : NULL;
    GdkWindow *window;
    cairo_region_t *region;

    if (!priv || !p->win || w <= 0 || h <= 0)
        return;
    if (priv->shape_radius == priv->corner_radius &&
        priv->shape_w == w && priv->shape_h == h)
        return;
    window = gtk_widget_get_window(p->win);
    if (!window)
        return;
    region = nm_rounded_region(w, h, priv->corner_radius);
    gdk_window_shape_combine_region(window, region, 0, 0);
    if (region)
        cairo_region_destroy(region);
    priv->shape_radius = priv->corner_radius;
    priv->shape_w = w;
    priv->shape_h = h;
}

static void nm_draw(XsPlugin *p, cairo_t *cr, int w, int h)
{
    PrivData *priv = p ? p->priv : NULL;

    if (!priv || !priv->cache)
        return;
    if (w != priv->cache_width || h != priv->cache_height) {
        priv->cache_width = w;
        priv->cache_height = h;
        nm_rebuild(priv);
    }
    nm_apply_shape(p, w, h);
    cairo_set_source_surface(cr, priv->cache, 0, 0);
    cairo_paint(cr);
}

static void nm_shutdown(XsPlugin *p)
{
    PrivData *priv = p ? p->priv : NULL;

    if (!priv)
        return;
    /* Запись таблицы указывала бы на уже освобождённый priv: обработчик
     * Properties после перезапуска плагина получил бы висячий указатель. */
    if (nm_instances)
        g_hash_table_remove(nm_instances, p->name);
    if (p->win) {
        GdkWindow *win = gtk_widget_get_window(p->win);
        if (win)
            gdk_window_shape_combine_region(win, NULL, 0, 0);
    }
    if (priv->cache)
        cairo_surface_destroy(priv->cache);
    g_free(priv->ifname);
    g_free(priv->ip);
    g_free(priv->font);
    g_free(priv->label_font);
    {
        guint i;
        for (i = 0; i < NM_SERIES_MAX; i++) {
            g_free(priv->series_label_font[i]);
            g_free(priv->series_value_font[i]);
            g_free(priv->series_label[i]);
            g_free(priv->total_label[i]);
            g_free(priv->total_value[i]);
            g_free(priv->total_label_font[i]);
            g_free(priv->total_value_font[i]);
        }
    }
    g_free(priv);
    p->priv = NULL;
}

static int nm_find_combo_text(GtkComboBox *combo, const char *text)
{
    GtkTreeModel *model;
    GtkTreeIter iter;
    gboolean valid;

    if (!combo || !text)
        return -1;
    model = gtk_combo_box_get_model(combo);
    if (!model)
        return -1;
    valid = gtk_tree_model_get_iter_first(model, &iter);
    while (valid) {
        gchar *value = NULL;
        gtk_tree_model_get(model, &iter, 0, &value, -1);
        if (value && !strcmp(value, text)) {
            GtkTreePath *path = gtk_tree_model_get_path(model, &iter);
            gint index = path ? gtk_tree_path_get_indices(path)[0] : -1;
            g_free(value);
            if (path)
                gtk_tree_path_free(path);
            return index;
        }
        g_free(value);
        valid = gtk_tree_model_iter_next(model, &iter);
    }
    return -1;
}

/* Живой экземпляр, к которому относится диалог. Таблица живёт на уровне
 * файла: nm_properties регистрирует priv в ней, а обработчики ищут его
 * по имени плагина — иначе Properties не видел бы живой applet. */
static void nm_instances_ensure(void)
{
    if (!nm_instances)
        nm_instances = g_hash_table_new(g_str_hash, g_str_equal);
}

static PrivData *nm_live_priv(NmDialogContext *ctx)
{
    gpointer data;

    if (!ctx)
        return NULL;
    nm_instances_ensure();
    data = g_hash_table_lookup(nm_instances, nm_dialog_context_name(ctx));
    return data;
}

static void nm_position_changed(GtkSpinButton *spin, gpointer data)
{
    NmDialogContext *ctx = data;
    PrivData *priv = nm_live_priv(ctx);
    const char *key;
    int value;
    XsPlugin *p;
    char numeric_key[32];
    int series_index = -1;

    if (!priv)
        return;
    p = priv->plugin;
    key = g_object_get_data(G_OBJECT(spin), "xs-key");
    if (!key)
        return;
    value = (int) gtk_spin_button_get_value(spin);

    if (g_str_has_prefix(key, "series") &&
        sscanf(key, "series%d_%31s", &series_index, numeric_key) == 2) {
        if (series_index >= 0 && series_index < NM_SERIES_MAX) {
            if (!strcmp(numeric_key, "x"))
                priv->series_x[series_index] = value;
            else if (!strcmp(numeric_key, "y"))
                priv->series_y[series_index] = value;
            else if (!strcmp(numeric_key, "label_x"))
                priv->series_label_x[series_index] = value;
            else if (!strcmp(numeric_key, "label_y"))
                priv->series_label_y[series_index] = value;
        }
    } else if (g_str_has_prefix(key, "total") &&
               sscanf(key, "total%d_%31s", &series_index, numeric_key) == 2) {
        if (series_index >= 0 && series_index < NM_SERIES_MAX) {
            if (!strcmp(numeric_key, "x"))
                priv->total_x[series_index] = value;
            else if (!strcmp(numeric_key, "y"))
                priv->total_y[series_index] = value;
            else if (!strcmp(numeric_key, "value_x"))
                priv->total_value_x[series_index] = value;
            else if (!strcmp(numeric_key, "value_y"))
                priv->total_value_y[series_index] = value;
        }
    } else if (!strcmp(key, "header_x")) {
        priv->header_x = value;
    } else if (!strcmp(key, "header_y")) {
        priv->header_y = value;
    } else if (!strcmp(key, "window_width")) {
        priv->width = value;
    } else if (!strcmp(key, "window_height")) {
        priv->height = value;
    } else if (!strcmp(key, "header_placement")) {
        priv->header_placement = value ? NM_LABEL_OUTSIDE
                                      : NM_LABEL_INSIDE;
    } else if (g_str_has_prefix(key, "total") &&
               g_str_has_suffix(key, "_placement")) {
        guint n = 0;

        if (sscanf(key, "total%u_placement", &n) == 1 &&
            n < NM_SERIES_MAX)
            priv->total_placement[n] = value ? NM_LABEL_OUTSIDE
                                             : NM_LABEL_INSIDE;
    } else if (!strcmp(key, "window_radius")) {
        priv->window_radius = value;
    } else if (!strcmp(key, "corner_radius")) {
        priv->corner_radius = value;
        /* Рамка окна не может стать меньше только что выбранного
         * скругления графика — иначе окно срежет углы графика. */
        if (priv->window_radius < priv->corner_radius) {
            priv->window_radius = priv->corner_radius;
            value = priv->window_radius;
        }
    } else if (!strcmp(key, "graph_max_kib")) {
        priv->graph_max_kib = value;
    } else if (!strcmp(key, "rate_smooth")) {
        priv->rate_smooth = CLAMP(value, NM_RATE_SMOOTH_MIN,
                                  NM_RATE_SMOOTH_MAX);
        value = priv->rate_smooth;
    } else if (!strcmp(key, "update_ms")) {
        priv->update_ms = value;
        xs_host_api()->set_tick(p, value);
    }
    /* graph_max_kib может не влезть в int, поэтому пишем его строкой
     * через conf_dbl-совместимый путь; остальные ключи — обычный int. */
    if (!strcmp(key, "graph_max_kib")) {
        char *text = g_strdup_printf("%ld", (long) priv->graph_max_kib);
        g_key_file_set_string(priv->kf, p->name, key, text);
        g_free(text);
    } else
        g_key_file_set_integer(priv->kf, p->name, key, value);
    xs_core_plugin_conf_flush(p->name);
    xs_host_api()->resize(p, priv->width, priv->height);
    priv->cache_width = priv->width;
    priv->cache_height = priv->height;
    nm_rebuild(priv);
    gtk_widget_queue_draw(p->win);
}

static void nm_label_changed(GtkEditable *entry, gpointer data)
{
    NmDialogContext *ctx = data;
    PrivData *priv = nm_live_priv(ctx);
    const char *key;
    const char *value;
    XsPlugin *p;
    int series_index = -1;
    char tail[32];

    if (!priv)
        return;
    p = priv->plugin;
    key = g_object_get_data(G_OBJECT(entry), "xs-key");
    if (!key)
        return;
    value = gtk_entry_get_text(GTK_ENTRY(entry));
    if (g_str_has_prefix(key, "series") &&
        sscanf(key, "series%d_%31s", &series_index, tail) == 2) {
        if (series_index >= 0 && series_index < NM_SERIES_MAX &&
            !strcmp(tail, "label")) {
            g_free(priv->series_label[series_index]);
            priv->series_label[series_index] = g_strdup(value);
        }
    } else if (g_str_has_prefix(key, "total")) {
        /* У сводки два текстовых поля: подпись и необязательный префикс
         * значения. */
        if (sscanf(key, "total%d_%31s", &series_index, tail) == 2 &&
            series_index >= 0 && series_index < NM_SERIES_MAX) {
            if (!strcmp(tail, "label")) {
                g_free(priv->total_label[series_index]);
                priv->total_label[series_index] = g_strdup(value);
            } else if (!strcmp(tail, "value")) {
                g_free(priv->total_value[series_index]);
                priv->total_value[series_index] = g_strdup(value);
            }
        }
    }
    g_key_file_set_string(priv->kf, p->name, key, value);
    xs_core_plugin_conf_flush(p->name);
    nm_rebuild(priv);
    if (p->win)
        gtk_widget_queue_draw(p->win);
}

/* Определена ниже, в блоке хелперов Properties. */
static void nm_pos_set_y_visible(GtkWidget *y, gboolean visible);

static void nm_placement_changed(GtkComboBox *combo, gpointer data)
{
    NmDialogContext *ctx = data;
    PrivData *priv = nm_live_priv(ctx);
    gint active;
    const char *key;
    GtkWidget *yy;
    NmLabelPlacement place;
    gboolean inside;
    XsPlugin *p;

    if (!priv || !GTK_IS_COMBO_BOX(combo))
        return;
    p = priv->plugin;
    key = g_object_get_data(G_OBJECT(combo), "xs-key");
    if (!key)
        return;
    active = gtk_combo_box_get_active(combo);

    place = nm_placement_from_combo(active);
    inside = (place == NM_LABEL_INSIDE);
    if (!strcmp(key, "header_placement")) {
        priv->header_placement = place;
    } else if (g_str_has_prefix(key, "series")) {
        int idx = -1;

        if (sscanf(key, "series%d_", &idx) != 1 || idx < 0 ||
            idx >= NM_SERIES_MAX)
            return;
        priv->label_placement[idx] = place;
    } else if (g_str_has_prefix(key, "total")) {
        int idx = -1;

        if (sscanf(key, "total%d_", &idx) != 1 || idx < 0 ||
            idx >= NM_SERIES_MAX)
            return;
        priv->total_placement[idx] = place;
    } else {
        return;
    }

    g_key_file_set_string(priv->kf, p->name, key,
                          nm_placement_to_string(place));
    xs_core_plugin_conf_flush(p->name);
    nm_rebuild(priv);
    if (p->win)
        gtk_widget_queue_draw(p->win);

    /* Поле Y показывается только «внутри»: снаружи элемент прижат к
     * рамке окна, и высота не должна настраиваться. Скрываем сразу, не
     * дожидаясь переоткрытия диалога. */
    yy = g_object_get_data(G_OBJECT(combo), NM_CTX_KEY ".yy");
    if (yy && GTK_IS_WIDGET(yy))
        nm_pos_set_y_visible(yy, inside);
}

static void nm_graph_mode_changed(GtkComboBox *combo, gpointer data)
{
    NmDialogContext *ctx = data;
    PrivData *priv = nm_live_priv(ctx);
    gint active;
    XsPlugin *p;

    if (!priv)
        return;
    p = priv->plugin;
    active = gtk_combo_box_get_active(combo);
    priv->graph_mode = (active == 1) ? NM_GRAPH_COMBINED : NM_GRAPH_SPLIT;
    g_key_file_set_string(priv->kf, p->name, "graph_mode",
                          (active == 1) ? "combined" : "split");
    xs_core_plugin_conf_flush(p->name);
    nm_rebuild(priv);
    if (p->win)
        gtk_widget_queue_draw(p->win);
}

static void nm_iface_changed(GtkComboBox *combo, gpointer data)
{
    NmDialogContext *ctx = data;
    PrivData *priv = nm_live_priv(ctx);
    GtkTreeModel *model;
    GtkTreeIter iter;
    gchar *value = NULL;
    XsPlugin *p;

    if (!priv)
        return;
    p = priv->plugin;
    model = gtk_combo_box_get_model(combo);
    if (!model || !gtk_combo_box_get_active_iter(combo, &iter))
        return;
    gtk_tree_model_get(model, &iter, 0, &value, -1);
    if (value) {
        g_free(priv->ifname);
        priv->ifname = g_strdup(value);
        g_free(priv->ip);
        priv->ip = nm_interface_ipv4(priv->ifname);
        /* Смена цели обнуляет baseline: иначе первая дельта после
         * переключения считалась бы от счётчиков прошлого интерфейса. */
        priv->prev_valid = FALSE;
        priv->sample_valid = FALSE;
        memset(priv->history, 0, sizeof(priv->history));
        memset(priv->head, 0, sizeof(priv->head));
        memset(priv->count, 0, sizeof(priv->count));
        memset(priv->total_bytes, 0, sizeof(priv->total_bytes));
        memset(priv->current_rate, 0, sizeof(priv->current_rate));
        memset(priv->smooth, 0, sizeof(priv->smooth));
        g_key_file_set_string(priv->kf, p->name, "ifname", priv->ifname);
        xs_core_plugin_conf_flush(p->name);
        nm_rebuild(priv);
        if (p->win)
            gtk_widget_queue_draw(p->win);
        g_free(value);
    }
}

static void nm_color_set(GtkColorButton *button, gpointer data)
{
    NmDialogContext *ctx = data;
    PrivData *priv = nm_live_priv(ctx);
    const char *key;
    GdkRGBA rgba;
    gdouble out[4];
    int series_index = -1;
    char tail[32];
    XsPlugin *p;

    if (!priv)
        return;
    p = priv->plugin;
    key = g_object_get_data(G_OBJECT(button), "xs-key");
    if (!key)
        return;
    gtk_color_chooser_get_rgba(GTK_COLOR_CHOOSER(button), &rgba);
    out[0] = rgba.red;
    out[1] = rgba.green;
    out[2] = rgba.blue;
    out[3] = rgba.alpha;
    /* У alpha-only селекторов (фон, рамка) берём alpha ИЗ КНОПКИ, у RGB-only
     * берём её из сохранённого состояния: gtk_color_chooser без шкалы
     * возвращает alpha=1.0 и обнулил бы прозрачность при первом же клике. */
    {
        gboolean rgb_only = g_object_get_data(G_OBJECT(button),
                                              "xs-rgb-only") ? TRUE : FALSE;
        if (rgb_only) {
            const gdouble *current = NULL;
            if (g_str_has_prefix(key, "series") &&
                sscanf(key, "series%d_%31s", &series_index, tail) == 2) {
                if (series_index >= 0 && series_index < NM_SERIES_MAX)
                    current = priv->series_color[series_index];
            } else if (!strcmp(key, "text_color")) {
                current = priv->text_color;
            }
            if (current)
                out[3] = current[3];
        }
    }
    if (g_str_has_prefix(key, "series") &&
        sscanf(key, "series%d_%31s", &series_index, tail) == 2) {
        if (series_index >= 0 && series_index < NM_SERIES_MAX) {
            if (!strcmp(tail, "color")) {
                memcpy(priv->series_color[series_index], out,
                       sizeof(gdouble) * 4);
                memcpy(priv->series_text_color[series_index], out,
                       sizeof(gdouble) * 4);
            } else if (!strcmp(tail, "text_color")) {
                memcpy(priv->series_text_color[series_index], out,
                       sizeof(gdouble) * 4);
                /* Общий цвет текста — это значение по умолчанию для
                 * подписи и числа, поэтому их явные цвета должны
                 * уехать вместе с ним, иначе кнопка «текст» не влияла
                 * бы на подпись и число. */
                memcpy(priv->series_label_color[series_index], out,
                       sizeof(gdouble) * 4);
                memcpy(priv->series_value_color[series_index], out,
                       sizeof(gdouble) * 4);
            } else if (!strcmp(tail, "label_color")) {
                memcpy(priv->series_label_color[series_index], out,
                       sizeof(gdouble) * 4);
            } else if (!strcmp(tail, "value_color")) {
                memcpy(priv->series_value_color[series_index], out,
                       sizeof(gdouble) * 4);
            }
        }
    } else if (!strcmp(key, "graph_background_color")) {
        memcpy(priv->graph_bg, out, sizeof(gdouble) * 4);
    } else if (!strcmp(key, "window_background_color")) {
        /* Кнопка была в диалоге с самого начала, а ветки в обработчике
         * не было: priv->window_bg обновлялся только при перезапуске
         * applet, то есть по кнопке фон окна не менялся. */
        memcpy(priv->window_bg, out, sizeof(gdouble) * 4);
    } else if (!strcmp(key, "border_color")) {
        memcpy(priv->border, out, sizeof(gdouble) * 4);
    } else if (!strcmp(key, "text_color")) {
        memcpy(priv->text_color, out, sizeof(gdouble) * 4);
        /* text_color — значение по умолчанию для обоих элементов шапки,
         * поэтому их явные цвета должны уехать вместе с ним. */
        memcpy(priv->header_ifname_color, out, sizeof(gdouble) * 4);
        memcpy(priv->header_ip_color, out, sizeof(gdouble) * 4);
    } else if (!strcmp(key, "header_ifname_color")) {
        memcpy(priv->header_ifname_color, out, sizeof(gdouble) * 4);
    } else if (!strcmp(key, "header_ip_color")) {
        memcpy(priv->header_ip_color, out, sizeof(gdouble) * 4);
    } else if (g_str_has_prefix(key, "total")) {
        /* Сводка: общий текст, подпись и число — три разных цвета.
         * Общий текст уводит за собой оба, иначе кнопка перестала бы на
         * них влиять. */
        unsigned n = 0;
        char t[32];

        if (sscanf(key, "total%u_%31s", &n, t) == 2 && n < NM_SERIES_MAX) {
            if (!strcmp(t, "text_color")) {
                memcpy(priv->total_text_color[n], out, sizeof(gdouble) * 4);
                memcpy(priv->total_color[n], out, sizeof(gdouble) * 4);
                memcpy(priv->total_value_color[n], out, sizeof(gdouble) * 4);
            } else if (!strcmp(t, "color")) {
                memcpy(priv->total_color[n], out, sizeof(gdouble) * 4);
            } else if (!strcmp(t, "value_color")) {
                memcpy(priv->total_value_color[n], out, sizeof(gdouble) * 4);
            }
        }
    }
    g_key_file_set_string(priv->kf, p->name, key, nm_format_rgba(out));
    xs_core_plugin_conf_flush(p->name);
    nm_rebuild(priv);
    if (p->win)
        gtk_widget_queue_draw(p->win);
}

static void nm_series_font_set(GtkFontButton *button, gpointer data)
{
    NmDialogContext *ctx = data;
    PrivData *priv = nm_live_priv(ctx);
    const char *key;
    const char *value;
    int series_index = -1;
    char tail[32];
    XsPlugin *p;

    if (!priv)
        return;
    p = priv->plugin;
    key = g_object_get_data(G_OBJECT(button), "xs-key");
    if (!key)
        return;
    value = gtk_font_button_get_font_name(GTK_FONT_BUTTON(button));
    /* tail заполняется одним из двух sscanf ниже, а не обоими: второй
     * перезаписал бы первый, и ветка total прочитала бы хвост от ключа
     * series. Поэтому выбираем формат по префиксу. */
    series_index = -1;
    tail[0] = '\0';
    if (g_str_has_prefix(key, "series")) {
        if (sscanf(key, "series%d_%31s", &series_index, tail) == 2 &&
            series_index >= 0 && series_index < NM_SERIES_MAX) {
            if (!strcmp(tail, "label_font")) {
                g_free(priv->series_label_font[series_index]);
                priv->series_label_font[series_index] = g_strdup(value);
            } else if (!strcmp(tail, "value_font")) {
                g_free(priv->series_value_font[series_index]);
                priv->series_value_font[series_index] = g_strdup(value);
            } else if (!strcmp(tail, "font")) {
                /* Старый общий ключ: подпись и число шли одним шрифтом. */
                g_free(priv->series_label_font[series_index]);
                priv->series_label_font[series_index] = g_strdup(value);
                g_free(priv->series_value_font[series_index]);
                priv->series_value_font[series_index] = g_strdup(value);
            }
        }
    } else if (g_str_has_prefix(key, "total")) {
        /* У сводки два независимых шрифта: подписи и числа. */
        if (sscanf(key, "total%d_%31s", &series_index, tail) == 2 &&
            series_index >= 0 && series_index < NM_SERIES_MAX) {
            if (!strcmp(tail, "label_font")) {
                g_free(priv->total_label_font[series_index]);
                priv->total_label_font[series_index] = g_strdup(value);
            } else if (!strcmp(tail, "value_font")) {
                g_free(priv->total_value_font[series_index]);
                priv->total_value_font[series_index] = g_strdup(value);
            }
        }
    } else if (!strcmp(key, "label_font")) {
        /* Шрифт шапки: имя интерфейса и адрес.
         *
         * Ключ не начинается ни с «series», ни с «total», поэтому раньше
         * он не попадал ни в одну ветку выше. Строка записи в файл ниже
         * общая и срабатывала, но priv->label_font в памяти оставался
         * прежним — applet рисовал «wan0» и IP старым шрифтом до
         * перезапуска демона. Ровно то, на что жаловался пользователь:
         * «header шрифт и размер шрифта не влияют на заголовок». */
        g_free(priv->label_font);
        priv->label_font = g_strdup(value);
    }
    g_key_file_set_string(priv->kf, p->name, key, value);
    xs_core_plugin_conf_flush(p->name);
    nm_rebuild(priv);
    if (p->win)
        gtk_widget_queue_draw(p->win);
}

/* Рекурсивная привязка по типу виджета: у ключевых контролов
 * разная сигнатура (font-set, color-set, value-changed), и дублировать
 * хрупкий обход по вложенным индексам в плагине не стоит. */
static void nm_bind_keyed_descendants(GtkWidget *widget, NmDialogContext *ctx)
{
    if (!widget)
        return;
    if (GTK_IS_FONT_BUTTON(widget)) {
        g_signal_connect(widget, "font-set",
                         G_CALLBACK(nm_series_font_set), ctx);
        return;
    }
    if (GTK_IS_COLOR_BUTTON(widget)) {
        g_signal_connect(widget, "color-set",
                         G_CALLBACK(nm_color_set), ctx);
        return;
    }
    if (GTK_IS_SPIN_BUTTON(widget)) {
        g_signal_connect(widget, "value-changed",
                         G_CALLBACK(nm_position_changed), ctx);
        return;
    }
    if (GTK_IS_ENTRY(widget)) {
        g_signal_connect(widget, "changed", G_CALLBACK(nm_label_changed), ctx);
        return;
    }
    if (GTK_IS_COMBO_BOX(widget)) {
        const char *key = g_object_get_data(G_OBJECT(widget), "xs-key");
        if (key && g_str_has_suffix(key, "placement"))
            g_signal_connect(widget, "changed",
                             G_CALLBACK(nm_placement_changed), ctx);
        else if (key && !strcmp(key, "graph_mode"))
            g_signal_connect(widget, "changed",
                             G_CALLBACK(nm_graph_mode_changed), ctx);
        else
            g_signal_connect(widget, "changed",
                             G_CALLBACK(nm_iface_changed), ctx);
        return;
    }
    if (GTK_IS_CONTAINER(widget)) {
        GList *children = gtk_container_get_children(GTK_CONTAINER(widget));
        GList *it;
        for (it = children; it; it = it->next)
            nm_bind_keyed_descendants(GTK_WIDGET(it->data), ctx);
        g_list_free(children);
    }
}

/* Секция настроек: грид + счётчик строк.
 *
 * Раньше каждый хелпер жёстко писал в строку 0, и все поля секции
 * ложились в одну строку друг на друга. Счётчик двигает каждую пару
 * подпись/контрол на свою строку. */
typedef struct {
    GtkWidget *grid;
    int row;
} NmGrid;

static NmGrid *nm_grid_new(void)
{
    NmGrid *g = g_new0(NmGrid, 1);

    g->grid = gtk_grid_new();
    gtk_grid_set_column_spacing(GTK_GRID(g->grid), 8);
    gtk_grid_set_row_spacing(GTK_GRID(g->grid), 4);
    return g;
}

/* Подпись поля. Перевод ЗДЕСЬ, а не в вызовах nm_add_*(..., "Label"):
 * их десятки, и каждый литерал пришлось бы оборачивать вручную.
 * Аргумент label - видимая подпись; ключи конфигов идут отдельным
 * параметром key и сюда не попадают. */
static void nm_grid_add_label(NmGrid *g, const char *label)
{
    GtkWidget *l = gtk_label_new(_(label));

    gtk_widget_set_halign(l, GTK_ALIGN_START);
    gtk_grid_attach(GTK_GRID(g->grid), l, 0, g->row, 1, 1);
}

static void nm_grid_add_widget(NmGrid *g, GtkWidget *w)
{
    gtk_grid_attach(GTK_GRID(g->grid), w, 1, g->row, 1, 1);
    g->row++;
}

/* Пара координат в ОДНОЙ строке. Восемь отдельных полей Position X/Y и
 * Label X/Y разворачивали каждую секцию серии вчетверо и делали диалог
 * вчетверо длиннее; в строке "X / Y" те же восемь значений занимают
 * две строки на серию. */
/* Пара координат. Возвращает Y-спин, чтобы вызывающий спрятал его
 * вместе с Placement: снаружи высота элемента считается по фазе, и
 * ручной Y там ломал бы симметрию отступов. */
static GtkWidget *nm_add_xy(NmGrid *g, NmDialogContext *ctx,
                            const char *label,
                            const char *key_x, const char *key_y,
                            int value_x, int value_y, int max)
{
    GtkWidget *box = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 4);
    GtkWidget *sx = gtk_spin_button_new_with_range(0, max, 1);
    GtkWidget *sy = gtk_spin_button_new_with_range(0, max, 1);

    gtk_spin_button_set_value(GTK_SPIN_BUTTON(sx), value_x);
    gtk_spin_button_set_value(GTK_SPIN_BUTTON(sy), value_y);
    /* Три цифры: у плагина NM_MAX_POS = 2047, а у спина с кнопками
     * прокрутки поле занимает пять знаков. Убираем прокрутку — ввод
     * с клавиатуры остаётся, колесо тоже работает. */
    gtk_spin_button_set_numeric(GTK_SPIN_BUTTON(sx), FALSE);
    gtk_spin_button_set_numeric(GTK_SPIN_BUTTON(sy), FALSE);
    gtk_entry_set_max_width_chars(GTK_ENTRY(sx), 3);
    gtk_entry_set_max_width_chars(GTK_ENTRY(sy), 3);
    gtk_widget_set_size_request(sx, NM_DIALOG_ENTRY, -1);
    gtk_widget_set_size_request(sy, NM_DIALOG_ENTRY, -1);
    g_object_set_data_full(G_OBJECT(sx), "xs-key", g_strdup(key_x), g_free);
    g_object_set_data_full(G_OBJECT(sy), "xs-key", g_strdup(key_y), g_free);
    gtk_box_pack_start(GTK_BOX(box), sx, TRUE, TRUE, 0);
    gtk_box_pack_start(GTK_BOX(box), gtk_label_new(_("/")), FALSE, FALSE, 0);
    gtk_box_pack_start(GTK_BOX(box), sy, TRUE, TRUE, 0);
    nm_grid_add_label(g, label);
    nm_grid_add_widget(g, box);
    nm_bind_keyed_descendants(sx, ctx);
    nm_bind_keyed_descendants(sy, ctx);
    return sy;
}


/* Позиция элемента: X всегда, Y — только если элемент выбран «внутри».
 *
 * Снаружи Y не должен волновать: элемент прижат к рамке окна сверху или
 * снизу, и ручная настройка высоты ломала бы симметрию отступов. Раньше
 * в диалоге стояла пара X/Y всегда, но Y молча игнорировался — пользователь
 * двигал спин-кнопку и ничего не происходило.
 *
 * Y всегда создаётся, но в «Outside» скрыт: gtk_grid не умеет прятать
 * ячейку, зато умеет скрывать виджет, и строка схлопывается сама.
 * Скрытие повторяется обработчиком смены Placement, поэтому видно сразу,
 * а не после переоткрытия диалога. */
static void nm_pos_set_y_visible(GtkWidget *y, gboolean visible)
{
    if (y)
        gtk_widget_set_visible(y, visible);
}

/* Возвращает Y-спин, чтобы вызывающий связал его с комбо Placement. */
static GtkWidget *nm_add_pos(NmGrid *g, NmDialogContext *ctx,
                             const char *label,
                             const char *key_x, const char *key_y,
                             int value_x, int value_y, int max,
                             gboolean show_y)
{
    GtkWidget *box = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 4);
    GtkWidget *sx = gtk_spin_button_new_with_range(0, max, 1);
    GtkWidget *sy = gtk_spin_button_new_with_range(0, max, 1);

    gtk_spin_button_set_value(GTK_SPIN_BUTTON(sx), value_x);
    gtk_spin_button_set_value(GTK_SPIN_BUTTON(sy), value_y);
    /* Три цифры: у плагина NM_MAX_POS = 2047, а у спина с кнопками
     * прокрутки поле занимает пять знаков. Убираем прокрутку — ввод
     * с клавиатуры остаётся, колесо тоже работает. */
    gtk_spin_button_set_numeric(GTK_SPIN_BUTTON(sx), FALSE);
    gtk_spin_button_set_numeric(GTK_SPIN_BUTTON(sy), FALSE);
    gtk_entry_set_max_width_chars(GTK_ENTRY(sx), 3);
    gtk_entry_set_max_width_chars(GTK_ENTRY(sy), 3);
    gtk_widget_set_size_request(sx, NM_DIALOG_ENTRY, -1);
    gtk_widget_set_size_request(sy, NM_DIALOG_ENTRY, -1);
    g_object_set_data_full(G_OBJECT(sx), "xs-key", g_strdup(key_x), g_free);
    g_object_set_data_full(G_OBJECT(sy), "xs-key", g_strdup(key_y), g_free);
    gtk_box_pack_start(GTK_BOX(box), sx, TRUE, TRUE, 0);
    gtk_box_pack_start(GTK_BOX(box), gtk_label_new(_("/")), FALSE, FALSE, 0);
    gtk_box_pack_start(GTK_BOX(box), sy, TRUE, TRUE, 0);
    nm_grid_add_label(g, label);
    nm_grid_add_widget(g, box);
    nm_bind_keyed_descendants(sx, ctx);
    nm_bind_keyed_descendants(sy, ctx);
    nm_pos_set_y_visible(sy, show_y);
    return sy;
}

static void nm_add_int(NmGrid *g, NmDialogContext *ctx, const char *key,
                       const char *label, int value, int min, int max)
{
    GtkWidget *spin = gtk_spin_button_new_with_range(min, max, 1);

    gtk_spin_button_set_value(GTK_SPIN_BUTTON(spin), value);
    gtk_widget_set_size_request(spin, 90, -1);
    /* КЛЮЧ КОПИРУЕТСЯ. Раньше сюда передавался указатель на буфер в стеке
     * nm_properties(); GTK хранит его в виджете, а к моменту клика стек
     * давно перезаписан — обработчик читал мусор, и настройка не влияла
     * ни на что. */
    g_object_set_data_full(G_OBJECT(spin), "xs-key", g_strdup(key), g_free);
    nm_grid_add_label(g, label);
    nm_grid_add_widget(g, spin);
    nm_bind_keyed_descendants(spin, ctx);
}

static void nm_add_color(NmGrid *g, NmDialogContext *ctx, const char *key,
                         const char *label, const gdouble color[4],
                         gboolean with_alpha)
{
    GtkWidget *button = gtk_color_button_new_with_rgba(&(GdkRGBA) {
        color[0], color[1], color[2], color[3] });

    gtk_color_chooser_set_use_alpha(GTK_COLOR_CHOOSER(button), with_alpha);
    if (!with_alpha)
        g_object_set_data(G_OBJECT(button), "xs-rgb-only", GINT_TO_POINTER(1));
    g_object_set_data_full(G_OBJECT(button), "xs-key", g_strdup(key), g_free);
    nm_grid_add_label(g, label);
    nm_grid_add_widget(g, button);
    nm_bind_keyed_descendants(button, ctx);
}

/* Поле ввода подписи. Длина ограничена NM_DIALOG_LABEL_CHARS: в секции
 * рядом стоят кнопка шрифта и кнопка цвета, и длинная подпись
 * растягивала бы всю секцию за пределы окна диалога.
 *
 * Ограничение на ввод, а не на отображение: в конфиг попадает ровно то,
 * что введено, и длинная строка из старого конфига читается целиком —
 * обрезать её молча было бы потерей данных. */
#define NM_DIALOG_LABEL_CHARS 10

static GtkWidget *nm_color_button(const char *key, const gdouble color[4],
                                  gboolean with_alpha, const char *tip)
{
    GtkWidget *cb = gtk_color_button_new_with_rgba(&(GdkRGBA) {
        color[0], color[1], color[2], color[3] });

    gtk_color_chooser_set_use_alpha(GTK_COLOR_CHOOSER(cb), with_alpha);
    if (!with_alpha)
        g_object_set_data(G_OBJECT(cb), "xs-rgb-only", GINT_TO_POINTER(1));
    g_object_set_data_full(G_OBJECT(cb), "xs-key", g_strdup(key), g_free);
    if (tip)
        gtk_widget_set_tooltip_text(cb, tip);
    return cb;
}

static void nm_add_font(NmGrid *g, NmDialogContext *ctx, const char *key,
                        const char *label, const char *value)
{
    GtkWidget *button = gtk_font_button_new_with_font(value ? value : "Sans 8");

    gtk_widget_set_size_request(button, NM_DIALOG_FONT, -1);
    g_object_set_data_full(G_OBJECT(button), "xs-key", g_strdup(key), g_free);
    nm_grid_add_label(g, label);
    nm_grid_add_widget(g, button);
    nm_bind_keyed_descendants(button, ctx);
}

/* combo_y: Y-спин, который нужно показывать/прятать вместе с этим
 * Placement. Может быть NULL. */
static GtkWidget *nm_add_combo(NmGrid *g, NmDialogContext *ctx,
                               const char *key, const char *label,
                               const char *const *options, int active,
                               GtkWidget *combo_y)
{
    GtkWidget *combo = gtk_combo_box_text_new();
    int i;

    for (i = 0; options[i]; i++)
        gtk_combo_box_text_append(GTK_COMBO_BOX_TEXT(combo), options[i],
                                  options[i]);
    gtk_combo_box_set_active(GTK_COMBO_BOX(combo), active);
    /* Ширина под самую длинную подпись — «Outside bottom». Уже, и GTK
     * обрезает её по «Outside bott…», и вариант перестаёт читаться. */
    gtk_widget_set_size_request(combo, NM_DIALOG_COMBO, -1);
    g_object_set_data_full(G_OBJECT(combo), "xs-key", g_strdup(key), g_free);
    nm_grid_add_label(g, label);
    nm_grid_add_widget(g, combo);
    nm_bind_keyed_descendants(combo, ctx);
    if (combo_y)
        g_object_set_data(G_OBJECT(combo), NM_CTX_KEY ".yy",
                          combo_y);
    return combo;
}

/* Блок «Label» или «Value» элемента.
 *
 * У серии и у сводки одинаковый набор: текст, шрифт, цвет и X/Y.
 * Собран одним хелпером, иначе серия и сводка разъезжаются по
 * содержимому: у серии было четыре цвета одной строкой, у сводки
 * один на всю строку с числом.
 *
 * placement_key + combo_y — параметры nm_add_combo; combo_y может быть
 * NULL, если Y не нужно прятать (у серии и сводки прячется). */
static void nm_add_inline_text(NmGrid *g, NmDialogContext *ctx,
                               const char *label_txt,
                               const char *text_key, const char *text,
                               const char *font_key, const char *font,
                               const char *color_key,
                               const gdouble color[4], const char *tip)
{
    GtkWidget *row = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 4);

    if (text_key) {
        GtkWidget *entry = gtk_entry_new();

        gtk_entry_set_text(GTK_ENTRY(entry), text ? text : "");
        gtk_entry_set_max_length(GTK_ENTRY(entry), NM_DIALOG_LABEL_CHARS);
        gtk_widget_set_size_request(entry, NM_DIALOG_LABEL, -1);
        g_object_set_data_full(G_OBJECT(entry), "xs-key", g_strdup(text_key),
                               g_free);
        gtk_box_pack_start(GTK_BOX(row), entry, FALSE, FALSE, 0);
        nm_bind_keyed_descendants(entry, ctx);
    }
    if (font_key) {
        GtkWidget *fb = gtk_font_button_new_with_font(font ? font
                                                            : "Sans 8");

        gtk_widget_set_size_request(fb, NM_DIALOG_FONT, -1);
        g_object_set_data_full(G_OBJECT(fb), "xs-key", g_strdup(font_key),
                               g_free);
        gtk_box_pack_start(GTK_BOX(row), fb, FALSE, FALSE, 0);
        nm_bind_keyed_descendants(fb, ctx);
    }
    if (color_key) {
        gtk_box_pack_start(GTK_BOX(row),
                           nm_color_button(color_key, color, FALSE, tip),
                           FALSE, FALSE, 0);
    }
    nm_grid_add_label(g, label_txt ? label_txt : "");
    nm_grid_add_widget(g, row);
    if (row)
        nm_bind_keyed_descendants(row, ctx);
}

/* Заголовок секции переводится здесь, а не в вызовах
 * nm_section(..., "Appearance"): заголовков десятки. */
static GtkWidget *nm_section(GtkWidget *page, const char *title)
{
    GtkWidget *frame = gtk_frame_new(_(title));

    gtk_box_pack_start(GTK_BOX(page), frame, FALSE, FALSE, 4);
    return frame;
}

static void nm_properties(XsPlugin *p, GtkNotebook *notebook)
{
    PrivData *priv = p->priv;
    NmDialogContext *ctx;
    GtkWidget *page, *frame;
    NmGrid *g;
    GPtrArray *names;
    int i;

    if (!priv)
        return;
    nm_instances_ensure();
    g_hash_table_insert(nm_instances, g_strdup(p->name), priv);
    ctx = nm_dialog_context_new(p->name);

    page = gtk_box_new(GTK_ORIENTATION_VERTICAL, 6);
    gtk_container_set_border_width(GTK_CONTAINER(page), 8);

    /* --- Интерфейс и график --- */
    frame = nm_section(page, "Interface");
    g = nm_grid_new();
    {
        guint index;
        GtkWidget *combo;

        names = nm_discover_interfaces(FALSE);
        combo = gtk_combo_box_text_new();
        for (index = 0; index < names->len; index++)
            gtk_combo_box_text_append(GTK_COMBO_BOX_TEXT(combo),
                                      g_ptr_array_index(names, index),
                                      g_ptr_array_index(names, index));
        index = (guint) MAX(0, nm_find_combo_text(GTK_COMBO_BOX(combo),
                                                 priv->ifname));
        gtk_combo_box_set_active(GTK_COMBO_BOX(combo), (gint) index);
        g_object_set_data_full(G_OBJECT(combo), "xs-key",
                               g_strdup("ifname"), g_free);
        nm_grid_add_label(g, "Interface");
        nm_grid_add_widget(g, combo);
        nm_bind_keyed_descendants(combo, ctx);
        g_ptr_array_free(names, TRUE);
    }
    {
        static const char *modes[] = {"Split", "Combined", NULL};
        nm_add_combo(g, ctx, "graph_mode", "Graph", modes,
                     priv->graph_mode == NM_GRAPH_SPLIT ? 0 : 1, NULL);
    }
    /* Предел шкалы в КиБ/с: 0 = по максимуму истории. Шкала общая для обеих
     * серий, поэтому одна настройка на оба графика. */
    nm_add_int(g, ctx, "graph_max_kib", "Graph max (KiB/s, 0=auto)",
               (int) priv->graph_max_kib, 0, 2000000);
    gtk_container_add(GTK_CONTAINER(frame), g->grid);

    /* --- Серии --- по схеме Placement / Label / Value / Graph color.
     *
     * Каждая настройка в своей строке. Важно: координаты НЕ дублируются.
     * Раньше строка Placement рисовала X/Y сама (через nm_add_pos), и
     * ниже стоял ещё один nm_add_xy с теми же ключами series%d_label_x/y.
     * Два виджета на один ключ: меняешь число в верхнем — нижнее до
     * переоткрытия диалога показывает старое, и наоборот. Это ровно тот
     * баг, что был с кнопкой цвета заливки.
     *
     * Теперь Placement — только выпадающий список, а X/Y живут в своей
     * строке рядом с тем элементом, который двигают: подпись — с Label,
     * число — с Value. Y прячется снаружи, как и раньше. */
    for (i = 0; i < NM_SERIES_MAX; i++) {
        char title[32];
        char kx[40], ky[40], key[40];
        char k_lbl[40], k_lblf[40], k_lblc[40], k_valf[40], k_valc[40],
             k_fill[40];
        static const char *placements[] = {"Inside graph", "Outside top",
                                        "Outside bottom", NULL};
        gboolean inside = priv->label_placement[i] == NM_LABEL_INSIDE;
        GtkWidget *ly, *vy;

        g_snprintf(title, sizeof(title), "%s:",
                   i == 0 ? "Download" : "Upload");
        frame = nm_section(page, title);
        g = nm_grid_new();

        /* Ключи для следующих строк. X/Y идут ПЕРЕД Placement, потому что
         * Placement прячет Y-спин подписи, а значит должен получить его
         * указатель. */
        g_snprintf(kx, sizeof(kx), "series%d_label_x", i);
        g_snprintf(ky, sizeof(ky), "series%d_label_y", i);
        g_snprintf(key, sizeof(key), "series%d_placement", i);

        /* Label: текст, шрифт, цвет — одной строкой, X/Y — следующей */
        g_snprintf(k_lbl, sizeof(k_lbl), "series%d_label", i);
        g_snprintf(k_lblf, sizeof(k_lblf), "series%d_label_font", i);
        g_snprintf(k_lblc, sizeof(k_lblc), "series%d_label_color", i);
        nm_add_inline_text(g, ctx, "Label", k_lbl, priv->series_label[i],
                           k_lblf, priv->series_label_font[i], k_lblc,
                           priv->series_label_color[i],
                           "Colour of the «Down:» / «Up:» label");
        ly = nm_add_xy(g, ctx, "Label X/Y", kx, ky, priv->series_label_x[i],
                       priv->series_label_y[i],
                       MAX(priv->design_width, priv->design_height) - 1);
        nm_pos_set_y_visible(ly, inside);
        priv->series_y_spin[i] = ly;

        /* Placement — после X/Y: комбо получает указатель на Y-спин
         * подписи и прячет его, когда выбрано «Outside top/bottom». */
        nm_add_combo(g, ctx, key, "Placement", placements,
                     (int) priv->label_placement[i], priv->series_y_spin[i]);

        /* Value: шрифт и цвет той же строкой, текста у числа нет */
        g_snprintf(k_valf, sizeof(k_valf), "series%d_value_font", i);
        g_snprintf(k_valc, sizeof(k_valc), "series%d_value_color", i);
        nm_add_inline_text(g, ctx, "Value", NULL, NULL, k_valf,
                           priv->series_value_font[i], k_valc,
                           priv->series_value_color[i],
                           "Colour of the speed value");
        g_snprintf(kx, sizeof(kx), "series%d_x", i);
        g_snprintf(ky, sizeof(ky), "series%d_y", i);
        vy = nm_add_xy(g, ctx, "Value X/Y", kx, ky, priv->series_x[i],
                       priv->series_y[i],
                       MAX(priv->design_width, priv->design_height) - 1);
        nm_pos_set_y_visible(vy, TRUE);

        /* Graph color */
        g_snprintf(k_fill, sizeof(k_fill), "series%d_color", i);
        nm_add_color(g, ctx, k_fill, "Graph color", priv->series_color[i],
                     FALSE);

        gtk_container_add(GTK_CONTAINER(frame), g->grid);
        g_free(g);
    }

    /* --- Оформление --- */
    frame = nm_section(page, "Appearance");
    g = nm_grid_new();
    nm_add_color(g, ctx, "graph_background_color", "Graph background",
                 priv->graph_bg, TRUE);
    nm_add_color(g, ctx, "window_background_color", "Window background",
                 priv->window_bg, TRUE);
    nm_add_color(g, ctx, "text_color", "Text", priv->text_color, FALSE);
    nm_add_color(g, ctx, "border_color", "Graph border", priv->border, TRUE);
    gtk_container_add(GTK_CONTAINER(frame), g->grid);
    g_free(g);

    /* --- Заголовок --- отдельной секцией. Раньше он был завален в
     * Appearance вместе со сводками, а выбора inside/outside у него не
     * было вовсе: заголовок всегда рисовался в графике. */
    frame = nm_section(page, "Header");
    g = nm_grid_new();
    nm_add_font(g, ctx, "label_font", "Font", priv->label_font);
    {
        static const char *placements[] = {"Inside graph", "Outside top",
                                        "Outside bottom", NULL};
        /* Y-спин создаём ДО комбо: комбо получает его указателем, чтобы
         * прятать поле сразу при переключении Placement. */
        GtkWidget *hy = nm_add_pos(g, ctx, "Position", "header_x",
                                   "header_y", priv->header_x, priv->header_y,
                                   MAX(priv->design_width,
                                       priv->design_height) - 1,
                                   priv->header_placement == NM_LABEL_INSIDE);

        g_object_ref_sink(hy);
        nm_add_combo(g, ctx, "header_placement", "Placement", placements,
                     (int) priv->header_placement, hy);
        g_object_unref(hy);
    }
    /* Цвета шапки — здесь же, а не в Appearance. Шапка состоит из двух
     * элементов, и каждый красится своим цветом:
     *   header_ifname_color  имя интерфейса, слева
     *   header_ip_color      адрес, прижат к правому краю
     * Оба по умолчанию берут общий text_color, который остаётся здесь
     * как значение по умолчанию для всего неописанного текста. */
    {
        GtkWidget *row = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 4);

        gtk_box_pack_start(GTK_BOX(row), gtk_label_new(_("Name")), FALSE,
                           FALSE, 0);
        gtk_box_pack_start(GTK_BOX(row),
                           nm_color_button("header_ifname_color",
                                           priv->header_ifname_color, FALSE,
                                           "Цвет имени интерфейса"),
                           FALSE, FALSE, 0);
        gtk_box_pack_start(GTK_BOX(row), gtk_label_new(_("IP")), FALSE,
                           FALSE, 0);
        gtk_box_pack_start(GTK_BOX(row),
                           nm_color_button("header_ip_color",
                                           priv->header_ip_color, FALSE,
                                           "Цвет адреса"),
                           FALSE, FALSE, 0);
        nm_grid_add_widget(g, row);
        nm_bind_keyed_descendants(row, ctx);
    }
    gtk_container_add(GTK_CONTAINER(frame), g->grid);
    g_free(g);

    /* --- Сводки (overall) --- по той же схеме: Placement / Label /
     * Value / Graph color, но у сводки вместо цвета графика ничего
     * лишнего — она вне графика.
     *
     * Подпись и число разделены. Раньше это была одна строка с форматом
     * «Total up: %s», собранная nm_format_label(), и число ехало в
     * подписи по шаблону: поставить его в другую точку было нельзя. */
    for (i = 0; i < NM_SERIES_MAX; i++) {
        char title[40];
        char kx[40], ky[40], key[40];
        char k_lbl[40], k_lblf[40], k_lblc[40], k_val[40], k_valf[40],
             k_valc[40];
        static const char *placements[] = {"Inside graph", "Outside top",
                                        "Outside bottom", NULL};
        gboolean inside = priv->total_placement[i] == NM_LABEL_INSIDE;
        GtkWidget *ly;

        g_snprintf(title, sizeof(title), "%s overall:",
                   i == 0 ? "Download" : "Upload");
        frame = nm_section(page, title);
        g = nm_grid_new();

        /* X/Y идут перед Placement: Placement прячет Y-спин подписи, а
         * значит должен получить его указатель. Дублировать координаты в
         * двух строках нельзя — это два виджета на одни ключи, и нижний
         * показывал бы старое значение до переоткрытия диалога. */
        g_snprintf(kx, sizeof(kx), "total%u_x", i);
        g_snprintf(ky, sizeof(ky), "total%u_y", i);

        /* Label: текст, шрифт, цвет */
        g_snprintf(k_lbl, sizeof(k_lbl), "total%u_label", i);
        g_snprintf(k_lblf, sizeof(k_lblf), "total%u_label_font", i);
        g_snprintf(k_lblc, sizeof(k_lblc), "total%u_color", i);
        nm_add_inline_text(g, ctx, "Label", k_lbl, priv->total_label[i],
                           k_lblf, priv->total_label_font[i], k_lblc,
                           priv->total_color[i], "Colour of the summary label");
        ly = nm_add_xy(g, ctx, "Label X/Y", kx, ky, priv->total_x[i],
                       priv->total_y[i],
                       MAX(priv->design_width, priv->design_height) - 1);
        nm_pos_set_y_visible(ly, inside);

        /* Placement — только список */
        g_snprintf(key, sizeof(key), "total%u_placement", i);
        nm_add_combo(g, ctx, key, "Placement", placements,
                     (int) priv->total_placement[i], ly);

        /* Value: необязательный текст, шрифт, цвет */
        g_snprintf(k_val, sizeof(k_val), "total%u_value", i);
        g_snprintf(k_valf, sizeof(k_valf), "total%u_value_font", i);
        g_snprintf(k_valc, sizeof(k_valc), "total%u_value_color", i);
        nm_add_inline_text(g, ctx, "Value", k_val, priv->total_value[i],
                           k_valf, priv->total_value_font[i], k_valc,
                           priv->total_value_color[i], "Colour of the summary value");
        g_snprintf(kx, sizeof(kx), "total%u_value_x", i);
        g_snprintf(ky, sizeof(ky), "total%u_value_y", i);
        nm_add_xy(g, ctx, "Value X/Y", kx, ky, priv->total_value_x[i],
                  priv->total_value_y[i],
                  MAX(priv->design_width, priv->design_height) - 1);

        gtk_container_add(GTK_CONTAINER(frame), g->grid);
        g_free(g);
    }

    /* --- Окно --- */
    frame = nm_section(page, "Window");
    g = nm_grid_new();
    {
        /* Ширина и высота несут разные минимумы, поэтому пара
         * nm_add_xy с общим максимумом тут не годится. */
        nm_add_int(g, ctx, "window_width", "Width", priv->width,
                   NM_MIN_WINDOW_WIDTH, 1600);
        nm_add_int(g, ctx, "window_height", "Height", priv->height,
                   NM_MIN_WINDOW_HEIGHT, 1200);
    }
    nm_add_int(g, ctx, "corner_radius", "Graph corner", priv->corner_radius,
               0, 200);
    /* Нижний предел — текущее скругление графика: меньше нельзя, иначе
     * рамка окна срежет углы графика. */
    nm_add_int(g, ctx, "window_radius", "Window corner", priv->window_radius,
               priv->corner_radius, 200);
    nm_add_int(g, ctx, "update_ms", "Update (ms)", priv->update_ms, 100,
               60000);
    nm_add_int(g, ctx, "rate_smooth", "Smoothing", priv->rate_smooth,
               NM_RATE_SMOOTH_MIN, NM_RATE_SMOOTH_MAX);
    gtk_container_add(GTK_CONTAINER(frame), g->grid);
    g_free(g);

    /* Страница длиннее окна диалога: без прокрутки Properties вырос бы
     * под все строки и ушёл бы за пределы экрана. */
    {
        GtkWidget *tab = page;
        int width, height;

        nm_properties_size(p->type, &width, &height);
        if (width > 0 && height > 0) {
            GtkWidget *scroller = nm_properties_scroller(page, width, height);
            if (scroller)
                tab = scroller;
        }
        /* Контекст живёт ДОЛЬШЕ диалога: каждый контрол держит его в
         * g_signal_connect(user_data) и при срабатывании читает
         * instance_name. Освобождать его здесь нельзя — обработчик
         * получал бы указатель на уже освобождённую строку, и
         * g_hash_table_lookup() падал бы внутри g_str_hash(). Ловится
         * как segfault при прокрутке диалога колесом мыши. */
        g_object_set_data_full(G_OBJECT(tab), NM_CTX_KEY, ctx,
                               (GDestroyNotify) nm_dialog_context_unref);
        gtk_notebook_append_page(notebook, tab,
                                 gtk_label_new(_("Network Monitor")));
        gtk_widget_show_all(tab);
        /* show_all показывает ВСЕ виджеты, включая только что спрятанные
         * Y-спины. Поэтому «Placement: снаружи» при первом открытии
         * диалога показывал поле, которое ничего не делает: скрытие
         * применялось при создании, а show_all его тут же отменял.
         * Обработчик смены Placement повторял скрытие, но только после
         * клика по комбо.
         *
         * Повторяем скрытие для всех серий, которым оно полагается.
         * nm_pos_set_y_visible вызывается выше, при nm_add_pos, — там
         * проходит priv->label_placement, то есть текущее значение из
         * конфига. */
        for (i = 0; i < NM_SERIES_MAX; i++) {
            if (priv->series_y_spin[i])
                nm_pos_set_y_visible(priv->series_y_spin[i],
                                     priv->label_placement[i]
                                     == NM_LABEL_INSIDE);
        }
    }
}

static const XsPluginOps nm_ops = {
    .init = nm_init,
    .draw = nm_draw,
    .tick = nm_tick,
    .shutdown = nm_shutdown,
    .properties = nm_properties,
};
static XsPluginDesc nm_desc = {
    "network_monitor", XS_API_VERSION, &nm_ops,
    "Per-interface up/down speed and history monitor",
    "xscreenlets", "1.0"
};
XS_PLUGIN_EXPORT(&nm_desc)
