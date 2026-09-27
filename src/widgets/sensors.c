/* sensors.c — апплет температур: список «метка: число», без графиков.
 *
 * Каркас (окно, скругление, масштаб координат, текст, диалог) взят по
 * образцу network_monitor: там уже отлажены rgba-окно, shape по контуру
 * окна и компактные контролы Properties. Своё ядро — sensors_core.c,
 * потому что данные здесь другие (hwmon, а не /proc).
 *
 * Наследие сетевого апплета, которое стоит знать при правках:
 *
 *  - Округление рисуется ДВАЖДЫ: cairo-клип в draw() ограничивает
 *    только рисование плагина, само X-окно остаётся прямоугольным, пока
 *    не задан shape. Без shape клики в углы попадают в невидимый угол.
 *  - Скругление окна не меньше скругления содержимого, иначе рамка
 *    срезает содержимое по диагонали.
 *  - Каждая настройка имеет три независимых места: билдер в UI,
 *    ветка обработчика и чтение в init. Пропуск любого выглядит как
 *    «кнопка не работает» — и проверено это на практике.
 */
#include "sensors_core.h"
#include "sensors_nvml.h"

#include "xs_api.h"
#include "common.h"

#include <math.h>
#include <string.h>

static char *sen_source_key(const char *chip, const char *device,
                            const char *label)
{
    return sensor_row_id(chip, device, label);
}


/* ------------------------------------------------------------- размеры */

#define SEN_DEFAULT_WIDTH   200
#define SEN_DEFAULT_HEIGHT  100
#define SEN_MIN_WIDTH       80
#define SEN_MIN_HEIGHT      40
/* Пользователь просил скругление 2-4 px по умолчанию. 3 — середина
 * диапазона: 2 почти не читается на глаз, 4 заметно срезает углы на
 * панели 200px. */
#define SEN_DEFAULT_RADIUS  3
#define SEN_MAX_RADIUS      40
/* Максимум строк в апплете. 21 чип на этой машине даёт 46 каналов —
 * больше 40 строк в окно не влезет, и лишнее просто не рисуется. */
#define SEN_MAX_ROWS        40
#define SEN_VALUE_GAP     6   /* зазор подпись->число, как в network_monitor */
#define SEN_MARGIN          6
#define SEN_LABEL_GAP       4
#define SEN_DIALOG_FONT     180
#define SEN_DIALOG_COMBO   132
#define SEN_SPIN_DIGITS     3
/* «строки нет» в поиске индекса: 0 — реальный индекс, поэтому
 * нельзя использовать 0 как признак отсутствия */
#define SEN_ROW_NONE ((guint) -1)

/* ----------------------------------------------------------------- данные */

typedef struct {
    XsPlugin *plugin;
    GKeyFile *kf;

    int width, height;
    gboolean height_auto;   /* высоту не задавали — считаем по строкам */
    gboolean width_auto;    /* ширину не задавали — считаем по подписям */
    int design_width, design_height;
    int corner_radius;      /* окно; содержимое скругляется тем же */
    int update_ms;
    gdouble opacity;
    gboolean fahrenheit;

    char *label_font;
    char *value_font;
    gdouble label_color[4];
    gdouble value_color[4];
    gdouble background_color[4];
    gdouble border_color[4];

    int label_x, value_x, first_row_y;
    gboolean value_align_right;   /* число прижато вправо (по умолчанию) */
    int line_step;

    /* Список строк: «метка» = «чип/канал» (например «nvme0/Composite»),
     * показывается подпись. Сами строки в конфиге хранятся как
     * «метка;чип;канал» — по ключу собирать список нельзя, потому что
     * метка не уникальна (четыре nvme дают четыре «Composite»). */
    GPtrArray *rows;        /* char* — подпись, как её видит пользователь */
    GPtrArray *row_sources; /* char* — «чип/канал», для разбора */

    /* кэш отрисовки */
    cairo_surface_t *cache;
    int cache_width, cache_height;

    /* shape: кэш применённой формы, чтобы не пересчитывать каждый кадр */
    int shape_radius, shape_w, shape_h;

    /* значения последнего чтения: индекс строки -> текущая строка «58.0°C»
     * либо NULL, если канала нет */
    GPtrArray *values;     /* char* на каждый rows->len */
    gboolean values_valid;
} SenPriv;

/* Совпадает ли подпись строки с формой «имя группы · канал»?
 *
 * Диалог показывает группу («nct7904-i2c», «drivetemp-scsi»), а полное
 * имя sensors(1) — с коротким слотом («nct7904-i2c-1-2d»). Подпись
 * записывается такая, какая была в таблице, поэтому при восстановлении
 * источника надо понимать обе формы. */
static gboolean sen_label_is_group_form(const char *label, SensorChip *c,
                                        const char *channel)
{
    char *group = sensor_group_name(c->chip, c->dev_path);
    char *want = g_strdup_printf("%s · %s", group, channel);
    gboolean hit = g_strcmp0(want, label) == 0;

    g_free(want);
    g_free(group);
    return hit;
}

/* Источник битый, если не находит сенсор.
 *
 * Раньше сюда попадало читаемое имя («nct7904-i2c · temp3») вместо
 * ключа «чип/устройство/канал». Такая строка молча показывала подпись
 * без числа — заметить можно было только глазами. */
static gboolean sen_source_is_broken(SenPriv *priv, SensorList *found,
                                     const char *source)
{
    if (!source || !*source)
        return TRUE;
    /* Источник NVIDIA не лежит в sysfs, поэтому поиск по дереву hwmon
     * для него всегда даёт «нет». Без этой проверки миграция на каждом
     * запуске пыталась бы «починить» живую строку карты, а чистка
     * мёртвых строк — удалила бы её совсем. */
    if (nvml_source_is_nvidia(source))
        return FALSE;
    if (!found)
        return FALSE;   /* дерево не прочитано — чинить нечего */
    return sensor_find_reading(found, source) == NULL;
}

static gboolean sen_config_has_broken_sources(SenPriv *priv)
{
    SensorList *found = sensor_list_read("/sys/class/hwmon");
    gboolean any = FALSE;

    if (!found)
        return FALSE;
    for (guint i = 0; i < priv->row_sources->len; i++) {
        if (sen_source_is_broken(priv, found,
                                 g_ptr_array_index(priv->row_sources, i))) {
            any = TRUE;
            break;
        }
    }
    sensor_list_free(found);
    return any;
}


/* Контекст диалога. Живёт дольше properties(): контролы держат его
 * указателем, и по имени инстанса находят ЖИВОЙ priv. Имя — это же
 * имя секции конфига. */
typedef struct {
    SenPriv *priv;
    char *instance_name;
    /* Пока строится список сенсоров, обработчики молчат. Список
     * создаётся ДО sen_connect_children(), но gtk_entry_set_text() при
     * заполнении всё равно шлёт «changed» на каждое из 46 полей — и
     * каждый вызов писал конфиг и перестраивал кеш, то есть 46 раз
     * за одно открытие Properties. */
    gboolean building;
} SenDialogContext;

/* Таблица «имя инстанса -> priv». Нужна, потому что контекст диалога
 * переживает properties(), а priv может быть пересоздан: без таблицы
 * обработчик читает освобождённую память. */
static GHashTable *sen_instances;

/* ------------------------------------------------------------------ путь */

/* Идентификатор строки в конфиге: «чип/устройство/канал».
 *
 * Устройство в середине — обязательная часть, а не украшение. Имя чипа
 * не уникально (четыре nvme = «nvme», 13 дисков = «drivetemp»), а номер
 * hwmon меняется между загрузками, поэтому строка «nvme/Composite»
 * после перезагрузки указала бы на другой диск. Полный путь sysfs
 * устройства устойчив — это тот же приём, что /dev/disk/by-id в
 * disk_monitor. */

/* ---------------------------------------------------------------- текст */

static void sen_set_color(cairo_t *cr, const gdouble color[4])
{
    cairo_set_source_rgba(cr, color[0], color[1], color[2], color[3]);
}

/* Ширина строки в пикселях по имени шрифта. Контекст cairo передаётся
 * явно: pango_cairo_create_layout() берёт его из «текущего», а
 * cairo_get_current_cr() вне cairo_save/restore даёт NULL — и на этом
 * падал компилятор. */
static int sen_text_width(cairo_t *cr, const char *text, const char *font)
{
    PangoFontDescription *fd = pango_font_description_from_string(font);
    PangoRectangle logical;
    PangoLayout *layout;
    int width;

    if (!text || !*text || !cr)
        return 0;
    layout = pango_cairo_create_layout(cr);
    pango_layout_set_font_description(layout, fd);
    pango_layout_set_text(layout, text, -1);
    pango_layout_get_extents(layout, NULL, &logical);
    width = logical.width / PANGO_SCALE;
    g_object_unref(layout);
    pango_font_description_free(fd);
    return width;
}


/* Высота строки по имени шрифта. Pango отдаёт логические единицы —
 * делить на PANGO_SCALE обязательно, иначе полоса становится в 1024
 * раза выше окна и весь текст исчезает. */
static int sen_row_height(cairo_t *cr, const char *font)
{
    PangoFontDescription *fd = pango_font_description_from_string(font);
    PangoRectangle logical;
    PangoLayout *layout;
    int height;

    if (!cr)
        return 1;
    layout = pango_cairo_create_layout(cr);
    pango_layout_set_font_description(layout, fd);
    pango_layout_get_extents(layout, NULL, &logical);
    height = logical.height / PANGO_SCALE;
    if (height < 1)
        height = 1;
    g_object_unref(layout);
    pango_font_description_free(fd);
    return height;
}

/* Реальный шаг строк: НЕ МЕНЬШЕ высоты текста.
 *
 * line_step в конфиге — это шаг при компактном шрифте. Пользователь,
 * увеличивший шрифт в Настройках, получал наезд строк друг на друга:
 * при line_step=12 и Sans 10 высота строки 13, и вторая ложилась на
 * первую. Оба места — расчёт высоты окна в init и отрисовка — обязаны
 * брать шаг отсюда, иначе окно считается по одному шагу, а текст
 * рисуется по другому, и последняя строка уезжает за нижний край.
 *
 * Шрифт меряется тем же Pango, что и отрисовка, на 1x1-поверхности:
 * отдельная поверхность того же размера не нужна, измерение не зависит
 * от размеров контекста. */
static int sen_effective_step(cairo_t *cr, int line_step,
                             const char *label_font, const char *value_font)
{
    int text_h;

    if (!cr)
        return MAX(line_step, 1);
    text_h = MAX(sen_row_height(cr, label_font),
                 sen_row_height(cr, value_font));
    return MAX(line_step, text_h);
}

/* Шрифты заданы строками в design-пикселях, окно можно растянуть —
 * масштабируем координаты и кегль по фактическому размеру. */
static int sen_scale(int value, int design, int actual, int max)
{
    if (actual <= 0 || design <= 0)
        return value;
    return (int) ((gint64) value * (max > 0 ? max : actual - 1) / design);
}

/* Кегль, масштабированный под фактическую ширину окна.
 *
 * pango_font_description_get_size() отдаёт размер в единицах PANGO_SCALE
 * (1024 = 1 пункт), а не в пунктах: для "Sans 8" это 8192. Подстановка
 * этого числа в описание шрифта даёт "Sans 8192" — окно рисует рамку, а
 * текст не виден совсем, потому что глифы не помещаются ни в одну
 * строку. Делить на PANGO_SCALE обязательно.
 *
 * Окно не трогаем при design == actual: масштаб 1 иначе округлит кегль
 * вниз на нечётном размере окна и текст «поплывёт» между кадрами. */
static const char *sen_scale_font(const char *font, int design, int actual)
{
    PangoFontDescription *fd = pango_font_description_from_string(font);
    const char *family = pango_font_description_get_family(fd);
    int points = pango_font_description_get_size(fd) / PANGO_SCALE;
    int scaled;
    static char buf[128];

    if (design <= 0 || actual <= 0)
        design = actual = 1;
    scaled = (points * actual) / design;
    if (scaled < 1)
        scaled = 1;
    g_snprintf(buf, sizeof(buf), "%s %d", family ? family : "Sans", scaled);
    pango_font_description_free(fd);
    return buf;
}

static void sen_show_text(cairo_t *cr, const char *font, int x, int baseline,
                          const char *text, const gdouble color[4],
                          int max_width)
{
    PangoFontDescription *fd;
    PangoLayout *layout;
    int text_width;

    if (!text || !*text)
        return;
    fd = pango_font_description_from_string(font);
    layout = pango_cairo_create_layout(cr);
    pango_layout_set_font_description(layout, fd);
    pango_layout_set_text(layout, text, -1);
    pango_layout_get_extents(layout, NULL, NULL);
    text_width = 0;
    {
        PangoRectangle logical;
        pango_layout_get_extents(layout, NULL, &logical);
        text_width = logical.width / PANGO_SCALE;
    }
    /* Текст, уехавший за правый край окна, читать нельзя: прижимаем
     * к краю, а не обрезаем на середине глифа. */
    if (max_width > 0 && x + text_width > max_width)
        x = MAX(0, max_width - text_width);
    sen_set_color(cr, color);
    cairo_move_to(cr, x, baseline);
    pango_cairo_show_layout(cr, layout);
    g_object_unref(layout);
    pango_font_description_free(fd);
}

/* ------------------------------------------------------------- скругление */

static void sen_rounded_path(cairo_t *cr, int width, int height, int radius)
{
    const double inset = 1.0;
    double w = width - 2 * inset, h = height - 2 * inset;
    double r = sensor_corner_radius_value(radius);

    if (w <= 0 || h <= 0) {
        cairo_rectangle(cr, 0, 0, width, height);
        return;
    }
    if (!sensor_corner_radius_is_rounded(r)) {
        cairo_rectangle(cr, inset, inset, w, h);
        return;
    }
    r = MIN(r, MIN(w, h) / 2.0);
    cairo_new_sub_path(cr);
    cairo_arc(cr, inset + w - r, inset + r, r, -G_PI / 2.0, 0.0);
    cairo_arc(cr, inset + w - r, inset + h - r, r, 0.0, G_PI / 2.0);
    cairo_arc(cr, inset + r, inset + h - r, r, G_PI / 2.0, G_PI);
    cairo_arc(cr, inset + r, inset + r, r, G_PI, 1.5 * G_PI);
    cairo_close_path(cr);
}

static void sen_apply_shape(XsPlugin *p, int w, int h)
{
    SenPriv *priv = p ? p->priv : NULL;
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
    region = sensor_rounded_region(w, h, priv->corner_radius);
    gdk_window_shape_combine_region(window, region, 0, 0);
    if (region)
        cairo_region_destroy(region);
    priv->shape_radius = priv->corner_radius;
    priv->shape_w = w;
    priv->shape_h = h;
}

/* --------------------------------------------------------------- чтение */

/* Перечитать все показанные строки. Значение каждой ищется по паре
 * «чип/канал» — индекс в списке sysfs не годится, он меняется при
 * перезагрузке. */
/* Прочитать значения всех показанных строк.
 *
 * Поиск идёт по УСТОЙЧИВОМУ идентификатору строки («чип/устройство/канал»),
 * а не по номеру hwmon и не по одному имени чипа. Обе причины важны:
 *
 *  - номер hwmonN меняется между загрузками: тот же диск после
 *    перезагрузки может стать hwmon10 вместо hwmon8, и строка конфига
 *    молча переехала бы на другой диск;
 *  - имя чипа НЕ уникально: все четыре nvme называются «nvme», все 13
 *    дисков — «drivetemp». Поиск по имени схлопывал бы их в один.
 *
 * Показывается и канал с ошибкой чтения: значение неизвестно, но строка
 * остаётся на месте с прочерком. Иначе сбой одного чтения на общем с
 * ipmi i2c-адаптере убирал бы строку на кадр, и она мигала бы. */
static void sen_read_values(SenPriv *priv)
{
    SensorList *list;
    guint i;

    if (!priv->values)
        priv->values = g_ptr_array_new_with_free_func(g_free);
    for (i = 0; i < priv->values->len; i++)
        sensor_array_replace(priv->values, i, NULL);
    while (priv->values->len < priv->rows->len)
        g_ptr_array_add(priv->values, NULL);

    list = sensor_list_read("/sys/class/hwmon");
    if (!list) {
        /* Каталог sysfs недоступен целиком: показываем прочерки, а не
         * пустоту — иначе апплет выглядит сломанным, хотя это просто
         * не удалось прочитать. */
        for (i = 0; i < priv->rows->len; i++)
            sensor_array_replace(priv->values, i, g_strdup("—"));
        priv->values_valid = TRUE;
        return;
    }
    for (i = 0; i < priv->rows->len; i++) {
        const char *row_id = g_ptr_array_index(priv->row_sources, i);
        SensorReading *r;
        char *text = NULL;

        /* Тестовая строка: значение фиксированное и НЕ читается с
         * датчиков. Отдельная ветка обязательна — иначе источник
         * «dummy» не нашёлся бы ни в sysfs, ни в NVML, и пользователь
         * увидел бы прочерк вместо проверки, что вывод работает. */
        if (g_strcmp0(row_id, SEN_DUMMY_SOURCE) == 0) {
            sensor_array_replace(priv->values, i,
                           sensor_format_value(SEN_DUMMY_CELSIUS,
                                               priv->fahrenheit, TRUE));
            continue;
        }

        /* NVIDIA не отдаёт hwmon-узел, её каналы лежат в NVML. Сначала
         * пробуем sysfs, и только если источник помечен как nvidia/ —
         * идём в NVML: иначе строка с карты вечно давала бы прочерк. */
        r = list ? sensor_find_reading(list, row_id) : NULL;
        if (!r && nvml_source_is_nvidia(row_id)) {
            gdouble c = 0.0;

            if (nvml_read_value(row_id, &c))
                text = sensor_format_value(c, priv->fahrenheit, TRUE);
            else
                text = g_strdup("—");
        }
        if (r) {
            if (r->read_error)
                text = g_strdup("—");     /* канал есть, значение неизвестно */
            else if (r->valid)
                text = sensor_format_value(r->celsius, priv->fahrenheit, TRUE);
            else
                text = g_strdup("—");     /* канал не подключён (-128) */
        }
        if (text)
            sensor_array_replace(priv->values, i, text);
    }
    sensor_list_free(list);
    priv->values_valid = TRUE;
}

/* ------------------------------------------------------------ отрисовка */

static cairo_surface_t *sen_render(SenPriv *priv, int width, int height)
{
    cairo_surface_t *surface;
    cairo_t *cr;
    const char *label_font, *value_font;
    int step, y, shown = 0;
    int label_x, value_x;

    surface = cairo_image_surface_create(CAIRO_FORMAT_ARGB32, width, height);
    cr = cairo_create(surface);

    /* Фон: сплошной цвет с его альфой, затем рамка по контуру окна. */
    cairo_set_operator(cr, CAIRO_OPERATOR_SOURCE);
    cairo_set_source_rgba(cr, priv->background_color[0],
                          priv->background_color[1], priv->background_color[2],
                          priv->background_color[3]);
    cairo_paint(cr);
    cairo_set_operator(cr, CAIRO_OPERATOR_OVER);

    cairo_set_line_width(cr, 1.0);
    sen_set_color(cr, priv->border_color);
    sen_rounded_path(cr, width, height, priv->corner_radius);
    cairo_stroke(cr);

    /* Клип по контуру окна: глиф у скруглённого угла просто не рисуется,
     * без ручных границ на каждую строку. */
    cairo_save(cr);
    sen_rounded_path(cr, width, height, priv->corner_radius);
    cairo_clip(cr);

    label_font = sen_scale_font(priv->label_font, priv->design_width, width);
    value_font = sen_scale_font(priv->value_font, priv->design_width, width);
    step = sen_scale(priv->line_step, priv->design_height, height,
                     height - 2 * SEN_MARGIN);
    if (step < 1)
        step = 1;
    /* Шаг строк не может быть меньше высоты текста, иначе строки
     * наезжают друг на друга. Пользовательский line_step задаёт шаг при
     * МЕНЬШЕЙ высоте шрифта, а увеличение шрифта в Настройках об этом
     * не говорит: при line_step=12 и шрифте Sans 10 высота строки 13,
     * и вторая строка ложилась на первую. Поэтому шаг расширяется
     * снизу, если текст выше. */
    step = sen_effective_step(cr, step, label_font, value_font);
    label_x = sen_scale(priv->label_x, priv->design_width, width, width - 1);
    value_x = sen_scale(priv->value_x, priv->design_width, width, width - 1);
    y = sen_scale(priv->first_row_y, priv->design_height, height, height - 1);

    for (guint i = 0; i < priv->rows->len; i++) {
        const char *label = g_ptr_array_index(priv->rows, i);
        const char *value = priv->values_valid
                                ? g_ptr_array_index(priv->values, i) : NULL;
        int row_height;

        if (shown >= SEN_MAX_ROWS)
            break;
        if (y > height || y < 0)
            break;  /* список не влез — дальше идти незачем */
        row_height = sen_row_height(cr, label_font);
        if (row_height > step)
            row_height = step;

        if (value && *value) {
            /* Подпись и число — два независимых элемента, как в
             * network_monitor: позиция числа задана своим ключом и не
             * зависит от длины подписи. Иначе «Position ничего не
             * делает» и «Label position двигает всю строку». */
            /* По умолчанию число выровнено по ПРАВОМУ краю окна: подпись
             * прижата влево, а колонка чисел читается взглядом независимо
             * от длины подписей. При value_align=left число стартует от
             * value_x, как в прежних сборках. */
            if (priv->value_align_right) {
                int w = sen_text_width(cr, value, value_font);
                int value_left = width - SEN_MARGIN - w;

                /* Подпись ограничена тем же местом, где начинается
                 * число. Без этого длинная подпись (а у нас есть
                 * «coretemp-isa-0000 · Package id 0») уезжала вправо и
                 * ложилась прямо на значение — обе строки становились
                 * нечитаемыми. */
                sen_show_text(cr, label_font, label_x, y, label,
                              priv->label_color, value_left);
                sen_show_text(cr, value_font, value_left, y,
                              value, priv->value_color, 0);
            } else {
                sen_show_text(cr, label_font, label_x, y, label,
                              priv->label_color, 0);
                sen_show_text(cr, value_font, value_x, y, value,
                              priv->value_color, width - SEN_MARGIN);
            }
        } else if (label && *label) {
            sen_show_text(cr, label_font, label_x, y, label,
                          priv->label_color, 0);
        }
        y += step;
        shown++;
    }
    cairo_restore(cr);
    cairo_destroy(cr);
    cairo_surface_mark_dirty(surface);
    return surface;
}

/* ------------------------------------------------------------- callbacks */

static void sen_rebuild_cache(SenPriv *priv, int width, int height)
{
    if (priv->cache)
        cairo_surface_destroy(priv->cache);
    priv->cache = sen_render(priv, width, height);
    priv->cache_width = width;
    priv->cache_height = height;
}

/* Сериализация строк в «подпись|источник». Определение стоит рядом с
 * sen_save(), но вызывается и из init — прототип нужен здесь. */
static char *sen_rows_to_config(const SenPriv *priv);

/* Определены ниже, а вызываются из init: и очистка заглушки, и поиск
 * строки по источнику нужны раньше определения. */
static guint sen_row_index(SenPriv *priv, const char *source);
static void sen_drop_dummy_row(SenPriv *priv);

static int sen_init(XsPlugin *p, GKeyFile *kf)
{
    SenPriv *priv = g_new0(SenPriv, 1);
    const char *rows_text;
    int x, y;

    p->priv = priv;
    priv->plugin = p;
    priv->kf = kf;
    /* Таблица инстансов: контекст диалога переживает properties(), и его
     * обработчики должны находить ЖИВОЙ priv по имени секции. Без таблицы
     * обработчик читает освобождённую память — ровно тот UAF, который
     * однажды уронил демон при прокрутке диалога колесом. */
    if (!sen_instances)
        sen_instances = g_hash_table_new_full(g_str_hash, g_str_equal,
                                              g_free, NULL);
    g_hash_table_replace(sen_instances, g_strdup(p->name), priv);
    priv->width = CLAMP(xs_host_api()->conf_int(kf, p->name, "window_width",
                                              SEN_DEFAULT_WIDTH),
                        SEN_MIN_WIDTH, 1600);
    priv->height = CLAMP(xs_host_api()->conf_int(kf, p->name, "window_height",
                                               SEN_DEFAULT_HEIGHT),
                         SEN_MIN_HEIGHT, 1200);
    /* Авто-высота по числу строк. Ключ window_height НЕ годится как
     * признак: плагин сам записывает его в конфиг при старте («чтобы
     * конфиг документировал себя»), и на следующем запуске авто-режим
     * выключался навсегда — окно переставало расти, и каждая добавленная
     * строка обрезалась снизу. Признак хранится отдельным ключом, и его
     * пишет только плагин. */
    priv->height_auto = xs_host_api()->conf_int(kf, p->name, "height_auto",
                                                -1) < 0;
    priv->width_auto = xs_host_api()->conf_int(kf, p->name, "width_auto",
                                               -1) < 0;
    priv->corner_radius = CLAMP(xs_host_api()->conf_int(kf, p->name,
                                                        "corner_radius",
                                                        SEN_DEFAULT_RADIUS),
                                0, SEN_MAX_RADIUS);
    priv->update_ms = CLAMP(xs_host_api()->conf_int(kf, p->name, "update_ms",
                                                   5000),
                            200, 60000);
    priv->opacity = xs_host_api()->conf_dbl(kf, p->name, "opacity", 1.0);
    /* Единицы: выбор строкой, а не флажком. Старый ключ fahrenheit=0|1
     * ещё читается, но новым конфигам он не пишется: «1» и «true» в нём
     * означали разное, и это молча ломало перенос конфига. */
    {
        const char *units = xs_host_api()->conf_str(kf, p->name, "units",
                                                    NULL);
        if (units && g_ascii_strcasecmp(units, "celsius") == 0)
            priv->fahrenheit = FALSE;
        else if (units && g_ascii_strcasecmp(units, "fahrenheit") == 0)
            priv->fahrenheit = TRUE;
        else if (units && *units)
            /* Неизвестное значение молча включало Фаренгейт: ветка
             * «units есть -> TRUE» ловила и опечатку, и «kelvin». */
            priv->fahrenheit = FALSE;
        else
            priv->fahrenheit = xs_host_api()->conf_int(kf, p->name,
                                                       "fahrenheit", 1) != 0;
    }

    priv->label_font = xs_host_api()->conf_str(kf, p->name, "label_font",
                                               "Sans 8");
    priv->value_font = xs_host_api()->conf_str(kf, p->name, "value_font",
                                               "Sans 8");

    priv->label_x = xs_host_api()->conf_int(kf, p->name, "label_x", SEN_MARGIN);
    /* Позиция числа по умолчанию — за самой длинной подписью.
     *
     * Фиксированное 110 рассчитано на «coretemp Core 0», а подпись в
     * стиле sensors («nvme-pci-0300 · Sensor 1») шире, и число ложилось
     * прямо на неё. Ровно тот же дефект, что был с итоговой строкой в
     * network_monitor: дефолт обязан считаться по содержимому, а не
     * быть зашитым числом. Пользовательский value_x из конфига
     * уважается — правится только дефолт. */
    priv->value_x = xs_host_api()->conf_int(kf, p->name, "value_x", -1);

    /* Выравнивание значения строковым ключом, а не флагом: это режим
     * отображения, и со временем появится третий вариант (по центру),
     * который флагом не выразить. Дефолт — вправо. */
    {
        const char *al = xs_host_api()->conf_str(kf, p->name,
                                                 "value_align", "right");

        priv->value_align_right = !al || g_ascii_strcasecmp(al, "left") != 0;
    }
    priv->first_row_y = xs_host_api()->conf_int(kf, p->name, "first_row_y", 4);
    priv->line_step = xs_host_api()->conf_int(kf, p->name, "line_step", 12);

    /* Строки: «метка;чип/канал;…». Читаются из одного ключа, потому что
     * метка не уникальна — четыре nvme дают четыре «Composite», и по
     * одной метке найти канал нельзя. */
    rows_text = xs_host_api()->conf_str(kf, p->name, "rows", "");
    priv->rows = g_ptr_array_new_with_free_func(g_free);
    priv->row_sources = g_ptr_array_new_with_free_func(g_free);
    if (rows_text && *rows_text) {
        GPtrArray *raw = sensor_config_list(rows_text);
        gboolean orphan = FALSE;   /* нашлась строка без источника */

        for (guint i = 0; i < raw->len; i++) {
            const char *entry = g_ptr_array_index(raw, i);
            const char *semi = strchr(entry, '|');

            if (!semi || semi == entry || !semi[1]) {
                g_ptr_array_add(priv->rows, g_strdup(entry));
                g_ptr_array_add(priv->row_sources, g_strdup(""));
                orphan = TRUE;
                continue;
            }
            g_ptr_array_add(priv->rows, g_strndup(entry, semi - entry));
            g_ptr_array_add(priv->row_sources, g_strdup(semi + 1));
        }
        g_ptr_array_unref(raw);

        /* Миграция: строка без источника не находит значение и рисует одну
         * подпись без числа. Источник восстанавливается по подписи среди
         * найденных сенсоров: «nvme-pci-0300 · Composite» указывает на
         * канал однозначно. Такой конфиг остался от прежнего обработчика
         * галочек, который писал одни подписи, — восстановить один раз при
         * старте дешевле, чем годами показывать строки без чисел. */
        /* Миграция нужна и для строк с НЕПРАВИЛЬНЫМ источником, не
         * только с пустым: прежняя сборка писала в источник читаемое имя
         * («nct7904-i2c · temp3»), и такая строка тоже не находила
         * сенсор. Ищем битые заранее. */
        if (orphan || sen_config_has_broken_sources(priv)) {
            SensorList *found = sensor_list_read("/sys/class/hwmon");
            guint fixed = 0;

            if (found) {
                for (guint i = 0; i < priv->rows->len; i++) {
                    const char *cur = g_ptr_array_index(priv->row_sources, i);
                    const char *label = g_ptr_array_index(priv->rows, i);
                    gboolean done = FALSE;

                    /* Пропускаем только тот источник, который реально
                     * находит сенсор. Битый (в нём читаемое имя вместо
                     * ключа) чиним так же, как пустой. */
                    if (cur && *cur && !sen_source_is_broken(priv, found, cur))
                        continue;
                    for (guint k = 0; k < found->chips->len && !done; k++) {
                        SensorChip *c =
                            g_ptr_array_index(found->chips, k);
                        char *sname = sensor_chip_sensors_name(c);

                        for (guint m = 0; m < c->readings->len; m++) {
                            SensorReading *r =
                                g_ptr_array_index(c->readings, m);
                            char *want = g_strdup_printf("%s · %s", sname,
                                                         r->label);
                            gboolean hit;

                            /* Сначала имя в стиле sensors, затем старый
                             * формат «чип + канал» из ранних сборок.
                             * Второе совпадение неоднозначно — у coretemp
                             * два устройства с каналом «Core 1», — поэтому
                             * берём первое и предупреждаем в лог. */
                            if (g_strcmp0(want, label) == 0) {
                                hit = TRUE;
                            } else if (sen_label_is_group_form(label, c,
                                                                   r->label)) {
                                /* Подпись из диалога собирается из ИМЕНИ
                                 * ГРУППЫ («nct7904-i2c · temp3»), а не из
                                 * полного имени sensors(1)
                                 * («nct7904-i2c-1-2d · temp3»). Обе формы
                                 * встречаются в конфигах. */
                                hit = TRUE;
                            } else {
                                char *legacy = g_strdup_printf("%s %s",
                                                               c->chip,
                                                               r->label);
                                hit = g_strcmp0(legacy, label) == 0;
                                if (hit)
                                    p->host->log("sensors: подпись «%s» "
                                                 "неоднозначна, взят первый "
                                                 "совпавший канал", label);
                                g_free(legacy);
                            }
                            g_free(want);
                            if (!hit)
                                continue;
                            g_free(g_ptr_array_index(priv->row_sources, i));
                            g_ptr_array_index(priv->row_sources, i) =
                                sen_source_key(c->chip, c->device, r->label);
                            fixed++;
                            done = TRUE;
                            break;
                        }
                        g_free(sname);
                    }
                }
                sensor_list_free(found);
            }
            if (fixed)
                p->host->log("sensors: восстановлено источников: %d", fixed);
        }

        /* Строка, сенсор которой исчез совсем, выводиться не может.
         *
         * Каналы у вентиляторов nct7904 имели temp6_type=6, и раньше они
         * попадали в апплет как «0.0°C». Теперь core их не отдаёт, и такая
         * строка осталась бы висеть с прочерком навсегда — без причины и
         * без возможности починить. Удаляем, логируем, чтобы был след в
         * конфиге. */
        {
            SensorList *now = sensor_list_read("/sys/class/hwmon");
            guint dropped = 0;

            if (now) {
                for (guint i = 0; i < priv->row_sources->len; i++) {
                    const char *src = g_ptr_array_index(priv->row_sources,
                                                        i);

                    if (src && *src && (g_strcmp0(src, SEN_DUMMY_SOURCE) == 0
                                        || sensor_find_reading(now, src)
                                        || nvml_source_is_nvidia(src)))
                        continue;   /* строка жива (hwmon, NVML или dummy) */
                    g_ptr_array_remove_index(priv->rows, i);
                    g_ptr_array_remove_index(priv->row_sources, i);
                    i--;   /* remove_index сдвинул хвост на себя */
                    dropped++;
                }
                sensor_list_free(now);
            }
            if (dropped)
                p->host->log("sensors: удалено строк с исчезнувшим "
                             "сенсором: %u", dropped);
        }
    }

    priv->values = g_ptr_array_new_with_free_func(g_free);

    /* Цвета. ЧИТАЕМЫЕ ЗДЕСЬ КЛЮЧИ ОБЯЗАНЫ СОВПАДАТЬ с теми, что пишет
     * обработчик: расхождение ключей — самый частый дефект этого класса,
     * и он невидим: файл меняется, а на экране ничего. */
    {
        static const gdouble label_def[4] = { 0.12, 1.0, 1.0, 1.0 };
        static const gdouble value_def[4] = { 1.0, 1.0, 1.0, 1.0 };
        static const gdouble bg_def[4]    = { 0.098, 0.098, 0.098, 0.29 };
        static const gdouble border_def[4] = { 0.451, 0.451, 0.451, 1.0 };
        const char *t;

        t = xs_host_api()->conf_str(kf, p->name, "label_color", NULL);
        if (t && sensor_parse_rgba(t, priv->label_color))
            ;
        else
            memcpy(priv->label_color, label_def, sizeof(label_def));
        t = xs_host_api()->conf_str(kf, p->name, "value_color", NULL);
        if (t && sensor_parse_rgba(t, priv->value_color))
            ;
        else
            memcpy(priv->value_color, value_def, sizeof(value_def));
        t = xs_host_api()->conf_str(kf, p->name, "background_color", NULL);
        if (t && sensor_parse_rgba(t, priv->background_color))
            ;
        else
            memcpy(priv->background_color, bg_def, sizeof(bg_def));
        t = xs_host_api()->conf_str(kf, p->name, "border_color", NULL);
        if (t && sensor_parse_rgba(t, priv->border_color))
            ;
        else
            memcpy(priv->border_color, border_def, sizeof(border_def));
    }

    /* Смесь «тестовая строка + реальные сенсоры» в конфиге — след
     * прежних сборок, где заглушка не убиралась при включении
     * сенсора. Убираем: она не должна стоять рядом с настоящими
     * данными. */
    if (priv->rows->len > 1 && sen_row_index(priv, SEN_DUMMY_SOURCE)
                                  != SEN_ROW_NONE)
        sen_drop_dummy_row(priv);

    /* Дефолт: НИ ОДНОГО реального сенсора.
     *
     * Апплет не должен угадывать за пользователя, что показывать. Если
     * строки не выбраны, показываем одну тестовую строку «dummy» с
     * фиксированным значением: она доказывает, что плагин жив, окно
     * нарисовано и текст виден, не выдавая себя за реальные данные.
     * Прежнее поведение (первые 4 найденных канала) было хуже пустого:
     * пользователь получал в выводе четыре сенсора, которых он не
     * выбирал, и не мог отличить их от намеренно выбранных. */
    if (priv->rows->len == 0) {
        g_ptr_array_add(priv->rows, g_strdup(SEN_DUMMY_LABEL));
        g_ptr_array_add(priv->row_sources, g_strdup(SEN_DUMMY_SOURCE));
        g_ptr_array_add(priv->values, NULL);
    }

    /* Значения читаем ДО расчёта геометрии: авто-ширина меряет самое
     * длинное число (SEN_VALUE_GAP между подписью и числом), и без
     * них окно выходило бы по одним подписям, а числа уезжали бы за
     * край. */
    sen_read_values(priv);

    /* Дефолт позиции числа — за самой длинной подписью.
     *
     * Считается ПОСЛЕ формирования списка строк, и это не стилистика:
     * пока строк нет, цикл выше измеряет пустоту, widest остаётся 0, и
     * value_x схлопывается на label_x. На пустом дефолте число ложилось
     * прямо на подпись. С дефолтом из четырёх сенсоров расчёт попадал
     * на заполненный список только потому, что те добавлялись раньше —
     * работало случайно. */
    if (priv->value_x < 0) {
        cairo_surface_t *probe =
            cairo_image_surface_create(CAIRO_FORMAT_ARGB32, 1, 1);
        cairo_t *pcr = cairo_create(probe);
        const char *font = priv->label_font;
        int widest = 0;

        for (guint i = 0; i < priv->rows->len; i++) {
            int w = sen_text_width(pcr, g_ptr_array_index(priv->rows, i),
                                   font);
            if (w > widest)
                widest = w;
        }
        cairo_destroy(pcr);
        cairo_surface_destroy(probe);
        priv->value_x = priv->label_x + widest + SEN_VALUE_GAP;
        /* Не даём колонке чисел уехать за окно: сжатие делает sen_show_text,
         * но лучше сдвинуть колонку целиком. */
        if (priv->value_x > priv->width - SEN_MARGIN - 40)
            priv->value_x = MAX(priv->label_x + widest + SEN_VALUE_GAP,
                                priv->width - 60);
    }


    /* Высота по числу строк, если пользователь её не задавал. Длина
     * строки задаётся line_step, а не размером окна: 12 строк в окне
     * 100px обрезаются по нижнему краю, и последняя температура не
     * видна вообще. Пользовательская высота уважается. */
    if (priv->height_auto) {
        cairo_surface_t *probe =
            cairo_image_surface_create(CAIRO_FORMAT_ARGB32, 1, 1);
        cairo_t *pcr = cairo_create(probe);
        int step = sen_effective_step(pcr, priv->line_step,
                                      priv->label_font, priv->value_font);
        int need;

        cairo_destroy(pcr);
        cairo_surface_destroy(probe);
        /* +1 на последнюю строку: базовая линия последней строки стоит
         * на first_row_y + (n-1)*step, а под ней ещё descender. */
        need = priv->first_row_y + (int) priv->rows->len * step
             + SEN_MARGIN * 2;

        /* Растёт И уменьшается: снял строки — окно должно сжаться, иначе
         * пользователь получит пустое поле внизу, решив, что это норма. */
        priv->height = CLAMP(need, SEN_MIN_HEIGHT, 1200);
    }

    /* Ширина по самым длинным подписи и числу — по тому же принципу,
     * что высота по строкам. Фиксированные 200px не годились, когда
     * подписи длиннее: «coretemp-isa-0000 · Package id 0» занимала
     * почти всю ширину, и число ложилось прямо на неё. Обе колонки
     * становятся нечитаемыми, а дефект выглядит как «шрифт едет».
     *
     * Считается ПОСЛЕ того, как известен состав строк, и меряется тем
     * же шрифтом, которым будет нарисовано, иначе масштаб уехал бы.
     * Пользовательская ширина уважается: width_auto снимается, как
     * только пользователь выставил размер вручную. */
    if (priv->width_auto) {
        cairo_surface_t *probe =
            cairo_image_surface_create(CAIRO_FORMAT_ARGB32, 1, 1);
        cairo_t *pcr = cairo_create(probe);
        int widest = 0, need_w;

        for (guint i = 0; i < priv->rows->len; i++) {
            const char *src = g_ptr_array_index(priv->row_sources, i);
            int lw = sen_text_width(pcr, g_ptr_array_index(priv->rows, i),
                                    priv->label_font);
            const char *val = priv->values_valid && i < priv->values->len
                                ? g_ptr_array_index(priv->values, i) : NULL;
            int vw = val ? sen_text_width(pcr, val, priv->value_font) : 0;
            int w;

            if (lw > widest)
                widest = lw;
            /* Зазор нужен даже без числа — иначе «Чтото» и «36.6°C»
             * встали бы впритык к краю окна. */
            w = lw + SEN_VALUE_GAP + vw;
            if (w > widest)
                widest = w;
            (void) src;
        }
        need_w = priv->label_x + widest + SEN_MARGIN * 2;
        cairo_destroy(pcr);
        cairo_surface_destroy(probe);
        priv->width = CLAMP(need_w, 120, 1200);
    }

    priv->design_width = priv->width;
    priv->design_height = priv->height;

    /* Пишем дефолты, чтобы конфиг сам документировал себя.
     *
     * В конфиг пишется ПАРА «подпись|источник», а не одни подписи: init
     * разбирает именно пару, и запись одних подписей теряла привязку к
     * устройству. Строка переживала бы перезагрузку только до первого
     * же сохранения — а оно происходит в init.
     *
     * Если ни один сенсор не выбран, ключ rows удаляется, а не пишется
     * пустым: пустая строка означала бы «выбрана строка без подписи»,
     * и следующий старт прогнал бы по ней миграцию, превратив дефолт
     * в набор несуществующих сенсоров. Отсутствие ключа — это и есть
     * «ничего не выбрано». */
    {
        char *rows_text = sen_rows_to_config(priv);

        if (rows_text)
            g_key_file_set_string(kf, p->name, "rows", rows_text);
        else
            g_key_file_remove_key(kf, p->name, "rows", NULL);
        g_free(rows_text);
    }
    g_key_file_set_integer(kf, p->name, "window_width", priv->width);
    g_key_file_set_integer(kf, p->name, "window_height", priv->height);
    /* Признак авто-высоты: -1 — считать по числу строк, любое другое
     * значение — пользователь задал высоту руками, её уважаем. */
    g_key_file_set_integer(kf, p->name, "height_auto",
                           priv->height_auto ? -1 : priv->height);
    /* Признак авто-ширины: -1 — считать по подписям, любое другое
     * значение — пользователь задал ширину руками, её уважаем. */
    g_key_file_set_integer(kf, p->name, "width_auto",
                           priv->width_auto ? -1 : priv->width);
    g_key_file_set_integer(kf, p->name, "corner_radius",
                           priv->corner_radius);
    g_key_file_set_integer(kf, p->name, "update_ms", priv->update_ms);
    g_key_file_set_string(kf, p->name, "units",
                          priv->fahrenheit ? "fahrenheit" : "celsius");
    g_key_file_set_string(kf, p->name, "label_font", priv->label_font);
    g_key_file_set_string(kf, p->name, "value_font", priv->value_font);
    g_key_file_set_integer(kf, p->name, "label_x", priv->label_x);
    g_key_file_set_integer(kf, p->name, "value_x", priv->value_x);
    g_key_file_set_string(kf, p->name, "value_align",
                          priv->value_align_right ? "right" : "left");
    g_key_file_set_integer(kf, p->name, "first_row_y", priv->first_row_y);
    g_key_file_set_integer(kf, p->name, "line_step", priv->line_step);
    xs_core_plugin_conf_flush(p->name);

    x = xs_host_api()->conf_int(kf, p->name, "x", 80);
    y = xs_host_api()->conf_int(kf, p->name, "y", 80);
    p->win = xs_host_api()->make_window(p, x, y, priv->width, priv->height);
    if (!p->win) {
        p->host->log("sensors: failed to create window");
        return -1;
    }
    sen_rebuild_cache(priv, priv->width, priv->height);
    xs_host_api()->set_opacity(p, CLAMP(priv->opacity, 0.1, 1.0));
    xs_host_api()->set_tick(p, priv->update_ms);
    return 0;
}

static guint sen_tick(XsPlugin *p)
{
    SenPriv *priv = p ? p->priv : NULL;

    if (!priv || !p->win)
        return 0;
    sen_read_values(priv);
    if (priv->cache_width != priv->width || priv->cache_height != priv->height)
        sen_rebuild_cache(priv, priv->width, priv->height);
    else {
        cairo_surface_t *fresh = sen_render(priv, priv->cache_width,
                                            priv->cache_height);
        cairo_surface_destroy(priv->cache);
        priv->cache = fresh;
    }
    xs_host_api()->invalidate(p);
    gtk_widget_queue_draw(p->win);
    return priv->update_ms;
}

static void sen_draw(XsPlugin *p, cairo_t *cr, int w, int h)
{
    SenPriv *priv = p ? p->priv : NULL;

    if (!priv || !priv->cache)
        return;
    if (priv->cache_width != w || priv->cache_height != h)
        sen_rebuild_cache(priv, w, h);
    sen_apply_shape(p, w, h);
    cairo_set_operator(cr, CAIRO_OPERATOR_SOURCE);
    cairo_set_source_surface(cr, priv->cache, 0, 0);
    cairo_paint(cr);
    cairo_set_operator(cr, CAIRO_OPERATOR_OVER);
}

static void sen_shutdown(XsPlugin *p)
{
    SenPriv *priv = p ? p->priv : NULL;

    if (!priv)
        return;
    /* Запись из таблицы инстансов УБИРАЕТСЯ: контролы диалога переживают
     * properties() и держат контекст с именем инстанса. Если запись
     * останется, обработчик после пересоздания апплета найдёт по имени
     * уже освобождённый priv — то же чтение из freed memory, что однажды
     * уронило демон при прокрутке диалога колесом. */
    if (sen_instances)
        g_hash_table_remove(sen_instances, p->name);
    if (priv->cache)
        cairo_surface_destroy(priv->cache);
    if (priv->rows)
        g_ptr_array_unref(priv->rows);
    if (priv->row_sources)
        g_ptr_array_unref(priv->row_sources);
    if (priv->values)
        g_ptr_array_unref(priv->values);
    g_free(priv->label_font);
    g_free(priv->value_font);
    g_free(priv);
    p->priv = NULL;
}

/* ------------------------------------------------------------- свойства */

/* Живой priv по имени инстанса. Таблица имён нужна, потому что контекст
 * диалога переживает properties(): контролы живут дольше функции. */
static GHashTable *sen_instances;

static SenPriv *sen_live_priv(SenDialogContext *ctx)
{
    if (!ctx || !sen_instances)
        return NULL;
    return g_hash_table_lookup(sen_instances, ctx->instance_name);
}

/* Сериализация строк в конфиг: «подпись|источник», через «;».
 *
 * ОДНА функция на запись и на init. Дублировать разбор «подпись|источник»
 * в двух местах означало две расходящиеся версии: init писал пары, а
 * обработчик галочек — одни подписи, и первое же движение галочки стирало
 * привязку к устройству. Строка переживала бы перезагрузку только до
 * первого клика в Properties. */
/* Сериализация активных сенсоров в конфиг.
 *
 * В конфиг попадают ТОЛЬКО включённые сенсоры. Отключённый сенсор не
 * хранится вовсе — ни как выключенная строка, ни как «группа|канал» без
 * галочки. Иначе конфиг разрастался бы мусором от всех когда-либо
 * включённых датчиков, и файл перестал бы отвечать на вопрос «что сейчас
 * выводится».
 *
 * Отдельно отсекается тестовая строка dummy: это не сенсор, а маркер
 * работы плагина, и в списке выбранного ему не место. Её наличие
 * определяется тем, что других строк нет.
 *
 * Пустой результат — это НЕ пустой ключ, а отсутствие ключа rows.
 * Пустая строка означала бы «выбрана строка без подписи», и init
 * чинил бы её миграцией, превращая нормальный дефолт в набор
 * несуществующих сенсоров. */
static char *sen_rows_to_config(const SenPriv *priv)
{
    GPtrArray *labels = g_ptr_array_new_with_free_func(g_free);
    GPtrArray *sources = g_ptr_array_new_with_free_func(g_free);
    char *text;

    for (guint i = 0; i < priv->rows->len; i++) {
        const char *label = g_ptr_array_index(priv->rows, i);
        const char *src = g_ptr_array_index(priv->row_sources, i);

        if (!src || !*src || g_strcmp0(src, SEN_DUMMY_SOURCE) == 0)
            continue;   /* не сенсор: в конфиг не попадает */
        g_ptr_array_add(labels, g_strdup(label ? label : ""));
        g_ptr_array_add(sources, g_strdup(src));
    }
    if (labels->len == 0) {
        g_ptr_array_unref(labels);
        g_ptr_array_unref(sources);
        return NULL;   /* выбора нет — ключ будет удалён */
    }
    text = sensor_config_join_pair(labels, sources);
    g_ptr_array_unref(labels);
    g_ptr_array_unref(sources);
    return text;
}

static void sen_save(SenPriv *priv)
{
    char *text = sen_rows_to_config(priv);

    if (text)
        g_key_file_set_string(priv->kf, priv->plugin->name, "rows", text);
    else
        g_key_file_remove_key(priv->kf, priv->plugin->name, "rows", NULL);
    g_free(text);
    xs_core_plugin_conf_flush(priv->plugin->name);
}

/* Галка для строки настроек. sen_row кладёт виджет в правую колонку, а
 * заголовок ставит слева, поэтому виджет должен быть без собственного
 * текста — иначе подпись уехала бы в третью колонку. */
static GtkWidget *sen_check(const char *key, gboolean active)
{
    GtkWidget *cb = gtk_check_button_new();

    gtk_toggle_button_set_active(GTK_TOGGLE_BUTTON(cb), active);
    g_object_set_data(G_OBJECT(cb), "xs-key", (gpointer) key);
    return cb;
}

/* Обработчик галки выравнивания. Отдельный от sen_int_changed: ключ
 * приходит виджетом, а поле занимает gboolean, а не int. Сохранение —
 * строкой, ровно как читает init. */
static void sen_bool_changed(GtkToggleButton *cb, gpointer data)
{
    SenDialogContext *ctx = data;
    SenPriv *priv = sen_live_priv(ctx);
    const char *key;

    if (!priv)
        return;
    key = g_object_get_data(G_OBJECT(cb), "xs-key");
    if (!key || strcmp(key, "value_align_right") != 0)
        return;
    priv->value_align_right = gtk_toggle_button_get_active(cb);
    priv->design_width = priv->width;
    priv->design_height = priv->height;
    sen_rebuild_cache(priv, priv->width, priv->height);
    xs_host_api()->resize(priv->plugin, priv->width, priv->height);
    xs_host_api()->invalidate(priv->plugin);
    if (priv->plugin->win)
        gtk_widget_queue_draw(priv->plugin->win);
    sen_save(priv);
}

static void sen_int_changed(GtkSpinButton *spin, gpointer data)
{
    SenDialogContext *ctx = data;
    SenPriv *priv = sen_live_priv(ctx);
    const char *key;
    int value;

    if (!priv)
        return;
    key = g_object_get_data(G_OBJECT(spin), "xs-key");
    if (!key)
        return;
    value = gtk_spin_button_get_value_as_int(spin);
    if (!strcmp(key, "window_width")) {
        priv->width = value;
        /* Пользователь задал ширину руками — авто-режим выключается.
         * Иначе следующая длинная подпись снова растолкнёт окно, и
         * выставленный размер перестанет держаться. */
        priv->width_auto = FALSE;
    }
    else if (!strcmp(key, "window_height")) {
        priv->height = value;
        /* Пользователь задал высоту руками — авто-режим выключается, иначе
         * следующая добавленная строка снова упрётся в обрезку снизу. */
        priv->height_auto = FALSE;
    }
    else if (!strcmp(key, "corner_radius"))      priv->corner_radius = value;
    else if (!strcmp(key, "update_ms"))          priv->update_ms = value;
    else if (!strcmp(key, "label_x"))            priv->label_x = value;
    else if (!strcmp(key, "value_x"))            priv->value_x = value;
    else if (!strcmp(key, "first_row_y"))        priv->first_row_y = value;
    else if (!strcmp(key, "line_step"))          priv->line_step = value;
    else return;
    g_key_file_set_integer(priv->kf, priv->plugin->name, key, value);
    xs_core_plugin_conf_flush(priv->plugin->name);
    priv->design_width = priv->width;
    priv->design_height = priv->height;
    if (!strcmp(key, "corner_radius"))
        priv->shape_radius = -1;  /* форма пересчитается при invalidate */
    sen_rebuild_cache(priv, priv->width, priv->height);
    xs_host_api()->resize(priv->plugin, priv->width, priv->height);
    xs_host_api()->invalidate(priv->plugin);
    if (priv->plugin->win)
        gtk_widget_queue_draw(priv->plugin->win);
}

static void sen_font_set(GtkFontButton *button, gpointer data)
{
    SenDialogContext *ctx = data;
    SenPriv *priv = sen_live_priv(ctx);
    const char *key, *value;

    if (!priv)
        return;
    key = g_object_get_data(G_OBJECT(button), "xs-key");
    if (!key)
        return;
    value = gtk_font_button_get_font_name(GTK_FONT_BUTTON(button));
    if (!strcmp(key, "label_font")) {
        g_free(priv->label_font);
        priv->label_font = g_strdup(value);
    } else if (!strcmp(key, "value_font")) {
        g_free(priv->value_font);
        priv->value_font = g_strdup(value);
    } else
        return;
    g_key_file_set_string(priv->kf, priv->plugin->name, key, value);
    xs_core_plugin_conf_flush(priv->plugin->name);
    sen_rebuild_cache(priv, priv->width, priv->height);
    xs_host_api()->invalidate(priv->plugin);
    if (priv->plugin->win)
        gtk_widget_queue_draw(priv->plugin->win);
}

static void sen_color_set(GtkColorButton *button, gpointer data)
{
    SenDialogContext *ctx = data;
    SenPriv *priv = sen_live_priv(ctx);
    const char *key;
    GdkRGBA rgba;
    gdouble *target = NULL;
    char *text;

    if (!priv)
        return;
    key = g_object_get_data(G_OBJECT(button), "xs-key");
    if (!key)
        return;
    gtk_color_chooser_get_rgba(GTK_COLOR_CHOOSER(button), &rgba);
    if (!strcmp(key, "label_color"))            target = priv->label_color;
    else if (!strcmp(key, "value_color"))       target = priv->value_color;
    else if (!strcmp(key, "background_color"))  target = priv->background_color;
    else if (!strcmp(key, "border_color"))      target = priv->border_color;
    if (!target)
        return;
    target[0] = rgba.red;
    target[1] = rgba.green;
    target[2] = rgba.blue;
    target[3] = rgba.alpha;
    text = sensor_format_rgba(priv->value_color); /* заполнено выше */
    g_key_file_set_string(priv->kf, priv->plugin->name, key, text);
    g_free(text);
    xs_core_plugin_conf_flush(priv->plugin->name);
    sen_rebuild_cache(priv, priv->width, priv->height);
    xs_host_api()->invalidate(priv->plugin);
    if (priv->plugin->win)
        gtk_widget_queue_draw(priv->plugin->win);
}

/* Единицы измерения. GtkComboBox шлёт «changed», а не «toggled» и не
 * «value-changed», поэтому у комбо свой обработчик: без него выбор
 * сохранялся в файл, но картинка не менялась. */
static void sen_units_changed(GtkComboBox *combo, gpointer data)
{
    SenDialogContext *ctx = data;
    SenPriv *priv = sen_live_priv(ctx);
    gboolean fah;

    if (!priv)
        return;
    if (!g_object_get_data(G_OBJECT(combo), "xs-key"))
        return;
    fah = gtk_combo_box_get_active(combo) == 1;
    if (fah == priv->fahrenheit)
        return;   /* выбор не изменился — конфиг не трогаем */
    priv->fahrenheit = fah;
    g_key_file_set_string(priv->kf, priv->plugin->name, "units",
                          fah ? "fahrenheit" : "celsius");
    xs_core_plugin_conf_flush(priv->plugin->name);
    sen_read_values(priv);
    sen_rebuild_cache(priv, priv->cache_width, priv->cache_height);
    xs_host_api()->invalidate(priv->plugin);
    if (priv->plugin->win)
        gtk_widget_queue_draw(priv->plugin->win);
}

/* ------------------------------------------------------------- диалог */

typedef struct {
    GtkWidget *grid;
    int row;
} SenGrid;

static void sen_grid_new(SenGrid *g)
{
    g->grid = gtk_grid_new();
    g->row = 0;
    gtk_grid_set_row_spacing(GTK_GRID(g->grid), 4);
    gtk_grid_set_column_spacing(GTK_GRID(g->grid), 6);
}

/* Одна строка на одну настройку. Хелпер, привязывающий к жёсткой строке
 * 0, складывает все контролы секции В ОДИН ряд, и читаемыми остаются
 * два поля из девяти — видно только на скриншоте. */
static void sen_row(SenGrid *g, const char *label, GtkWidget *widget)
{
    GtkWidget *l = gtk_label_new(label);

    gtk_widget_set_halign(l, GTK_ALIGN_START);
    gtk_grid_attach(GTK_GRID(g->grid), l, 0, g->row, 1, 1);
    gtk_widget_set_hexpand(widget, TRUE);
    gtk_grid_attach(GTK_GRID(g->grid), widget, 1, g->row, 1, 1);
    g->row++;
}

static GtkWidget *sen_spin(const char *key, int value, int min, int max)
{
    GtkWidget *spin = gtk_spin_button_new_with_range(min, max, 1);

    gtk_spin_button_set_digits(GTK_SPIN_BUTTON(spin), SEN_SPIN_DIGITS);
    gtk_spin_button_set_value(GTK_SPIN_BUTTON(spin), value);
    gtk_widget_set_size_request(spin, 70, -1);
    g_object_set_data_full(G_OBJECT(spin), "xs-key", g_strdup(key), g_free);
    return spin;
}

static GtkWidget *sen_color_button(const char *key, const gdouble color[4])
{
    GdkRGBA rgba = { color[0], color[1], color[2], color[3] };
    GtkWidget *button = gtk_color_button_new_with_rgba(&rgba);

    g_object_set_data_full(G_OBJECT(button), "xs-key", g_strdup(key), g_free);
    return button;
}

static GtkWidget *sen_font_button(const char *key, const char *font)
{
    GtkWidget *button =
        gtk_font_button_new_with_font(font ? font : "Sans 8");

    gtk_widget_set_size_request(button, SEN_DIALOG_FONT, -1);
    g_object_set_data_full(G_OBJECT(button), "xs-key", g_strdup(key), g_free);
    return button;
}

/* Пересортировать строки по включённым галочкам. Порядок в конфиге
 * меняется по нажатию, а не по алфавиту: пользователь сам решает, что
 * сверху. При снятии галочки строка удаляется — иначе в апплете остаются
 * значения для сенсоров, которые пользователь убрал. */
/* Индекс строки в priv по её источнику. */
/* Устойчивый идентификатор строки. Тонкая обёртка над sensor_row_id()
 * нужна, чтобы смысл аргументов читался в месте вызова: chip, device и
 * label вместе дают ключ, не зависящий от номера hwmon. */

static guint sen_row_index(SenPriv *priv, const char *source)
{
    for (guint i = 0; i < priv->row_sources->len; i++) {
        if (g_strcmp0(g_ptr_array_index(priv->row_sources, i), source) == 0)
            return i;
    }
    return SEN_ROW_NONE;
}

/* Убрать тестовую строку dummy, если она есть.
 *
 * Заглушка должна жить ровно тогда, когда не выбран ни один реальный
 * сенсор. Как только пользователь отметил хоть один, она лишняя: рядом
 * с настоящими данными она читается как ещё один датчик с неизменным
 * значением, и это ровно то недоразумение, которое дефолт должен был
 * устранить.
 *
 * Функция безопасна при любом состоянии: строки может не быть, она
 * может быть единственной, а может не быть и priv->values — на этом
 * месте мы просто ничего не трогаем. */
static void sen_drop_dummy_row(SenPriv *priv)
{
    guint idx;

    if (!priv || !priv->row_sources)
        return;
    idx = sen_row_index(priv, SEN_DUMMY_SOURCE);
    if (idx == SEN_ROW_NONE)
        return;
    g_ptr_array_remove_index(priv->rows, idx);
    g_ptr_array_remove_index(priv->row_sources, idx);
    if (priv->values && idx < priv->values->len)
        g_ptr_array_remove_index(priv->values, idx);
}

/* Порядок групп: сначала те, где больше сенсоров, потом по имени.
 * Сортировка обязана быть детерминированной, иначе таблицы прыгали бы
 * при каждом открытии Properties. */
typedef struct {
    const char *name;
    guint rows;
} SenGroupOrder;

static gint sen_group_cmp(gconstpointer pa, gconstpointer pb)
{
    const SenGroupOrder *a = pa, *b = pb;

    if (a->rows != b->rows)
        return a->rows > b->rows ? -1 : 1;
    return strcmp(a->name, b->name);
}

/* Галочка «выводить эту строку» в таблице.
 *
 * GtkTreeView, а не GtkGrid: заголовки колонок, сортировка по клику и
 * выделение строки идут из коробки.
 *
 * Галочка — GtkCellRendererToggle, а НЕ GtkWidgetCellRenderer со
 * вложенным GtkCheckButton: виджетные рендереры в дереве не
 * перерисовываются при прокрутке и переиспользуют ячейки неверно —
 * таблица дёргается. Toggle-рендерер рисует себя сам.
 *
 * Строка ищется в priv по ИСТОЧНИКУ (колонка 4), а не по индексу
 * модели: строки переставляются сортировкой, индекс после этого ничего
 * не значит. И по индексу искать нельзя, и читать колонку 2: там
 * лежит читаемое имя для человека, а не ключ.
 *
 * ОБЯЗАТЕЛЬНО пишем результат обратно в модель. Обработчик меняет
 * priv, но GtkCellRendererToggle рисует значение из модели, и без
 * gtk_list_store_set() галочка визуально не зажигается — при том что
 * строка в апплете появляется.
 */
static void sen_tree_toggled(GtkCellRendererToggle *cell, gchar *path_str,
                             gpointer data)
{
    SenDialogContext *ctx = data;
    SenPriv *priv = sen_live_priv(ctx);
    GtkTreeView *view = g_object_get_data(G_OBJECT(cell), "xs-view");
    GtkTreeModel *model;
    GtkTreePath *path;
    GtkTreeIter iter;
    gboolean cur = FALSE, want;
    char *source = NULL;
    char *label = NULL;
    char *pending = NULL;
    guint idx;

    if (!priv || !view || ctx->building)
        return;
    model = gtk_tree_view_get_model(view);
    path = gtk_tree_path_new_from_string(path_str);
    if (!path)
        return;
    if (!gtk_tree_model_get_iter(model, &iter, path)) {
        gtk_tree_path_free(path);
        return;
    }
    /* 0 — галочка, 3 — метка, 4 — источник, 5 — отложенная метка. */
    gtk_tree_model_get(model, &iter, 0, &cur, 3, &label, 4, &source, 5,
                       &pending, -1);
    want = !cur;
    if (!source) {
        gtk_tree_path_free(path);
        goto out;
    }
    idx = sen_row_index(priv, source);

    if (want) {
        if (idx == SEN_ROW_NONE) {
            /* Отложенная метка (введённая до включения) важнее полного
             * имени: пользователь её уже напечатал. */
            const char *use = pending && *pending ? pending : label;

            /* Тестовая строка живёт ровно до первого выбранного
             * сенсора. Дальше она лишняя: пользователь отметил, что
             * хочет видеть, и заглушка рядом с его данными только
             * мешает — выглядит как ещё один датчик с фиксированным
             * значением. Убираем её здесь же, в момент включения, а не
             * при следующей записи в конфиг. */
            sen_drop_dummy_row(priv);

            g_ptr_array_add(priv->rows, g_strdup(use ? use : ""));
            g_ptr_array_add(priv->row_sources, g_strdup(source));
            if (priv->values)
                g_ptr_array_add(priv->values, NULL);
        }
    } else {
        if (idx != SEN_ROW_NONE) {
            /* g_ptr_array_remove_index не сдвигает хвост — следующая
             * строка получила бы чужой источник. */
            g_ptr_array_remove_index(priv->rows, idx);
            g_ptr_array_remove_index(priv->row_sources, idx);
            if (priv->values && idx < priv->values->len)
                g_ptr_array_remove_index(priv->values, idx);
            /* Снят последний сенсор — возвращаем заглушку сразу, при
             * клике. Иначе окно осталось бы пустым до перезапуска
             * демона, и пользователь решил бы, что апплет сломался:
             * пустой список выглядит как «ничего не рисуется», а
             * заглушка честно говорит «плагин жив, выбора нет». */
            if (priv->rows->len == 0) {
                g_ptr_array_add(priv->rows, g_strdup(SEN_DUMMY_LABEL));
                g_ptr_array_add(priv->row_sources,
                                g_strdup(SEN_DUMMY_SOURCE));
                if (priv->values)
                    g_ptr_array_add(priv->values, NULL);
            }
        }
    }
    /* Галочка в модели — иначе не зажжётся. */
    gtk_list_store_set(GTK_LIST_STORE(model), &iter, 0, want, -1);
    gtk_tree_path_free(path);

    sen_save(priv);
    sen_read_values(priv);
    sen_rebuild_cache(priv, priv->width, priv->height);
    xs_host_api()->invalidate(priv->plugin);
    if (priv->plugin->win)
        gtk_widget_queue_draw(priv->plugin->win);
out:
    g_free(source);
    g_free(label);
    g_free(pending);
}

/* Правка метки прямо в таблице: колонка 3.
 *
 * Метку можно ввести и ДО включения галочки — она лежит в колонке 5
 * и подставляется при включении. Раньше она вешалась на renderer, а он
 * один на всю таблицу: правка одной строки затирала другую.
 */
static void sen_tree_label_edited(GtkCellRendererText *cell, gchar *path_str,
                                  gchar *new_text, gpointer data)
{
    SenDialogContext *ctx = data;
    SenPriv *priv = sen_live_priv(ctx);
    GtkTreeView *view = g_object_get_data(G_OBJECT(cell), "xs-view");
    GtkTreeModel *model;
    GtkTreePath *path;
    GtkTreeIter iter;
    char *source = NULL;
    guint idx;

    if (!priv || !view || ctx->building)
        return;
    /* Пустая метка вывела бы голое число без подписи, поэтому её
     * игнорируем: строка остаётся с прежним текстом. */
    if (!new_text || !*new_text)
        return;
    model = gtk_tree_view_get_model(view);
    path = gtk_tree_path_new_from_string(path_str);
    if (!path)
        return;
    if (!gtk_tree_model_get_iter(model, &iter, path)) {
        gtk_tree_path_free(path);
        return;
    }
    gtk_tree_model_get(model, &iter, 4, &source, -1);
    gtk_tree_path_free(path);
    if (!source)
        goto out;
    idx = sen_row_index(priv, source);

    if (idx == SEN_ROW_NONE) {
        /* Строка не выводится — откладываем метку до включения. */
        gtk_list_store_set(GTK_LIST_STORE(model), &iter, 3, new_text, 5,
                           new_text, -1);
        goto out;
    }
    g_free(g_ptr_array_index(priv->rows, idx));
    g_ptr_array_index(priv->rows, idx) = g_strdup(new_text);
    gtk_list_store_set(GTK_LIST_STORE(model), &iter, 3, new_text, -1);
    sen_save(priv);
    sen_read_values(priv);
    sen_rebuild_cache(priv, priv->width, priv->height);
    xs_host_api()->invalidate(priv->plugin);
    if (priv->plugin->win)
        gtk_widget_queue_draw(priv->plugin->win);
out:
    g_free(source);
}

/* Таблица одной группы: имя группы заголовком, четыре колонки.
 *
 * Четыре параллельных массива, и ПЕРЕПУТАТЬ их нельзя: sources — это
 * устойчивые идентификаторы «чип/устройство/канал», kinds — названия
 * каналов. Именно этой ошибкой строка «Чтото» получила источник
 * «loc1» вместо «i350bb/0500/loc1» и осталась без числа. */
static GtkWidget *sen_group_table(SenDialogContext *ctx, const char *group,
                                  GPtrArray *slots, GPtrArray *kinds,
                                  GPtrArray *sources)
{
    GtkWidget *frame = gtk_frame_new(group);
    GtkWidget *scroll = gtk_scrolled_window_new(NULL, NULL);
    GtkWidget *outer = gtk_box_new(GTK_ORIENTATION_VERTICAL, 2);
    GtkListStore *store;
    GtkTreeView *view;
    GtkCellRenderer *r;
    GtkTreeViewColumn *col;

    /* Свой скроллер внутри таблицы: групп пять, суммарно 46 строк, и
     * coretemp-isa (18 строк) растянула бы страницу Properties на весь
     * экран. */
    gtk_scrolled_window_set_policy(GTK_SCROLLED_WINDOW(scroll),
                                   GTK_POLICY_NEVER, GTK_POLICY_AUTOMATIC);
    gtk_scrolled_window_set_shadow_type(GTK_SCROLLED_WINDOW(scroll),
                                        GTK_SHADOW_NONE);
    gtk_widget_set_size_request(scroll, -1, 132);
    gtk_container_add(GTK_CONTAINER(scroll), outer);

    /* Шесть колонок, и порядок важен — на нём держится вся идентификация:
     *   0 вывод    — галочка, TRUE если строка выводится
     *   1 устройство — короткий слот («1-10», «8100»)
     *   2 канал    — полное имя sensors(1), для чтения человеком
     *   3 метка    — редактируемое поле
     *   4 источник — устойчивый ключ «чип/устройство/канал» (СКРЫТЫЙ)
     *   5 отложено  — метка, введённая ДО включения галочки
     *
     * Источник обязан лежать в модели отдельной колонкой. Раньше его там
     * не было, обработчики читали колонку 2 (читаемое имя), и
     * sen_row_index() всегда возвращал SEN_ROW_NONE: включение добавляло
     * строку в priv, но галочка не зажигалась и повторный клик добавлял
     * дубль, а снятие не работало вовсе. */
    store = gtk_list_store_new(6, G_TYPE_BOOLEAN, G_TYPE_STRING,
                               G_TYPE_STRING, G_TYPE_STRING, G_TYPE_STRING,
                               G_TYPE_STRING);
    view = GTK_TREE_VIEW(gtk_tree_view_new_with_model(
        GTK_TREE_MODEL(store)));
    gtk_tree_view_set_headers_visible(view, TRUE);
    gtk_tree_view_set_enable_search(view, FALSE);

    /* «вывод» — галочка */
    r = gtk_cell_renderer_toggle_new();
    g_signal_connect(r, "toggled", G_CALLBACK(sen_tree_toggled), ctx);
    g_object_set_data(G_OBJECT(r), "xs-view", view);
    col = gtk_tree_view_column_new_with_attributes("вывод", r, "active", 0,
                                                   NULL);
    gtk_tree_view_column_set_clickable(col, TRUE);
    gtk_tree_view_column_set_sizing(col, GTK_TREE_VIEW_COLUMN_FIXED);
    gtk_tree_view_column_set_fixed_width(col, 44);
    gtk_tree_view_append_column(view, col);

    /* «устройство» — короткое имя слота («1-10», «8100»). Не редактируется:
     * оно выведено из пути устройства, правка сделала бы строку
     * неузнаваемой. */
    r = gtk_cell_renderer_text_new();
    col = gtk_tree_view_column_new_with_attributes("устройство", r, "text", 1,
                                                   NULL);
    gtk_tree_view_column_set_clickable(col, TRUE);
    gtk_tree_view_column_set_sizing(col, GTK_TREE_VIEW_COLUMN_FIXED);
    gtk_tree_view_column_set_fixed_width(col, 78);
    gtk_tree_view_append_column(view, col);

    /* «канал» — полное имя в стиле sensors(1), серым. */
    r = gtk_cell_renderer_text_new();
    col = gtk_tree_view_column_new_with_attributes("канал", r, "text", 2, NULL);
    gtk_tree_view_column_set_clickable(col, TRUE);
    gtk_tree_view_column_set_sizing(col, GTK_TREE_VIEW_COLUMN_AUTOSIZE);
    gtk_tree_view_append_column(view, col);

    /* «метка» — то, что видит апплет. Правится прямо в таблице. */
    r = gtk_cell_renderer_text_new();
    g_object_set(r, "editable", TRUE, NULL);
    g_signal_connect(r, "edited", G_CALLBACK(sen_tree_label_edited), ctx);
    g_object_set_data(G_OBJECT(r), "xs-view", view);
    col = gtk_tree_view_column_new_with_attributes("метка", r, "text", 3, NULL);
    gtk_tree_view_column_set_clickable(col, TRUE);
    gtk_tree_view_column_set_expand(col, TRUE);
    gtk_tree_view_append_column(view, col);

    for (guint i = 0; i < slots->len; i++) {
        const char *slot = g_ptr_array_index(slots, i);
        const char *kind = g_ptr_array_index(kinds, i);
        const char *source = g_ptr_array_index(sources, i);
        guint idx = sen_row_index(ctx->priv, source);
        gboolean active = idx != SEN_ROW_NONE;
        const char *row_label = active
        ? (const char *) g_ptr_array_index(ctx->priv->rows, idx) : NULL;
        char *full = g_strdup_printf("%s · %s", group, kind);
        GtkTreeIter it;

        gtk_list_store_append(store, &it);
        /* Метка по умолчанию — полное имя сенсора: пользователь видит, что
         * выбрал, и переименовывает то, что ему нужно. */
        gtk_list_store_set(store, &it,
                           0, active,          /* вывод     */
                           1, slot,             /* устройство */
                           2, full,             /* канал     */
                           3, row_label ? row_label : full,  /* метка */
                           4, source,           /* источник (скрытый) */
                           5, NULL,             /* отложено   */
                           -1);
        g_free(full);
    }
    /* Модель нужна колонке «метка» для записи отредактированного текста,
     * поэтому она остаётся живой: gtk_tree_view_new_with_model() её не
     * забирает. Владение — у GtkTreeView, второй unref не нужен. */
    g_object_unref(store);
    gtk_container_add(GTK_CONTAINER(outer), GTK_WIDGET(view));
    gtk_container_add(GTK_CONTAINER(frame), scroll);
    return frame;
}

/* Список сенсоров таблицами по типам модулей.
 *
 * Был единый список из 46 строк с полями ввода, где подпись была уже
 * развёрнутой («nvme-pci-8100 · Composite») и не сказано было, какой это
 * контроллер. Теперь на каждый тип модуля своя таблица, а внутри
 * короткое имя слота и отдельное поле метки. */
static GtkWidget *sen_sensor_list(SenDialogContext *ctx)
{
    GtkWidget *box = gtk_box_new(GTK_ORIENTATION_VERTICAL, 10);
    SensorList *found;
    GHashTable *groups;    /* имя группы -> GPtrArray индексов строк */
    GPtrArray *order;      /* имена групп, в порядке вывода */
    GPtrArray *kind, *slot, *source;
    guint total = 0;

    ctx->building = TRUE;
    gtk_widget_set_size_request(box, -1, 300);

    found = sensor_list_read("/sys/class/hwmon");
    if (!found) {
        GtkWidget *empty = gtk_label_new("Сенсоры не найдены");

        gtk_container_add(GTK_CONTAINER(box), empty);
        ctx->building = FALSE;
        return box;
    }

    kind = g_ptr_array_new_with_free_func(g_free);
    slot = g_ptr_array_new_with_free_func(g_free);
    source = g_ptr_array_new_with_free_func(g_free);
    groups = g_hash_table_new_full(g_str_hash, g_str_equal, g_free,
                                   (GDestroyNotify) g_ptr_array_unref);
    order = g_ptr_array_new_with_free_func(g_free);

    for (guint i = 0; i < found->chips->len; i++) {
        SensorChip *c = g_ptr_array_index(found->chips, i);
        char *group = sensor_group_name(c->chip, c->dev_path);
        char *short_slot = sensor_bus_slot(c->dev_path);

        for (guint j = 0; j < c->readings->len; j++) {
            SensorReading *r = g_ptr_array_index(c->readings, j);
            GPtrArray *rows = g_hash_table_lookup(groups, group);

            if (!rows) {
                rows = g_ptr_array_new();
                g_hash_table_insert(groups, g_strdup(group), rows);
                g_ptr_array_add(order, g_strdup(group));
            }
            g_ptr_array_add(kind, g_strdup(r->label));
            g_ptr_array_add(slot, g_strdup(short_slot));
            g_ptr_array_add(source, sensor_row_id(c->chip, c->device, r->label));
            /* GPtrArray хранит указатели, а индекс — число: приводим явно. */
            g_ptr_array_add(rows, GUINT_TO_POINTER(kind->len - 1));
            total++;
        }
        g_free(group);
        g_free(short_slot);
    }

    /* Карты NVIDIA в дереве hwmon отсутствуют (у драйвера nvidia нет
     * /sys/class/hwmon-узла), поэтому список настроек дополняем NVML.
     * Без этого в таблице не было бы ни одной строки карты, и
     * выбрать её было бы негде. */
    {
        SensorList *nv = nvml_sensor_list_read();

        if (nv) {
            for (guint i = 0; i < nv->chips->len; i++) {
                SensorChip *c = g_ptr_array_index(nv->chips, i);
                char *group = sensor_group_name(c->chip, c->dev_path);
                char *short_slot = sensor_bus_slot(c->dev_path);

                for (guint j = 0; j < c->readings->len; j++) {
                    SensorReading *r = g_ptr_array_index(c->readings, j);
                    GPtrArray *rows = g_hash_table_lookup(groups, group);

                    if (!rows) {
                        rows = g_ptr_array_new();
                        g_hash_table_insert(groups, g_strdup(group), rows);
                        g_ptr_array_add(order, g_strdup(group));
                    }
                    g_ptr_array_add(kind, g_strdup(r->label));
                    g_ptr_array_add(slot, g_strdup(short_slot));
                    g_ptr_array_add(source,
                                    sensor_row_id(c->chip, c->device,
                                                  r->label));
                    g_ptr_array_add(rows, GUINT_TO_POINTER(kind->len - 1));
                    total++;
                }
                g_free(group);
                g_free(short_slot);
            }
            sensor_list_free(nv);
        }
    }
    g_print("sensors: строк=%u групп=%u\n", total, order->len);

    {
        GPtrArray *sorted = g_ptr_array_new_with_free_func(g_free);

        for (guint i = 0; i < order->len; i++) {
            const char *g = g_ptr_array_index(order, i);
            GPtrArray *rows = g_hash_table_lookup(groups, g);
            SenGroupOrder *item = g_new(SenGroupOrder, 1);

            item->name = g;
            item->rows = rows->len;
            g_ptr_array_add(sorted, item);
        }
        g_ptr_array_sort(sorted, sen_group_cmp);

        for (guint gi = 0; gi < sorted->len; gi++) {
            SenGroupOrder *item = g_ptr_array_index(sorted, gi);
            GPtrArray *rows = g_hash_table_lookup(groups, item->name);
            GPtrArray *k = g_ptr_array_new_with_free_func(g_free);
            GPtrArray *sl = g_ptr_array_new_with_free_func(g_free);
            GPtrArray *sr = g_ptr_array_new_with_free_func(g_free);
            GtkWidget *table;

            for (guint i = 0; i < rows->len; i++) {
                guint idx = GPOINTER_TO_UINT(g_ptr_array_index(rows, i));

                g_ptr_array_add(k, g_strdup(g_ptr_array_index(kind, idx)));
                g_ptr_array_add(sl, g_strdup(g_ptr_array_index(slot, idx)));
                g_ptr_array_add(sr, g_strdup(g_ptr_array_index(source, idx)));
            }
            /* Порядок: слоты, названия каналов, ИСТОЧНИКИ. Источники
             * обязательно из sr: в k лежат названия каналов («loc1»), и
             * строка с таким источником не находит сенсор — видна подпись
             * без числа. */
            table = sen_group_table(ctx, item->name, sl, k, sr);
            gtk_box_pack_start(GTK_BOX(box), table, FALSE, FALSE, 0);
        }
        g_ptr_array_unref(sorted);
    }

    g_ptr_array_unref(kind);
    g_ptr_array_unref(slot);
    g_ptr_array_unref(source);
    g_ptr_array_unref(order);
    g_hash_table_destroy(groups);
    sensor_list_free(found);
    ctx->building = FALSE;
    return box;
}


/* Привязать обработчики по ключу. Рекурсивный обход вместо ручного:
 * сигнатуры у font-set, color-set, value-changed и toggled разные. */
static void sen_connect_children(GtkWidget *widget, SenDialogContext *ctx)
{
    if (GTK_IS_FONT_BUTTON(widget))
        g_signal_connect(widget, "font-set", G_CALLBACK(sen_font_set), ctx);
    else if (GTK_IS_COLOR_BUTTON(widget))
        g_signal_connect(widget, "color-set", G_CALLBACK(sen_color_set), ctx);
    else if (GTK_IS_SPIN_BUTTON(widget))
        g_signal_connect(widget, "value-changed",
                         G_CALLBACK(sen_int_changed), ctx);
    else if (GTK_IS_COMBO_BOX_TEXT(widget) &&
             g_object_get_data(G_OBJECT(widget), "xs-key"))
        g_signal_connect(widget, "changed",
                         G_CALLBACK(sen_units_changed), ctx);
    /* Галка выравнивания несёт xs-key, в отличие от галочек выбора
     * сенсоров, где xs-source (они подключаются в sen_sensor_list).
     * Проверка ключа обязательна: иначе в этот обработчик попали бы и
     * те, и он получил бы не тот тип данных. */
    else if (GTK_IS_TOGGLE_BUTTON(widget) &&
             g_object_get_data(G_OBJECT(widget), "xs-key"))
        g_signal_connect(widget, "toggled",
                         G_CALLBACK(sen_bool_changed), ctx);

    if (GTK_IS_CONTAINER(widget)) {
        GList *children = gtk_container_get_children(GTK_CONTAINER(widget));

        for (GList *it = children; it; it = it->next)
            sen_connect_children(GTK_WIDGET(it->data), ctx);
        g_list_free(children);
    }
}

static void sen_context_free(SenDialogContext *ctx)
{
    if (!ctx)
        return;
    g_free(ctx->instance_name);
    g_free(ctx);
}

static void sen_properties(XsPlugin *p, GtkNotebook *notebook)
{
    SenPriv *priv = p ? p->priv : NULL;
    SenDialogContext *ctx;
    GtkWidget *page, *scroller, *inner, *frame;
    SenGrid g;
    int pos_max;

    if (!priv)
        return;
    ctx = g_new0(SenDialogContext, 1);
    ctx->priv = priv;
    /* Имя инстанса = имя секции конфига; обработчики находят по нему priv. */
    ctx->instance_name = g_strdup(p->name);
    pos_max = MAX(priv->design_width, priv->design_height) - 1;

    page = gtk_box_new(GTK_ORIENTATION_VERTICAL, 8);
    scroller = gtk_scrolled_window_new(NULL, NULL);
    /* Скроллер ограничен по высоте и НЕ сообщает своё содержимое вверх:
     * с propagate=TRUE он требует, чтобы страница была размером со все
     * секции, и окно диалога вырастает до размера всего содержимого. */
    gtk_scrolled_window_set_policy(GTK_SCROLLED_WINDOW(scroller),
                                   GTK_POLICY_NEVER, GTK_POLICY_AUTOMATIC);
    gtk_scrolled_window_set_propagate_natural_width(
        GTK_SCROLLED_WINDOW(scroller), FALSE);
    gtk_scrolled_window_set_propagate_natural_height(
        GTK_SCROLLED_WINDOW(scroller), FALSE);
    /* Содержимое живёт ВНУТРИ скроллера. Раньше секции добавлялись в
     * page после скроллера, и тот оставался пустым: диалог не
     * прокручивался, а секции Вид/Раскладка/Окно уезжали за край окна. */
    inner = gtk_box_new(GTK_ORIENTATION_VERTICAL, 8);
    gtk_widget_set_margin_start(inner, 8);
    gtk_widget_set_margin_end(inner, 8);
    gtk_widget_set_margin_bottom(inner, 8);
    gtk_container_add(GTK_CONTAINER(scroller), inner);
    gtk_box_pack_start(GTK_BOX(page), scroller, TRUE, TRUE, 0);

    /* --- Сенсоры --- */
    frame = gtk_frame_new("Показывать");
    {
        GtkWidget *list = sen_sensor_list(ctx);

        sen_grid_new(&g);
        gtk_widget_set_margin_start(list, 8);
        gtk_widget_set_margin_end(list, 8);
        gtk_widget_set_margin_bottom(list, 8);
        gtk_grid_attach(GTK_GRID(g.grid), list, 0, 0, 1, 1);
        g.row = 1;
        gtk_container_add(GTK_CONTAINER(frame), g.grid);
    }
    gtk_box_pack_start(GTK_BOX(inner), frame, FALSE, FALSE, 0);

    /* --- Вид --- общие настройки для меток и чисел, без исключений */
    frame = gtk_frame_new("Вид");
    sen_grid_new(&g);
    sen_row(&g, "Метка", sen_font_button("label_font", priv->label_font));
    sen_row(&g, "Цвет метки",
            sen_color_button("label_color", priv->label_color));
    sen_row(&g, "Число", sen_font_button("value_font", priv->value_font));
    sen_row(&g, "Цвет числа",
            sen_color_button("value_color", priv->value_color));
    {
        /* Переключатель, а не флажок: у шкалы две равноправные
         * альтернативы, и одна опция «Фаренгейт» читалась как
         * «включён ли перевод в фаренгейты» — а это и в градусах Цельсия
         * тоже идёт перевод. Флажок не может выразить выбор. */
        GtkWidget *combo = gtk_combo_box_text_new();

        gtk_combo_box_text_append_text(GTK_COMBO_BOX_TEXT(combo), "Celsius");
        gtk_combo_box_text_append_text(GTK_COMBO_BOX_TEXT(combo), "Fahrenheit");
        gtk_combo_box_set_active(GTK_COMBO_BOX(combo),
                                 priv->fahrenheit ? 1 : 0);
        g_object_set_data_full(G_OBJECT(combo), "xs-key",
                               g_strdup("units"), g_free);
        sen_row(&g, "Единицы", combo);
    }
    gtk_container_add(GTK_CONTAINER(frame), g.grid);
    gtk_box_pack_start(GTK_BOX(inner), frame, FALSE, FALSE, 0);

    /* --- Раскладка --- */
    frame = gtk_frame_new("Раскладка");
    sen_grid_new(&g);
    sen_row(&g, "Позиция метки", sen_spin("label_x", priv->label_x, 0,
                                         pos_max));
    sen_row(&g, "Позиция числа", sen_spin("value_x", priv->value_x, 0,
                                         pos_max));
    sen_row(&g, "Число вправо",
            sen_check("value_align_right", priv->value_align_right));
    sen_row(&g, "Первая строка", sen_spin("first_row_y", priv->first_row_y, 0,
                                          pos_max));
    sen_row(&g, "Шаг строк", sen_spin("line_step", priv->line_step, 1,
                                     pos_max));
    gtk_container_add(GTK_CONTAINER(frame), g.grid);
    gtk_box_pack_start(GTK_BOX(inner), frame, FALSE, FALSE, 0);

    /* --- Окно --- */
    frame = gtk_frame_new("Окно");
    sen_grid_new(&g);
    sen_row(&g, "Ширина",
            sen_spin("window_width", priv->width, SEN_MIN_WIDTH, 1600));
    sen_row(&g, "Высота",
            sen_spin("window_height", priv->height, SEN_MIN_HEIGHT, 1200));
    sen_row(&g, "Скругление", sen_spin("corner_radius", priv->corner_radius,
                                      0, SEN_MAX_RADIUS));
    sen_row(&g, "Обновление, мс",
            sen_spin("update_ms", priv->update_ms, 200, 60000));
    sen_row(&g, "Фон",
            sen_color_button("background_color", priv->background_color));
    sen_row(&g, "Рамка",
            sen_color_button("border_color", priv->border_color));
    gtk_container_add(GTK_CONTAINER(frame), g.grid);
    gtk_box_pack_start(GTK_BOX(inner), frame, FALSE, FALSE, 0);

    sen_connect_children(page, ctx);

    /* Высота страницы — это высота ОКНА диалога, а не содержимого:
     * содержимое прокручивается внутри. Запрос 700 растягивал окно на
     * две трети экрана ради контента, который прокручивается. */
    gtk_widget_set_size_request(page, 520, 620);
    gtk_notebook_append_page(notebook, page, gtk_label_new("Sensors"));
    /* Контекст живёт до конца окна: контролы переживают properties(). */
    g_object_set_data_full(G_OBJECT(page), "xs-sen-ctx", ctx,
                           (GDestroyNotify) sen_context_free);
}

static const XsPluginOps sen_ops = {
    .init = sen_init,
    .draw = sen_draw,
    .tick = sen_tick,
    .shutdown = sen_shutdown,
    .properties = sen_properties,
};

static XsPluginDesc sen_desc = {
    "sensors",
    XS_API_VERSION,
    &sen_ops,
    "Температуры сенсоров: метка: число",
    "kosmik2001",
    "1.0"
};

XS_PLUGIN_EXPORT(&sen_desc)
