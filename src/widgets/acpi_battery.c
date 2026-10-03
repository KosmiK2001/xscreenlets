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
    int body_x, body_y;
    /* Текст прижат по ПРАВОМУ краю: text_x считается от w - ширина. */
    int text_x, text_y_percent, text_y_time, text_y_only;
    int ind_x, ind_y, ind_w;
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

    /* Минимальный размер, который реально вмещает содержимое. Считается
     * по метрикам шрифта в draw(), потому что раньше был жёсткий
     * AB_MIN_WIDTH/AB_MIN_HEIGHT: при крупном шрифте окно можно было
     * сделать меньше нарисованного, и текст обрезался. */
    int min_width;
    int min_height;
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

/* Пересчитать тексты из прочитанного состояния. */
static void ab_refresh_text(AbState *st)
{
    const AcpiBattery *bat = st->battery;

    ab_set_text(&st->percent_text, NULL);
    ab_set_text(&st->time_text, NULL);
    st->low = FALSE;

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
/* Высота строки и её ascent для текущего шрифта.
 *
 * Считается через Pango. Жёсткая константа AB_TEXT_LINE_H была не
 * только неверной по величине, но и стояла не с того края: ab_text()
 * передавал в cairo_move_to() ВЕРХ строки, а pango_cairo_show_layout()
 * позиционирует от BASELINE. Текст уезжал вниз на ascent, и строки
 * наезжали друг на друга - это и было «странное» позиционирование. */
static int ab_text_metrics(cairo_t *cr, int *out_ascent)
{
    PangoContext *pc = pango_cairo_create_context(cr);
    PangoFontDescription *desc;
    PangoFontMetrics *fm;
    int h, ascent;

    /* Метрики берём у шрифта ИМЕННО ЭТОГО контекста (того же, которым
     * рисуем), иначе ascent не совпадёт с реально отрисованным текстом
     * и раскладка снова уедет. Описание шрифта и язык - как у того
     * контекста. */
    desc = pango_context_get_font_description(pc);
    fm = pango_context_get_metrics(pc, desc, pango_language_get_default());
    ascent = pango_font_metrics_get_ascent(fm) / PANGO_SCALE;
    h = (pango_font_metrics_get_ascent(fm) +
         pango_font_metrics_get_descent(fm)) / PANGO_SCALE;
    pango_font_metrics_unref(fm);

    if (out_ascent)
        *out_ascent = ascent;
    return h;
}

/* Рисует строку текста. x — левый край, y — ВЕРХ строки (baseline
 * внутри сдвигается на ascent, см. ab_text_metrics). */
static void ab_text(AbState *st, cairo_t *cr, const char *text,
                    double x, double y, gboolean red, int ascent)
{
    PangoLayout *layout;

    if (!text || !*text)
        return;

    cairo_move_to(cr, x, y + ascent);
    if (red)
        cairo_set_source_rgb(cr, 1.0, 0.15, 0.15);
    else
        cairo_set_source_rgb(cr, 1.0, 1.0, 1.0);

    layout = gtk_widget_create_pango_layout(st->widget, text);
    pango_cairo_show_layout(cr, layout);
    g_object_unref(layout);
}

static void ab_save_int(XsPlugin *p, const char *key, int value);

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
    GKeyFile *kf;
    double s;

    if (!st || !cmd)
        return;
    if (strcmp(cmd, "scale-applied") != 0)
        return;

    kf = xs_core_plugin_conf(p->name);
    if (!kf)
        return;
    s = xs_host_api()->conf_dbl(kf, p->name, "scale", 1.0);
    if (s < 0.2)
        s = 0.2;
    else if (s > 10.0)
        s = 10.0;

    st->window_width  = (int)(AB_DEFAULT_WIDTH * s);
    st->window_height = (int)(AB_DEFAULT_HEIGHT * s);
    ab_clamp(st);
    /* Ширину/высоту пишем в конфиг, иначе после рестарта демона размер
     * вернётся к дефолту и ползунок перестанет иметь смысл. */
    ab_save_int(p, "window_width", st->window_width);
    ab_save_int(p, "window_height", st->window_height);

    if (p->win) {
        p->host->resize(p, st->window_width, st->window_height);
        gtk_widget_queue_draw(p->win);
    }
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
    ab_clamp(st);

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
    cairo_rectangle(cr, g->ind_x, g->ind_y, g->ind_w * frac, AB_IND_H);
    cairo_clip(cr);
    p->host->theme_draw_full(p, cr, element,
                             g->ind_x, g->ind_y, g->ind_w, AB_IND_H);
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
static void ab_layout(AbState *st, int w, int h, cairo_t *cr, AbGeom *g)
{
    int rows;

    g->line_h = ab_text_metrics(cr, &g->ascent);

    /* Ширина текста меряется по САМОЙ ДЛИННОЙ из возможных строк, а не
     * по текущей. Иначе прижатый вправо текст прыгал бы: у "Full" и
     * " 90%" ширина разная, и правый край был бы то на месте, то нет.
     * Ширины берём у реального шрифта через Pango. */
    g->text_w = 0;
    {
        const char *samples[] = { "100%", " 90%", "00:00", "3:59",
                                  "Full", " battery", NULL };
        int i;

        for (i = 0; samples[i]; i++) {
            PangoLayout *l = gtk_widget_create_pango_layout(g->widget,
                                                            samples[i]);
            int pw = 0;

            pango_layout_get_pixel_size(l, &pw, NULL);
            g_object_unref(l);
            if (pw > g->text_w)
                g->text_w = pw;
        }
    }

    /* Сколько строк реально рисуется - от этого зависит и нужная высота,
     * и вертикальное выравнивание. */
    rows = 0;
    if (st->show_percent) rows++;
    if (st->show_time)    rows++;
    if (rows < 1)
        rows = 1;

    /* Минимальный размер: корпус слева, текст справа, оба отступом от
     * рамки. Окно меньше этого обрезает содержимое, поэтому draw()
     * потом поднимет размер до need_w/need_h. */
    g->need_w = 2 * AB_FRAME_PAD + AB_BODY_W + AB_GAP + g->text_w;
    g->need_h = 2 * AB_FRAME_PAD +
                MAX(AB_BODY_H, rows * g->line_h + (rows - 1) * AB_TEXT_ROW_GAP);

    /* Ниже минимума раскладка считается от минимума: иначе текст уехал бы
     * в отрицательные координаты и пропал совсем. */
    if (w < g->need_w)
        w = g->need_w;
    if (h < g->need_h)
        h = g->need_h;

    /* Корпус: натуральный 76x37, по вертикали по центру, слева от текста.
     * Вертикальный центр берём от h, а не от need_h - иначе при окне
     * меньше нужного корпус прилипал бы к верхнему краю. */
    g->body_x = AB_FRAME_PAD;
    g->body_y = (h - AB_BODY_H) / 2;
    if (g->body_y < 0)
        g->body_y = 0;

    /* Текст прижат к ПРАВОМУ краю с отступом AB_FRAME_PAD. */
    g->text_x = w - AB_FRAME_PAD - g->text_w;

    /* Вертикаль: процент прижат к ВЕРХУ рамки, время - к НИЗУ.
     * При одной строке она центрируется по корпусу. */
    if (rows >= 2) {
        g->text_y_percent = AB_FRAME_PAD;
        g->text_y_time = h - AB_FRAME_PAD - g->line_h;
    } else {
        g->text_y_percent = g->body_y + (AB_BODY_H - g->line_h) / 2;
        g->text_y_time = g->text_y_percent;
    }
    g->text_y_only = g->body_y + (AB_BODY_H - g->line_h) / 2;

    /* Индикатор вписан в корпус по тем же полям, что и у темы 76x37. */
    g->ind_x = g->body_x + AB_IND_INSET_X;
    g->ind_y = g->body_y + (AB_BODY_H - AB_IND_H) / 2;
    g->ind_w = AB_BODY_W - 2 * AB_IND_INSET_X;
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
    ab_layout(st, w, h, cr, &g);

    /* Окно не должно быть меньше нарисованного. Раньше размер брался
     * только из настроек (ab_clamp по жёстким 60x30), и при крупном
     * шрифте текст вылезал за рамку и обрезался. Требуемый размер
     * считает ab_layout() по метрикам шрифта; если окно меньше - растим
     * его и запоминаем минимум, чтобы ab_clamp() не ужал обратно. */
    if (g.need_w > st->min_width)
        st->min_width = g.need_w;
    if (g.need_h > st->min_height)
        st->min_height = g.need_h;
    if (w < g.need_w || h < g.need_h) {
        int nw = MAX(w, g.need_w);
        int nh = MAX(h, g.need_h);

        st->window_width  = nw;
        st->window_height = nh;
        api->resize(p, nw, nh);
        w = nw;
        h = nh;
        /* Раскладка считалась от меньшего размера - пересчитываем. */
        ab_layout(st, w, h, cr, &g);
    }

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
                             g.body_x, g.body_y, AB_BODY_W, AB_BODY_H);

    no_battery = (!st->battery ||
                  st->battery->state == ACPI_BATTERY_NOT_PRESENT);

    if (no_battery) {
        /* Батареи нет: индикатор рисуется В СЕРЫХ тонах. Аларм НЕ рисуем
         * никогда — красное читалось бы как «заряд кончился», а батареи
         * просто нет. Серый корпус даёт читаемый applet, при этом не
         * выдаёт его за низкий заряд. */
        if (xs_core_theme_has(p, "acpibattery-using"))
            ab_draw_gray(p, cr, "acpibattery-using",
                         g.ind_x, g.ind_y, g.ind_w, AB_IND_H);
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
    cairo_rectangle(cr, 0, 0, w, h);
    cairo_clip(cr);

    /* Блик ложится на корпус батарейки, поэтому рисуется по её
     * координатам, а не на всё окно. В оригинале он рисовался
     * последним, поверх текста, - здесь текст сбоку и блик ему не
     * мешает, а корпус получает объём. */
    if (xs_core_theme_has(p, "acpibattery-glass"))
        api->theme_draw_full(p, cr, "acpibattery-glass",
                             g.body_x, g.body_y, AB_BODY_W, AB_BODY_H);

    /* ascent передаётся в ab_text: тот сдвигает baseline от верха строки,
     * иначе текст уезжает вниз и строки слипаются. */
    if (no_battery) {
        ab_text(st, cr, " No", g.text_x, g.text_y_percent, FALSE, g.ascent);
        ab_text(st, cr, " battery", g.text_x, g.text_y_time, FALSE, g.ascent);
    } else if (st->show_percent && st->show_time) {
        ab_text(st, cr, st->percent_text, g.text_x, g.text_y_percent,
                st->low, g.ascent);
        ab_text(st, cr, st->time_text, g.text_x, g.text_y_time,
                st->low, g.ascent);
    } else if (st->show_percent) {
        ab_text(st, cr, st->percent_text, g.text_x, g.text_y_only,
                st->low, g.ascent);
    } else if (st->show_time) {
        ab_text(st, cr, st->time_text, g.text_x, g.text_y_only,
                st->low, g.ascent);
    }

    cairo_restore(cr);
}

static guint ab_tick(XsPlugin *p)
{
    AbState *st = p->priv;
    XsHostApi *api = p->host;

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
     * всё равно растил бы себя в draw(). */
    ab_int_prop(GTK_BOX(page), p, "window_width", "Window width",
                "Applet width in pixels", st->window_width,
                MAX(AB_MIN_WIDTH, st->min_width), 400);
    ab_int_prop(GTK_BOX(page), p, "window_height", "Window height",
                "Applet height in pixels", st->window_height,
                MAX(AB_MIN_HEIGHT, st->min_height), 200);
    ab_int_prop(GTK_BOX(page), p, "update_interval", "Update interval (s)",
                "Seconds between reads of sysfs", st->update_interval, 1, 3600);
    ab_int_prop(GTK_BOX(page), p, "alarm_threshold", "Low battery threshold (%)",
                "Charge percent at or below which the alarm colour is used",
                st->alarm_threshold, 0, 100);

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