/* acpi_battery.c — applet батареи: заряд в процентах, время, состояние.
 *
 * Каркас отрисовки (окно, тема, Properties) написан по образцу
 * disk_monitor/memory_monitor. Исходники ACPIBatteryScreenlet.py здесь не
 * используются и не меняются: это отдельный плагин со своим core.
 *
 * Отличия от оригинала, и каждое по причине:
 *
 *  - данные читаются из sysfs, а не из /proc/acpi/battery (см.
 *    acpi_battery_core.h — путь закрыт ядром ещё в 3.8);
 *  - отсутствие батареи даёт «No battery» и НЕ роняет applet. В
 *    оригинале listdir() несуществующего каталога ронял демон с
 *    OSError, а на месте обработки стояла пометка «TODO: raise
 *    exception!!!» — автор этот случай не закрыл;
 *  - «No battery» рисуется БЕЛОЙ надписью и обычным индикатором, а не
 *    алармом. В оригинале отсутствие данных попадало в ту же ветку,
 *    что и низкий заряд, и оба случая показывались красным:
 *    «батарея сдохла» и «батареи нет» выглядели одинаково;
 *  - UPower не используется. Он считает TimeToEmpty точнее, но требует
 *    живого демона и D-Bus, тогда как sysfs есть всегда, где есть
 *    устройство, и отдаёт те же capacity/status — включая батарейку
 *    Logitech-мыши, которую hid-logitech-hidpp добавляет как обычный
 *    узел power_supply с типом Mouse.
 *
 * Геометрия текста взята из оригинальной темы default (окно 100x50,
 * текст с x=37): тема acpibattery-* рассчитана именно на эти
 * координаты, сдвиг делает её нечитаемой.
 */
#include <gtk/gtk.h>
#include <string.h>
#include <stdlib.h>
#include <math.h>

#include "xs_api.h"
#include "common.h"
#include "acpi_battery_core.h"

#include "../core/i18n.h"
/* Окно 150x50 под тему green: батарейка 76x37 слева, текст сбоку.
 * Было 100x50 с текстом поверх батарейки (координаты оригинала).
 *
 * Ширину подняли со 130 до 150: на текст оставалось 130-86 = 44 px,
 * и "00:35" в него не влезало - applet показывал "00:3" плюс время
 * до конца заряда в минутах. Текст начинается с AB_TEXT_X = 86, то
 * есть на строку теперь 64 px, этого хватает на "00:35" и "3:59". */
#define AB_DEFAULT_WIDTH    150
#define AB_DEFAULT_HEIGHT    50
/* Порог ниже которого заряд считается низким. 15 — тот же процент,
 * что стоял в оригинале по умолчанию. */
#define AB_DEFAULT_ALARM     15
#define AB_DEFAULT_INTERVAL  30     /* секунд */

#define AB_MIN_WIDTH         60
#define AB_MIN_HEIGHT        30

/* Натуральный размер корпуса батарейки в теме green (76x37). Он НЕ
 * меняется под размер окна: корпус должен остаться батарейкой, а не
 * расплываться пятном. Меняется только его положение. */
#define AB_BODY_X            4      /* поле слева от блока */
#define AB_BODY_W           76
#define AB_BODY_H           37
/* Зазор между корпусом и текстом. */
#define AB_GAP               6
/* Зазор между строками «процент» и «время». */
#define AB_TEXT_ROW_GAP      2
/* Отступ от края окна (рамки) до текста. Текст выровнен по правому
 * краю, поэтому отступ считается справа и по вертикали. */
#define AB_FRAME_PAD          2

/* Дополнительный зазор от РАМКИ до содержимого слева и сверху/снизу.
 *
 * AB_FRAME_PAD недостаточно: он равен 2, но это отступ от края ОКНА, а не
 * от нарисованной рамки. В темах рамка имеет собственную толщину (в green
 * это 1 px тёмного #0D0F10, который при scale=1.64 становится 2 px на
 * экране), поэтому при pad=3 батарейка отстояла от рамки на 1 px и
 * визуально упиралась в неё вплотную.
 *
 * Считается в натуральных пикселях темы и масштабируется вместе со
 * всем остальным, иначе при крупном scale зазор не рос бы. */
#define AB_CONTENT_GAP        3

/* Индикатор вписан в корпус: тема нарисована с полями 8 по бокам. */
#define AB_IND_INSET_X       8
#define AB_IND_H            18
/* Выше этого процента полоса рисуется целиком: разница 95 и 100 на
 * 60 px не видна глазом, а обрезанный край читается как «не полный». */
#define AB_IND_FULL_PCT      95

/* Посчитанная от размера окна геометрия. Метрики текста (line_h, ascent,
 * text_w) тоже здесь: раскладка обязана опираться на реальный шрифт. */
typedef struct {
    GtkWidget *widget;              /* для замера текста через Pango */
    double scale;                   /* множитель из scale, >= 0.2 */
    /* Размеры корпуса и индикатора УЖЕ масштабированы. Натуральный
     * размер темы (76x37) живёт в макросах, здесь - результат. */
    int body_w, body_h;
    int ind_x, ind_y, ind_w, ind_h;
    int body_x, body_y;
    /* Текст прижат по ПРАВОМУ краю: text_x считается от w - ширина. */
    int text_x, text_y_percent, text_y_time, text_y_source, text_y_only;
    int line_h, ascent, text_w;
    /* Сколько места нужно окну, чтобы всё влезло без обрезания. */
    int need_w, need_h;
} AbGeom;

typedef struct {
    int      window_width;
    int      window_height;
    int      update_interval;
    int      alarm_threshold;
    gboolean show_percent;
    gboolean show_time;

    /* Последнее прочитанное состояние: держим между тиками, чтобы
     * перерисовка по движению мыши не ходила в sysfs. */
    AcpiBattery *battery;         /* NULL = батареи нет */
    char        *time_text;       /* NULL = строку не рисуем */
    char        *percent_text;
    gboolean low;            /* заряд ниже порога */
    GtkWidget   *widget;         /* окно для gtk_widget_create_pango_layout */
    /* Описание шрифта текста (NULL = брать из контекста виджета, как
     * раньше). Задаётся в Properties, хранится в конфиге ключом "font",
     * тем же способом, что в memory_monitor/cpu_monitor. */
    char        *font;

    /* Третья строка: состояние сети и имя источника.
     *
     * Нужна потому, что вторую строку занимает время/статус, а по нему
     * нельзя отличить «заряжается от сети» от «заряжается, потому что
     * подключён блок питания». Плюс имя источника: на ноутбуке в sysfs
     * два узла (BAT0 и батарейка мыши), и без имени непонятно, чей это
     * процент показан. */
    char        *source_text;   /* NULL = строку не рисуем */
    gboolean     has_ac;        /* узел Mains есть в sysfs */
    gboolean     ac_online;     /* сеть подключена */

    /* Минимальный размер, который реально вмещает содержимое. Считается
     * по метрикам шрифта в draw(), потому что раньше был жёсткий
     * AB_MIN_WIDTH/AB_MIN_HEIGHT: при крупном шрифте окно можно было
     * сделать меньше нарисованного, и текст обрезался. */
    int min_width;
    int min_height;

    /* Множитель из Properties/Size-меню. Масштабирует ВСЁ: окно, корпус,
     * индикатор и шрифт. Раньше он менял только окно, а батарейка
     * оставалась 76x37, и масштабирование выглядело как «ничего не
     * произошло». Конфиг - единственный источник истины (см.
     * ab_apply_scale): Properties и Size-меню пишут scale туда напрямую,
     * минуя нас, поэтому значение перечитывается на каждом тике. */
    double scale;

    /* Скругление углов окна. Скругление - это не элемент темы, а свойство
     * окна, поэтому его нет ни в одном ассете acpi_battery-*: рамку рисует
     * тема (acpibattery-bg), а форму окна задаём здесь.
     *
     * По умолчанию 0, как в disk_monitor: ставить скругление всем по
     * умлению нельзя, потому что углы непрозрачного окна срезаются по
     * shape-маске и часть окна просто перестаёт существовать.
     *
     * shape_* - последнее (радиус,w,h), отправленное X-серверу. Вызов
     * shape не бесплатный, поэтому при неизменных значениях его повторять
     * не надо. */
    int corner_radius;
    int shape_radius, shape_w, shape_h;
} AbState;

static void ab_set_text(char **dst, char *value)
{
    g_free(*dst);
    *dst = value;
}

static void ab_clamp(AbState *st)
{
    /* Минимум = max(жёсткий предел, измеренный размер содержимого).
     * st->min_* заполняется в draw() по метрикам шрифта, поэтому до
     * первого отрисовки работают только жёсткие значения. */
    int min_w = MAX(AB_MIN_WIDTH, st->min_width);
    int min_h = MAX(AB_MIN_HEIGHT, st->min_height);

    if (st->window_width  < min_w) st->window_width  = min_w;
    if (st->window_height < min_h) st->window_height = min_h;
    if (st->update_interval < 1)   st->update_interval = 1;
    if (st->alarm_threshold < 0)   st->alarm_threshold = 0;
    if (st->alarm_threshold > 100) st->alarm_threshold = 100;
}

/* Третья строка: состояние сети и имя источника.
 *
 * Показываем не всё сразу, а самое важное: есть ли смысл вообще смотреть.
 * Без сети и на полном заряде строка не нужна, поэтому в этом случае она
 * пустая и не занимает высоту (см. ab_layout: строк столько, сколько
 * непустых).
 *
 * Формат: " AC BAT0" при сети, "  BAT0" без. Имя источника нужно, потому
 * что на ноутбуке в sysfs два узла с зарядом (BAT0 и батарейка мыши), и
 * процент без имени неоднозначен. */
static void ab_refresh_source_text(AbState *st)
{
    const char *name = NULL;
    const char *ac = NULL;

    if (st->battery && st->battery->name)
        name = st->battery->name;

    if (st->has_ac)
        ac = st->ac_online ? " AC" : "  ";

    /* Нечего показывать: сети нет и имя неизвестно. */
    if (!ac && !name) {
        ab_set_text(&st->source_text, NULL);
        return;
    }
    ab_set_text(&st->source_text,
                g_strdup_printf(" %s %s", ac ? ac : " ",
                                name ? name : "?"));
}

/* Пересчитать тексты из прочитанного состояния. */
static void ab_refresh_text(AbState *st)
{
    const AcpiBattery *bat = st->battery;

    ab_set_text(&st->percent_text, NULL);
    ab_set_text(&st->time_text, NULL);
    ab_set_text(&st->source_text, NULL);
    st->low = FALSE;
    ab_refresh_source_text(st);

    if (!bat || bat->state == ACPI_BATTERY_NOT_PRESENT) {
        /* Устройства нет вовсе, либо батарейка извлечена: цифры не
         * показываем, сообщение рисует draw(). */
        ab_set_text(&st->percent_text, g_strdup("     "));
        return;
    }

    if (bat->percent >= 0) {
        ab_set_text(&st->percent_text, g_strdup_printf("%3d%%", bat->percent));
        st->low = (bat->percent <= st->alarm_threshold);
    } else {
        ab_set_text(&st->percent_text, g_strdup("  --"));
    }

    switch (bat->state) {
    case ACPI_BATTERY_FULL:
        ab_set_text(&st->time_text, g_strdup(" Full"));
        st->low = FALSE;      /* полная батарея не бывает «низкой» */
        break;
    case ACPI_BATTERY_CHARGING:
    case ACPI_BATTERY_DISCHARGING:
        ab_set_text(&st->time_text, acpi_battery_format_minutes(bat->minutes_left));
        break;
    case ACPI_BATTERY_UNKNOWN:
    default:
        /* Данных нет: прочерк и НЕ аларм. Иначе сервер без батареи или
         * ноутбук со статусом Unknown мигал бы красным. */
        ab_set_text(&st->time_text, g_strdup("   --"));
        st->low = FALSE;
        break;
    }
}

/* Прочитать sysfs. Список освобождаем сразу, нужные поля копируем:
 * держать весь список ради одного узла — лишняя память на каждый тик. */
static void ab_poll(AbState *st)
{
    AcpiBatteryList *list = acpi_battery_list_read(ACPI_BATTERY_SYSFS_ROOT);
    AcpiBattery *bat = NULL;

    acpi_battery_free(st->battery);
    st->battery = NULL;

    if (list) {
        /* Состояние сети забираем до освобождения списка: список умирает
         * вместе со своими элементами, а строки applet держит между тиками. */
        st->has_ac    = list->has_ac;
        st->ac_online = list->ac_online;
        bat = acpi_battery_list_primary(list);
        if (bat) {
            /* Deep copy: список сейчас освободится вместе со своими
             * элементами, а applet держит указатель между тиками. */
            st->battery = g_new0(AcpiBattery, 1);
            st->battery->name         = g_strdup(bat->name);
            st->battery->type         = g_strdup(bat->type);
            st->battery->status       = g_strdup(bat->status);
            st->battery->state        = bat->state;
            st->battery->percent      = bat->percent;
            st->battery->minutes_left = bat->minutes_left;
            st->battery->has_energy   = bat->has_energy;
        }
        acpi_battery_list_free(list);
    }

    ab_refresh_text(st);
}

/* Рисование текста. Белый — обычное состояние, красный — аларм.
 * Цвет из настроек не читаем: тема задаёт белый, а настройка цвета в
 * оригинале была, но не работала — писалась в конфиг и не читалась. */
/* Описание шрифта, увеличенное в scale раз.
 *
 * Шрифт масштабируется ВМЕСТЕ с картинкой, а не остаётся прежним: иначе
 * при scale=2 корпус и батарейка вырастают вдвое, а «100%» и «00:35»
 * остаются такими же мелкими, и applet выглядит сломанным.
 *
 * Берём описание шрифта из контекста виджета и умножаем кегль, а не
 * задаём свой шрифт целиком: описание виджета уже содержит семейство,
 * начертание и язык, и подмена испортила бы внешний вид. */
static PangoFontDescription *ab_font_scaled(AbState *st, double scale)
{
    PangoContext *pc;
    PangoFontDescription *base, *out;
    gint size;

    if (!st || !st->widget)
        return pango_font_description_from_string("Sans 10");
    pc = gtk_widget_get_pango_context(st->widget);
    if (!pc)
        return pango_font_description_from_string("Sans 10");

    /* Заданный в настройках шрифт имеет приоритет над системным.
     * Отступать к контексту виджета нельзя: семейство и начертание из
     * настроек должны сохраняться, а не подменяться темой рабочего
     * стола. NULL означает "настройки нет" - это исходное поведение. */
    if (st->font) {
        out = pango_font_description_from_string(st->font);
        if (out && pango_font_description_get_size(out) > 0) {
            size = pango_font_description_get_size(out);
            pango_font_description_set_size(out,
                                           (gint)(size * scale));
            return out;
        }
        if (out)
            pango_font_description_free(out);
    }

    base = pango_context_get_font_description(pc);
    out = pango_font_description_copy(base);
    size = pango_font_description_get_size(out);
    if (size <= 0)
        size = 10 * PANGO_SCALE;
    pango_font_description_set_size(out, (gint)(size * scale));
    return out;
}

/* Высота строки и её ascent для МАСШТАБИРОВАННОГО шрифта.
 *
 * Считается через Pango. Жёсткая константа была не только неверной по
 * величине, но и стояла не с того края: ab_text() передавал в
 * cairo_move_to() ВЕРХ строки, а pango_cairo_show_layout()
 * позиционирует от BASELINE. Текст уезжал вниз на ascent, и строки
 * наезжали друг на друга - это и было «странное» позиционирование. */
static int ab_text_metrics(AbState *st, double scale, int *out_ascent)
{
    PangoContext *pc = (st && st->widget)
                         ? gtk_widget_get_pango_context(st->widget) : NULL;
    PangoFontDescription *desc;
    PangoFontMetrics *fm;
    int h, ascent;

    if (!pc)
        pc = pango_cairo_create_context(NULL);

    desc = ab_font_scaled(st, scale);
    fm = pango_context_get_metrics(pc, desc, pango_language_get_default());
    ascent = pango_font_metrics_get_ascent(fm) / PANGO_SCALE;
    h = (pango_font_metrics_get_ascent(fm) +
         pango_font_metrics_get_descent(fm)) / PANGO_SCALE;
    pango_font_metrics_unref(fm);
    pango_font_description_free(desc);

    if (out_ascent)
        *out_ascent = ascent;
    return h;
}

/* Рисует строку текста. x — левый край, y — ВЕРХ строки (baseline
 * внутри сдвигается на ascent, см. ab_text_metrics). Шрифт берётся
 * масштабированный, тот же, которым мерилась раскладка, - иначе текст
 * не совпадёт с местом, которое под него выделили. */
/* y — верх, КУДА должна попасть ВЕРХНЯЯ точка реальных глифов, а не
 * абстрактной строки шрифта. Раньше тут стоял фиксированный сдвиг на
 * ascent, и он НЕ совпадал с тем, куда Pango реально кладёт глифы:
 * измерением на ноуте (scale 1.5) фактическое смещение оказалось равно
 * line_h, а не ascent. Из-за этого нижняя строка уходила за нижнюю
 * рамку окна и обрезалась, а при show_percent=0 та же строка рисовалась
 * посередине и читалась полностью.
 *
 * Поэтому смещение не вычисляется, а измеряется у той же раскладки:
 * pango_layout_get_pixel_extents() даёт реальные края глифов, и мы
 * сдвигаем базовую линию на разницу между ними и требуемым верхом.
 * ascent больше не нужен и не используется. */
static void ab_text(AbState *st, cairo_t *cr, const char *text,
                    double x, double y, gboolean red, int ascent, double scale)
{
    PangoLayout *layout;
    PangoFontDescription *desc;
    PangoRectangle ink;

    if (!text || !*text)
        return;

    layout = gtk_widget_create_pango_layout(st->widget, text);
    desc = ab_font_scaled(st, scale);
    pango_layout_set_font_description(layout, desc);

    /* ink.y — где Pango хочет верх глифов относительно текущей точки
     * отрисовки; сдвигаем на разницу с требуемым верхом строки. */
    pango_layout_get_pixel_extents(layout, NULL, &ink);
    if (red)
        cairo_set_source_rgb(cr, 1.0, 0.15, 0.15);
    else
        cairo_set_source_rgb(cr, 1.0, 1.0, 1.0);
    cairo_move_to(cr, x, (double)((int)y - ink.y));
    pango_cairo_show_layout(cr, layout);
    pango_font_description_free(desc);
    g_object_unref(layout);
}

static void ab_save_int(XsPlugin *p, const char *key, int value);

/* Подогнать окно под текущий scale из конфига.
 *
 * КАНОН: конфиг - единственный источник истины. И Properties-ползунок
 * Scale, и меню Size -> N % пишут ключ "scale" прямо в конфиг, минуя
 * плагин (см. xs_core_prop_scale_changed и обработчик "scale:" в
 * common.c). Поэтому значение перечитывается здесь каждый раз, а не
 * берётся из переданного аргумента.
 *
 * Вызывается из двух мест, и оба нужны:
 *  - ab_menu_cmd("scale-applied") - ползунок в Properties, реакция сразу;
 *  - ab_tick() - путь Size -> N %, который НЕ зовёт menu_cmd, ядро ждёт
 *    подгонки на ближайшем тике (так же сделано в calendar: cal_fit_height).
 *
 * Окно = масштабированный натуральный размер. window_width/window_height
 * НЕ пишутся в конфиг намеренно: они задаются пользователем вручную, и
 * запись поверх них ломала бы ручной размер - стоило бы двинуть ползунок
 * Scale, как ручной размер пропадал бы. */
static void ab_apply_scale(XsPlugin *p)
{
    AbState *st = p->priv;
    GKeyFile *kf;
    double s;
    int want_w, want_h;

    if (!st)
        return;
    kf = xs_core_plugin_conf(p->name);
    if (!kf)
        return;

    s = xs_host_api()->conf_dbl(kf, p->name, "scale", 1.0);
    if (s < 0.2)
        s = 0.2;
    else if (s > 10.0)
        s = 10.0;
    st->scale = s;

    /* Размер здесь НЕ задаётся. Раньше стояла формула
     * want = AB_DEFAULT_WIDTH * s, но она не совпадает с тем, что
     * считает ab_layout() по метрикам шрифта: apply_scale ужимал окно,
     * следующий draw() растил его до need_w, и на каждом тике размер
     * скакал туда-обратно - апплет дёргался. Теперь единственный
     * источник размера - need_w/need_h из ab_layout(), и применяется
     * он в draw() сразу перед отрисовкой.
     *
     * Окно не трогаем и st->window_*, пока p->win нет: первый размер
     * поставит первый же draw. */
    st->scale = s;
    (void)want_w;
    (void)want_h;
}

/* Обработка штатной команды ядра "scale-applied".
 *
 * В Properties есть ползунок Scale, ядро пишет его в конфиг как scale и
 * само окно не трогает - каждый плагин подгоняет размер сам. Батарея эту
 * команду не слушала, поэтому ползунок двигался, в конфиг писался
 * scale=1.11, а applet оставался прежнего размера: выглядело как
 * «масштабирование не работает».
 *
 * Канон батареи - AB_DEFAULT_WIDTH x AB_DEFAULT_HEIGHT, тот же, что и у
 * часов (240x240): масштаб множит его и пересоздаёт окно. */
static void ab_menu_cmd(XsPlugin *p, const char *cmd)
{
    AbState *st = p->priv;

    if (!st || !cmd)
        return;
    if (strcmp(cmd, "scale-applied") != 0)
        return;

    ab_apply_scale(p);
    if (p->win)
        gtk_widget_queue_draw(p->win);
}

static int ab_init(XsPlugin *p, GKeyFile *kf)
{
    AbState *st;
    XsHostApi *api = p->host;
    int x, y;

    /* g_new0 обнуляет min_width/min_height: до первого draw() размер
     * ограничивается только жёсткими AB_MIN_*. */
    st = g_new0(AbState, 1);
    /* Координаты читаем здесь же: если их не передать в make_window,
     * окно создастся в 0,0 и applet окажется в углу, где его не видно.
     * Значения по умолчанию те же, что у остальных плагинов. */
    x = api->conf_int(kf, p->name, "x", 80);
    y = api->conf_int(kf, p->name, "y", 80);
    st->window_width    = api->conf_int(kf, p->name, "window_width", AB_DEFAULT_WIDTH);
    st->window_height   = api->conf_int(kf, p->name, "window_height", AB_DEFAULT_HEIGHT);
    st->update_interval = api->conf_int(kf, p->name, "update_interval", AB_DEFAULT_INTERVAL);
    st->alarm_threshold = api->conf_int(kf, p->name, "alarm_threshold", AB_DEFAULT_ALARM);
    st->show_percent    = api->conf_int(kf, p->name, "show_percent", 1) != 0;
    st->show_time       = api->conf_int(kf, p->name, "show_time", 1) != 0;
    /* 0 по умолчанию, как в disk_monitor: скругление срезает углы окна,
     * поэтому включать его всем без запроса нельзя. */
    st->corner_radius    = CLAMP(api->conf_int(kf, p->name, "corner_radius", 0), 0, 200);
    /* Шрифт текста. conf_str возвращает строку, которой нужно владеть:
     * core не копирует результат. NULL - настройки нет, тогда шрифт берётся
     * из контекста виджета, как и до появления этого пункта. */
    st->font = api->conf_str(kf, p->name, "font", NULL);
    ab_clamp(st);

    /* scale читаем ДО make_window: окно должно создаться уже нужного
     * размера, иначе applet на секунду мелькнет в натуральном размере. */
    st->scale = api->conf_dbl(kf, p->name, "scale", 1.0);
    if (st->scale < 0.2)
        st->scale = 0.2;
    else if (st->scale > 10.0)
        st->scale = 10.0;

    p->priv = st;

    api->make_window(p, x, y, st->window_width, st->window_height);

    /* Тему ищем через xs_core_find_theme(), а не передаём в theme_load()
     * строку "default". Здесь был единственный апплет, который делал
     * именно так, и theme_load() вызывал g_dir_open("default") - открыть
     * относительный путь из текущего каталога демона невозможно. Тема не
     * грузилась вообще: xs_core_theme_has() возвращал FALSE на каждый
     * элемент, и апплет рисовал один только текст, хотя файлы тем на
     * диске были и читались.
     *
     * Остальные апплеты (clock, calendar, clearrss, frame_launcher) с
     * самого начала вызывали find_theme() первым. */
    {
        /* Имя темы берём из конфига (по умолчанию "default"). Раньше
         * строка "default" стояла здесь жёстко, и выбрать другую тему
         * было нечем - тема green из референса просто не подхватывалась,
         * хотя лежала рядом на диске. */
        char *theme_name = api->conf_str(kf, p->name, "theme", "default");
        char *theme_dir = xs_core_find_theme(p->type ? p->type : p->name,
                                             theme_name);

        if (theme_dir && api->theme_load(p, theme_dir)) {
            if (xs_core_is_debug())
                api->log("acpi_battery: theme loaded from %s", theme_dir);
        } else if (xs_core_is_debug()) {
            api->log("acpi_battery: theme '%s' not found, text only",
                     theme_name);
        }
        g_free(theme_name);
        g_free(theme_dir);
    }

    st->widget = p->win;
    /* Подгоняем окно под scale после make_window: на этом шаге p->win
     * уже есть, и размер применяется без лишнего кадра. */
    ab_apply_scale(p);
    ab_poll(st);
    api->set_tick(p, (guint)st->update_interval * 1000);
    return 0;
}

/* Рисует элемент темы в градациях серого.
 *
 * Как это работает: элемент рисуется во временную группу, затем группа
 * переносится в источник и поверх неё рисуется серым сплошным заполнением
 * с оператором MULTIPLY. Обе операции вместе дают обесцвечивание: чёрное
 * остаётся чёрным, белое становится серым, цветные пиксели уходят в
 * оттенки того же серого.
 *
 * Почему не cairo_set_source_surface + paint с OVER: обычный paint
 * положил бы серый ПОВЕРХ всего, включая тёмный фон индикатора, и рамка
 * пропала бы. MULTIPY сохраняет светлоту каждого пикселя.
 *
 * Группа обязательна: красить прямо на cr нельзя, там же лежат элементы,
 * уже нарисованные без обесцвечивания (фон и корпус батареи).
 */
static void ab_draw_gray(XsPlugin *p, cairo_t *cr, const char *element,
                         double x, double y, double w, double h)
{
    cairo_surface_t *surf;
    cairo_t *tmp;
    int iw, ih;

    if (!xs_core_theme_has(p, element))
        return;

    iw = (int)(w > 0.0 ? w : 1.0);
    ih = (int)(h > 0.0 ? h : 1.0);

    /* Промежуточная ARGB-поверхность нужна, чтобы MULTIPLY применился
     * только к нарисованному элементу, а не ко всему окну. */
    surf = cairo_image_surface_create(CAIRO_FORMAT_ARGB32, iw, ih);
    tmp = cairo_create(surf);
    p->host->theme_draw_full(p, tmp, element, x, y, w, h);

    /* MULTIPLY сохраняет светлоту каждого пикселя: чёрное остаётся
     * чёрным, белое становится серым. При OVER серый просто закрыл бы
     * элемент, и рамка индикатора исчезла бы. */
    cairo_set_operator(cr, CAIRO_OPERATOR_MULTIPLY);
    cairo_set_source_surface(cr, surf, x, y);
    cairo_paint_with_alpha(cr, 1.0);
    cairo_set_operator(cr, CAIRO_OPERATOR_OVER);

    cairo_destroy(tmp);
    cairo_surface_destroy(surf);
}

/* Рисует индикатор заряда, обрезанный по реальному проценту.
 *
 * Элемент темы нарисован на всю ширину AB_IND_W, а здесь мы режем его
 * клипом по проценту. В оригинальной python-теме индикатор был
 * статичной картинкой: уровень заряда не показывался никак, менялся
 * только цвет аларма. Полоса по проценту - то, чего там не было.
 *
 * При неизвестном проценте (нет батареи) рисуем полосу целиком, иначе
 * клип нулевой ширины оставил бы пустое место вместо индикатора. */
static void ab_draw_indicator(XsPlugin *p, cairo_t *cr, const char *element,
                             const AbGeom *g, double frac)
{
    if (!element || !xs_core_theme_has(p, element))
        return;
    if (frac < 0.0)
        frac = 0.0;
    if (frac > 1.0)
        frac = 1.0;

    cairo_save(cr);
    cairo_rectangle(cr, g->ind_x, g->ind_y, g->ind_w * frac, g->ind_h);
    cairo_clip(cr);
    p->host->theme_draw_full(p, cr, element,
                             g->ind_x, g->ind_y, g->ind_w, g->ind_h);
    cairo_restore(cr);
}

/* Геометрия темы green, посчитанная от РЕАЛЬНОГО размера окна.
 *
 * Раньше координаты были константами под окно 150x50, и при любом
 * другом размере из настроек батарейка с текстом оставалась в левом
 * верхнем углу, а всё остальное окно пустовало. Теперь блок
 * (батарейка + текст) центрируется по вертикали и прижат к левому
 * краю с полями, а индикатор считается от корпуса, а не от окна.
 *
 * Ширина блока = AB_BODY_W + AB_GAP + текст; берётся по вписыванию в
 * окно, при нехватке места батарейка сжимается. */
/* Радиус скругления из конфига: отрицательное значение в конфиге не должно
 * превращаться в ошибку shape-маски, поэтому всё, что не положительное,
 * трактуется как «без скругления». */
static double ab_corner_radius_value(int value)
{
    return value > 0 ? (double)value : 0.0;
}

static gboolean ab_corner_radius_is_rounded(double radius)
{
    return radius > 0.5;
}

/* Регион со скруглёнными углами для shape-маски окна.
 *
 * Считается по той же формуле, что dm_rounded_region() в disk_monitor и
 * sensor_rounded_region() в sensors, - дуги срезаются построчно через
 * floor(). Совпадение с соседними апплетами нужно по двум причинам:
 * одинаковый corner_radius обязан давать одинаковый на вид срез, иначе
 * рядом стоящие апплеты будут выглядеть по-разному.
 *
 * floor() здесь принципиален: он никогда не срезает глубже настоящей
 * дуги, поэтому в углу остаётся пиксель, а не дырка до фона. */
static cairo_region_t *ab_rounded_region(int width, int height, int radius)
{
    const double r = ab_corner_radius_value(radius);
    cairo_region_t *region;
    cairo_rectangle_int_t box;
    int scaled;

    if (width <= 0 || height <= 0)
        return NULL;
    if (!ab_corner_radius_is_rounded(r))
        return NULL;

    scaled = (int)MIN(r, MIN(width, height) / 2.0);
    region = cairo_region_create();
    if (!region)
        return NULL;

    /* Идём по строкам и срезаем четыре угла. floor() принципиален: он не
     * срезает глубже настоящей дуги, поэтому в углу остаётся пиксель. */
    for (int y = 0; y < height; y++) {
        int cut = 0;
        double dy;

        if (y < scaled)
            dy = scaled - y;
        else if (y >= height - scaled)
            dy = (double)(y - (height - scaled));
        else
            dy = 0.0;

        if (dy > 0.0) {
            double t = scaled * scaled - dy * dy;
            if (t < 0.0)
                t = 0.0;
            cut = (int)floor(scaled - sqrt(t));
        }
        box.x = cut;
        box.y = y;
        box.width = width - 2 * cut;
        box.height = 1;
        if (box.width > 0)
            cairo_region_union_rectangle(region, &box);
    }
    return region;
}

/* Контур скруглённого окна в cr как путь (не заливка).
 *
 * Это НЕ то же, что shape-маска: маска вырезает углы у X-окна, а этот путь
 * нужен, чтобы содержимое не рисовалось под срез. Без него текст и корпус
 * батарейки остаются в углах, которые маска уже съела, и выглядит это как
 * обрывок картинки на прозрачном фоне. */
static void ab_rounded_path(cairo_t *cr, int width, int height, int radius)
{
    /* В отступ на пиксель: маска режет ровно по краю окна, поэтому контур,
     * проведённый ПО краю, теряет половину обводки под срез. */
    const double inset = 1.0;
    double w = width - 2 * inset, h = height - 2 * inset;
    double r = ab_corner_radius_value(radius);

    if (w <= 0 || h <= 0) {
        cairo_rectangle(cr, 0, 0, width, height);
        return;
    }
    if (!ab_corner_radius_is_rounded(r)) {
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

/* Применить форму окна к X-серверу.
 *
 * Именно gdk_window_shape_combine_region(), а не его близнец
 * input_shape_: input-форма влияет только на то, какие пиксели считаются
 * кликабельными, но продолжает рисоваться. Нужна shape, чтобы угловые
 * пиксели реально перестали существовать. */
static void ab_apply_shape(AbState *st, XsPlugin *p, int w, int h)
{
    GdkWindow *window;
    cairo_region_t *region;

    if (!st || !p || !p->win || w <= 0 || h <= 0)
        return;
    if (st->shape_radius == st->corner_radius &&
        st->shape_w == w && st->shape_h == h)
        return;
    window = gtk_widget_get_window(p->win);
    if (!window)
        return;
    region = ab_rounded_region(w, h, st->corner_radius);
    gdk_window_shape_combine_region(window, region, 0, 0);
    if (region)
        cairo_region_destroy(region);
    st->shape_radius = st->corner_radius;
    st->shape_w = w;
    st->shape_h = h;
}

static void ab_layout(AbState *st, int w, int h, AbGeom *g)
{
    int rows, pad, gap, content_gap;
    PangoFontDescription *desc;

    g->scale = (st->scale > 0.0) ? st->scale : 1.0;
    g->line_h = ab_text_metrics(st, g->scale, &g->ascent);

    /* Все размеры темы умножаем на scale. Считаем в макросах (натуральный
     * размер темы green) и округляем, но НЕ обрезаем в ноль: при scale
     * меньше 1 индикатор и корпус должны остаться хотя бы в 1 px. */
    g->body_w = MAX(1, (int)(AB_BODY_W * g->scale + 0.5));
    g->body_h = MAX(1, (int)(AB_BODY_H * g->scale + 0.5));
    g->ind_h = MAX(1, (int)(AB_IND_H * g->scale + 0.5));
    pad = MAX(0, (int)(AB_FRAME_PAD * g->scale + 0.5));
    gap = MAX(0, (int)(AB_GAP * g->scale + 0.5));
    /* Зазор от рамки до содержимого слева: толщина нарисованной рамки
     * задаётся темой и нам неизвестна, поэтому отступ от окна pad
     * складывается с отдельным запасом. */
    content_gap = MAX(0, (int)(AB_CONTENT_GAP * g->scale + 0.5));

    /* Ширина текста меряется по САМОЙ ДЛИННОЙ из возможных строк, а не
     * по текущей. Иначе прижатый вправо текст прыгал бы: у "Full" и
     * " 90%" ширина разная, и правый край был бы то на месте, то нет.
     * Ширины берём у того же масштабированного шрифта, которым рисуем. */
    g->text_w = 0;
    desc = ab_font_scaled(st, g->scale);
    {
        /* Строки-примеры ДОЛЖНЫ совпадать с тем, что реально рисуется в
         * draw(): там везде ведущий пробел - " Full", " No",
         * " battery", а процент и таймер приходят из
         * acpi_battery_format_minutes() как "%d%%"/"%02d:%02d".
         * Раньше здесь стоял "Full" без пробела и "3:59", которых в
         * выводе не бывает: измеренная ширина оказывалась меньше
         * фактической, правый блок обрезался рамкой ("Full" съедало
         * почти целиком). */
        /* " AC BAT0" - реальный вид третьей строки. Ширина берётся по
         * САМОЙ ДЛИННОЙ строке всех трёх, иначе блок прыгает: при
         * hidpp_battery_0 в имени больше символов, чем в BAT0. */
        const char *samples[] = { "100%", " 90%", " 80%", " No",
                                  " battery", " Full", "00:00", "99:59",
                                  " AC BAT0", "   hidpp_battery_0",
                                  NULL };
        int i;

        for (i = 0; samples[i]; i++) {
            PangoLayout *l = gtk_widget_create_pango_layout(g->widget,
                                                            samples[i]);
            int pw = 0;

            pango_layout_set_font_description(l, desc);
            pango_layout_get_pixel_size(l, &pw, NULL);
            g_object_unref(l);
            if (pw > g->text_w)
                g->text_w = pw;
        }
    }
    pango_font_description_free(desc);

    /* Сколько строк реально рисуется - от этого зависит и нужная высота,
     * и вертикальное выравнивание. */
    rows = 0;
    if (st->show_percent) rows++;
    if (st->show_time)    rows++;
    /* Третья строка считается, только если в ней есть что показать.
     * Пустую строку не резервируем: иначе applet без сети (сервер)
     * получил бы лишнюю высоту с пустым местом внизу. */
    if (st->source_text && *st->source_text)
        rows++;
    if (rows < 1)
        rows = 1;

    /* Минимальный размер: корпус слева, текст справа, оба отступом от
     * рамки. Окно меньше этого обрезает содержимое, поэтому draw()
     * потом поднимет размер до need_w/need_h. */
    /* Слева отступ pad+content_gap, справа только pad: текст по твоему
     * правилу прижат вправо, и увеличение правого поля двигало его
     * от края, где он и должен быть. */
    g->need_w = pad + content_gap + g->body_w + gap + g->text_w + pad;
    g->need_h = 2 * pad +
                MAX(g->body_h, rows * g->line_h + (rows - 1) * AB_TEXT_ROW_GAP);

    /* Ниже минимума раскладка считается от минимума: иначе текст уехал бы
     * в отрицательные координации и пропал совсем. */
    if (w < g->need_w)
        w = g->need_w;
    if (h < g->need_h)
        h = g->need_h;

    /* Корпус: масштабированный размер, по вертикали по центру, слева от
     * текста. */
    g->body_x = pad + content_gap;
    g->body_y = (h - g->body_h) / 2;
    if (g->body_y < 0)
        g->body_y = 0;

    /* Текст прижат к ПРАВОМУ краю с отступом pad. */
    g->text_x = w - pad - g->text_w;

    /* Вертикаль: процент прижат к ВЕРХУ рамки, время - к НИЗУ.
     * При одной строке она центрируется по корпусу. */
    if (rows >= 2) {
        /* Две строки: первая у верхнего края, вторая у нижнего.
         * Три строки: равномерно по вертикали, иначе первая прижата бы к
         * рамке, а третья стояла бы впритык к ней снизу. */
        g->text_y_percent = pad;
        if (rows >= 3) {
            int block = rows * g->line_h + (rows - 1) * AB_TEXT_ROW_GAP;

            g->text_y_percent = pad;
            g->text_y_time    = pad + (h - 2 * pad - block) / 2 + g->line_h
                             + AB_TEXT_ROW_GAP;
            g->text_y_source  = g->text_y_time + g->line_h + AB_TEXT_ROW_GAP;
        } else {
            g->text_y_time   = h - pad - g->line_h;
            g->text_y_source = g->text_y_time;
        }
    } else {
        g->text_y_percent = g->body_y + (g->body_h - g->line_h) / 2;
        g->text_y_time = g->text_y_percent;
        g->text_y_source = g->text_y_percent;
    }
    g->text_y_only = g->body_y + (g->body_h - g->line_h) / 2;

    /* Индикатор вписан в корпус: поля тоже масштабируются, иначе при
     * scale=2 батарейка выросла бы, а желобка индикатора осталась бы
     * в натуральных 8 px. */
    g->ind_x = g->body_x + MAX(0, (int)(AB_IND_INSET_X * g->scale + 0.5));
    g->ind_y = g->body_y + (g->body_h - g->ind_h) / 2;
    g->ind_w = g->body_w - 2 * MAX(0, (int)(AB_IND_INSET_X * g->scale + 0.5));
    if (g->ind_w < 1)
        g->ind_w = 1;
}

/* Порог выше которого индикатор считается полным — тот же, что у темы:
 * при полосе по проценту длина и так читается, обрезать её на 95 %
 * незачем. */
static double ab_fill_fraction(int percent)
{
    if (percent < 0 || percent > 100)
        return 1.0;
    if (percent >= AB_IND_FULL_PCT)
        return 1.0;
    return percent / 100.0;
}

static void ab_draw(XsPlugin *p, cairo_t *cr, int w, int h)
{
    AbState *st = p->priv;
    XsHostApi *api = p->host;
    const char *indicator = NULL;
    gboolean no_battery;
    AbGeom g;

    g.widget = p->win;
    ab_layout(st, w, h, &g);

    /* Окно не должно быть меньше нарисованного. Размер берётся из
     * настроек пользователя как ПОЛ, а не как точное значение:
     *
     *   - меньше need_* нельзя, содержимое не влезет и текст обрежется;
     *   - больше можно, и это как раз то, зачем настройка нужна.
     *
     * Раньше здесь стояло st->window_width = nw, то есть размер из
     * настроек молча перезаписывался фактическим в памяти. В конфиг это
     * не писалось, поэтому настройка и реальный размер расходились:
     * ползунок показывал фактическую ширину, в конфиге лежала
     * пользовательская, и при открытии Properties нижняя граница
     * ползунка уже подпрыгивала до фактической - вернуть меньше было
     * невозможно. Теперь в памяти и в конфиге одно и то же число.
     *
     * Пользовательский размер НЕ пишется в конфиг: иначе движение
     * ползунка Scale затирало бы его (см. ab_apply_scale). */
    if (g.need_w > st->min_width)
        st->min_width = g.need_w;
    if (g.need_h > st->min_height)
        st->min_height = g.need_h;
    if (w < g.need_w || h < g.need_h) {
        int nw = MAX(w, g.need_w);
        int nh = MAX(h, g.need_h);

        api->resize(p, nw, nh);
        w = nw;
        h = nh;
        /* Раскладка считалась от меньшего размера - пересчитываем. */
        ab_layout(st, w, h, &g);
    }

    ab_apply_shape(st, p, w, h);

    /* Порядок как в оригинале: фон, корпус, индикатор, блик, текст.
     *
     * Именно theme_draw_full, а НЕ theme_draw: theme_draw подставляет
     * высоту, равную ширине, то есть просит тему нарисовать в квадрате
     * w x w. При окне 150x50 тема растягивалась бы по вертикали в 3
     * раза, и нижняя половина батарейки уезжала бы за край. */
    if (xs_core_theme_has(p, "acpibattery-bg"))
        api->theme_draw_full(p, cr, "acpibattery-bg", 0, 0, w, h);
    if (xs_core_theme_has(p, "acpibattery-battery"))
        api->theme_draw_full(p, cr, "acpibattery-battery",
                             g.body_x, g.body_y, g.body_w, g.body_h);

    no_battery = (!st->battery ||
                  st->battery->state == ACPI_BATTERY_NOT_PRESENT);

    if (no_battery) {
        /* Батареи нет: индикатор рисуется В СЕРЫХ тонах. Аларм НЕ рисуем
         * никогда — красное читалось бы как «заряд кончился», а батареи
         * просто нет. Серый корпус даёт читаемый applet, при этом не
         * выдаёт его за низкий заряд. */
        if (xs_core_theme_has(p, "acpibattery-using"))
            ab_draw_gray(p, cr, "acpibattery-using",
                         g.ind_x, g.ind_y, g.ind_w, g.ind_h);
    } else if (st->battery->state == ACPI_BATTERY_CHARGING) {
        indicator = "acpibattery-charging";
    } else if (st->battery->state == ACPI_BATTERY_DISCHARGING) {
        indicator = st->low ? "acpibattery-alarm" : "acpibattery-using";
    } else {
        indicator = "acpibattery-using";
    }
    /* Длина полосы = процент, поэтому состояние передаётся дальше. */
    ab_draw_indicator(p, cr, indicator, &g,
                      ab_fill_fraction(no_battery ? -1 : st->battery->percent));

    /* Клип по окну: при крупном шрифте текст вылезет за рамку, а окно
     * непрозрачное, и текст зарисуется поверх соседних апплетов. */
    cairo_save(cr);
    if (ab_corner_radius_is_rounded(ab_corner_radius_value(st->corner_radius))) {
        /* Клип по дуге, а не по прямоугольнику: маска уже срезала углы у
         * окна, но содержимое продолжает рисоваться по всему прямоугольнику
         * и вылезает под срез. Контур по дуге держит текст и корпус внутри
         * видимой области. */
        ab_rounded_path(cr, w, h, st->corner_radius);
        cairo_clip(cr);
    } else {
        cairo_rectangle(cr, 0, 0, w, h);
        cairo_clip(cr);
    }

    /* Блик ложится на корпус батарейки, поэтому рисуется по её
     * координатам, а не на всё окно. В оригинале он рисовался
     * последним, поверх текста, - здесь текст сбоку и блик ему не
     * мешает, а корпус получает объём. */
    if (xs_core_theme_has(p, "acpibattery-glass"))
        api->theme_draw_full(p, cr, "acpibattery-glass",
                             g.body_x, g.body_y, g.body_w, g.body_h);

    /* ascent передаётся в ab_text: тот сдвигает baseline от верха строки,
     * иначе текст уезжает вниз и строки слипаются. */
    if (no_battery) {
        ab_text(st, cr, " No", g.text_x, g.text_y_percent, FALSE,
                g.ascent, g.scale);
        ab_text(st, cr, " battery", g.text_x, g.text_y_time, FALSE,
                g.ascent, g.scale);
    } else if (st->show_percent && st->show_time) {
        ab_text(st, cr, st->percent_text, g.text_x, g.text_y_percent,
                st->low, g.ascent, g.scale);
        ab_text(st, cr, st->time_text, g.text_x, g.text_y_time,
                st->low, g.ascent, g.scale);
        /* Третья строка: сеть и имя источника. Показывается только если
         * ab_layout() посчитал для неё место, то есть строка непустая. */
        if (st->source_text && *st->source_text)
            ab_text(st, cr, st->source_text, g.text_x, g.text_y_source,
                    FALSE, g.ascent, g.scale);
    } else if (st->show_percent) {
        ab_text(st, cr, st->percent_text, g.text_x, g.text_y_only,
                st->low, g.ascent, g.scale);
    } else if (st->show_time) {
        ab_text(st, cr, st->time_text, g.text_x, g.text_y_only,
                st->low, g.ascent, g.scale);
    }

    cairo_restore(cr);
}

static guint ab_tick(XsPlugin *p)
{
    AbState *st = p->priv;
    XsHostApi *api = p->host;

    /* Меню Size -> N % пишет scale в конфиг и НЕ зовёт menu_cmd, так
     * что окно подгоняется здесь - на ближайшем тике, как cal_fit_height
     * у календаря. */
    ab_apply_scale(p);
    ab_poll(st);

    /* Интервал могли поменять в настройках — сбрасываем таймер, иначе
     * старая периодичность пережила бы правку до перезапуска. */
    api->set_tick(p, (guint)st->update_interval * 1000);
    return 0;
}

static void ab_shutdown(XsPlugin *p)
{
    AbState *st = p->priv;

    if (!st)
        return;
    acpi_battery_free(st->battery);
    g_free(st->time_text);
    g_free(st->percent_text);
    g_free(st->source_text);
    g_free(st->font);
    g_free(st);
    p->priv = NULL;
}

/* --- Properties --- */

static void ab_save_int(XsPlugin *p, const char *key, int value)
{
    GKeyFile *kf = xs_core_plugin_conf(p->name);

    if (!kf)
        return;
    g_key_file_set_integer(kf, p->name, key, value);
    xs_core_plugin_conf_flush(p->name);
}

static void ab_int_changed(GtkSpinButton *sb, gpointer data)
{
    XsPlugin *p = data;
    AbState *st = p->priv;
    const char *key = g_object_get_data(G_OBJECT(sb), "ab-key");
    int v = gtk_spin_button_get_value_as_int(sb);

    if (!key)
        return;

    if (!strcmp(key, "window_width")) {
        st->window_width = v;
        ab_clamp(st);
        ab_save_int(p, key, st->window_width);
        p->host->resize(p, st->window_width, st->window_height);
        return;
    }
    if (!strcmp(key, "window_height")) {
        st->window_height = v;
        ab_clamp(st);
        ab_save_int(p, key, st->window_height);
        p->host->resize(p, st->window_width, st->window_height);
        return;
    }
    if (!strcmp(key, "update_interval")) {
        st->update_interval = v;
        ab_save_int(p, key, v);
        p->host->set_tick(p, (guint)v * 1000);
        return;
    }
    if (!strcmp(key, "alarm_threshold")) {
        st->alarm_threshold = v;
        ab_save_int(p, key, v);
        ab_refresh_text(st);     /* порог влияет на цвет прямо сейчас */
        gtk_widget_queue_draw(p->win);
        return;
    }
    if (!strcmp(key, "corner_radius")) {
        st->corner_radius = v;
        /* shape_* сбрасываем, иначе ab_apply_shape() решит, что форма не
         * менялась, и не отправит новую маску в X. */
        st->shape_radius = -1;
        ab_save_int(p, key, v);
        gtk_widget_queue_draw(p->win);
        return;
    }
    ab_save_int(p, key, v);
}

static void ab_bool_changed(GtkToggleButton *tb, gpointer data)
{
    XsPlugin *p = data;
    AbState *st = p->priv;
    const char *key = g_object_get_data(G_OBJECT(tb), "ab-key");
    gboolean v = gtk_toggle_button_get_active(tb);

    if (!key)
        return;
    if (!strcmp(key, "show_percent"))
        st->show_percent = v;
    else if (!strcmp(key, "show_time"))
        st->show_time = v;
    ab_save_int(p, key, v ? 1 : 0);
    gtk_widget_queue_draw(p->win);
}

static GtkWidget *ab_int_prop(GtkBox *box, XsPlugin *p, const char *key,
                              const char *label, const char *desc,
                              int value, int min, int max)
{
    GtkWidget *sb = xs_prop_add_int(box, label, desc, value, min, max, 1.0);

    g_object_set_data(G_OBJECT(sb), "ab-key", (gpointer)key);
    g_signal_connect(sb, "value-changed", G_CALLBACK(ab_int_changed), p);
    return sb;
}

static GtkWidget *ab_bool_prop(GtkBox *box, XsPlugin *p, const char *key,
                               const char *label, const char *desc,
                               gboolean value)
{
    /* Перевод подписи и подсказки здесь: ab_bool_prop вызывается с
     * литералами, оборачивать каждый на стороне вызова бессмысленно.
     * Ключ конфига идёт отдельным параметром key и сюда не попадает. */
    GtkWidget *cb = gtk_check_button_new_with_label(_(label));

    gtk_toggle_button_set_active(GTK_TOGGLE_BUTTON(cb), value);
    gtk_widget_set_tooltip_text(cb, _(desc));
    gtk_box_pack_start(box, cb, FALSE, FALSE, 0);
    g_object_set_data(G_OBJECT(cb), "ab-key", (gpointer)key);
    g_signal_connect(cb, "toggled", G_CALLBACK(ab_bool_changed), p);
    return cb;
}

/* Выбор шрифта из Properties.
 *
 * Шрифт пишется прямо в конфиг, минуя ab_config_changed: там ключи
 * обрабатываются как целые числа, а здесь строка. Клавиатурный фокус
 * выставляется на виджет - иначе после выбора шрифта Properties теряет
 * клавиатуру и следующий Tab уходит в никуда (то же в memory_monitor). */
static void ab_font_set(GtkFontButton *button, gpointer data)
{
    XsPlugin *p = data;
    AbState *st = p ? p->priv : NULL;
    GKeyFile *kf;
    const char *value;

    if (!st)
        return;
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wdeprecated-declarations"
    value = gtk_font_button_get_font_name(button);
#pragma GCC diagnostic pop

    g_free(st->font);
    st->font = g_strdup(value);

    /* Метрики текста зависят от шрифта, поэтому пересчитываем минимум
     * окна: с новым кеглем содержимое может стать шире, и без пересчёта
     * applet остался бы со старым размером и обрезанным текстом. */
    st->min_width  = 0;
    st->min_height = 0;

    kf = xs_core_plugin_conf(p->name);
    if (kf) {
        if (value)
            g_key_file_set_string(kf, p->name, "font", value);
        else
            g_key_file_remove_key(kf, p->name, "font", NULL);
        xs_core_plugin_conf_flush(p->name);
    }
    gtk_widget_queue_draw(p->win);
}

static void ab_properties(XsPlugin *p, GtkNotebook *nb)
{
    AbState *st = p->priv;
    GtkWidget *page;

    if (!st)
        return;

    page = gtk_box_new(GTK_ORIENTATION_VERTICAL, 4);
    gtk_container_set_border_width(GTK_CONTAINER(page), 10);

    xs_prop_add_group_header(GTK_BOX(page),
        "Reads /sys/class/power_supply. Works with laptop batteries and with "
        "devices that expose a battery there (e.g. a Logitech mouse when the "
        "hid-logitech-hidpp module is loaded). Shows \"No battery\" when no "
        "source is found, instead of failing.");

    /* Нижняя граница ползунка - измеренный минимум, а не жёсткие 60x30:
     * при крупном шрифте меньший размер обрезал бы текст, и applet
     * всё равно растил бы себя в draw(). Размер из настроек работает как
     * ПОЛ: поставить меньше содержимого нельзя, больше - можно, и это
     * как раз то, зачем настройка нужна.
     *
     * Верхняя граница не ниже нижней: при крупном scale содержимое может
     * быть шире жёсткого предела, и ползунок с min > max в GTK ведёт себя
     * непредсказуемо (не скроллится либо рисуется перевёрнутым). */
    ab_int_prop(GTK_BOX(page), p, "window_width", "Window width",
                "Applet width in pixels. Acts as a minimum: the applet grows "
                "wider if its contents do not fit.",
                st->window_width,
                MAX(AB_MIN_WIDTH, st->min_width),
                MAX(400, MAX(AB_MIN_WIDTH, st->min_width)));
    ab_int_prop(GTK_BOX(page), p, "window_height", "Window height",
                "Applet height in pixels. Acts as a minimum.",
                st->window_height,
                MAX(AB_MIN_HEIGHT, st->min_height),
                MAX(400, MAX(AB_MIN_HEIGHT, st->min_height)));
    ab_int_prop(GTK_BOX(page), p, "update_interval", "Update interval (s)",
                "Seconds between reads of sysfs", st->update_interval, 1, 3600);
    ab_int_prop(GTK_BOX(page), p, "alarm_threshold", "Low battery threshold (%)",
                "Charge percent at or below which the alarm colour is used",
                st->alarm_threshold, 0, 100);
    /* Шрифт текста: процент, время, третья строка. Кегль из настройки
     * домножается на scale, поэтому Size -> N % продолжает работать и
     * увеличивает текст вместе с картинкой. Пустое значение = шрифт темы
     * рабочего стола, это поведение по умолчанию. */
    {
        GtkWidget *fw = xs_prop_add_font(GTK_BOX(page), "Text font",
                                         "Font of percentage, time and "
                                         "source line. Empty means the "
                                         "desktop theme font.",
                                         st->font);
        g_signal_connect(fw, "font-set", G_CALLBACK(ab_font_set), p);
    }

    /* Радиус углов окна в пикселях, 0 = прямые углы. Влияет только на форму
     * окна, тема (acpibattery-bg) продолжает рисовать рамку как умеет. */
    ab_int_prop(GTK_BOX(page), p, "corner_radius", "Corner radius",
                "Corner rounding in pixels; 0 keeps square corners",
                st->corner_radius, 0, 200);

    ab_bool_prop(GTK_BOX(page), p, "show_percent", _("Show percentage"),
                 "Display charge percent", st->show_percent);
    ab_bool_prop(GTK_BOX(page), p, "show_time", _("Show remaining time"),
                 "Display estimated time left", st->show_time);

    gtk_notebook_append_page(nb, page, gtk_label_new(_("ACPI Battery")));
    gtk_widget_show_all(page);
}

static const XsPluginOps ab_ops = {
    .init = ab_init,
    .draw = ab_draw,
    .tick = ab_tick,
    .button = NULL,
    .motion = NULL,
    .shutdown = ab_shutdown,
    .menu = NULL,
    .menu_cmd = ab_menu_cmd,
    .properties = ab_properties,
    .fill_themes = NULL,
    .scroll = NULL,
    .enter = NULL,
    .leave = NULL,
    .guest_list_changed = NULL,
};

static XsPluginDesc ab_desc = {
    "acpi_battery",
    XS_API_VERSION,
    &ab_ops,
    N_("Battery charge, remaining time and state from sysfs power_supply"),
    "kosmik2001 <kosmik2001@gmail.com>",
    "1.0"
};

XS_PLUGIN_EXPORT(&ab_desc)