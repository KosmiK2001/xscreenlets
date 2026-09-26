/* network_monitor_core.c — данные и вспомогательные функции сетевого
 * апплета. Каркас (подгонка координат, скругление, история, сглаживание,
 * цвет) перенесён из disk_monitor, который здесь служит ОБРАЗЦОМ: его
 * исходники не меняются и не импортируются, дублирование осознанное. */
#include "network_monitor_core.h"

#include <ctype.h>
#include <errno.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/types.h>

#define NM_NETDEV "/proc/net/dev"
#define NM_RATE_SMOOTH_MAX 14

/* ------------------------------------------------------------------ */
/* /proc/net/dev                                                       */
/* ------------------------------------------------------------------ */

/* Строка формата: "  eth0: RXbytes RXpackets RXerrs RXdrop ...".
 * Два поля счётчиков, которые нам нужны, — первое (rx_bytes) и девятое
 * (tx_bytes); между ними восемь полей receive, после tx_bytes — ещё
 * восемь. Ошибки и дропы берём из тех же строк, они попадают в
 * диагностику, когда канал переполнен. */
gboolean nm_parse_netdev(const char *text, const char *ifname,
                         NmNetSample *out)
{
    const char *line;

    if (!text || !ifname || !*ifname || !out)
        return FALSE;
    memset(out, 0, sizeof(*out));

    for (line = text; line && *line; ) {
        const char *colon;
        const char *name_start;
        const char *name_end;
        char name[64];
        gsize name_len;
        guint64 values[16];
        int n = 0;
        const char *p;

        /* имя интерфейса идёт после пробелов, до двоеточия */
        while (*line == ' ' || *line == '\t')
            line++;
        if (!*line)
            break;
        name_start = line;
        colon = strchr(line, ':');
        if (!colon)
            break;
        name_end = colon;
        /* пропускаем строку, если это не наш интерфейс, ДО разбора чисел */
        name_len = (gsize)(name_end - name_start);
        while (name_len > 0 &&
               (name_start[name_len - 1] == ' ' || name_start[name_len - 1] == '\t'))
            name_len--;
        if (name_len == 0 || name_len >= sizeof(name))
            goto next_line;
        memcpy(name, name_start, name_len);
        name[name_len] = '\0';
        if (strcmp(name, ifname) != 0)
            goto next_line;

        p = colon + 1;
        for (n = 0; n < (int)G_N_ELEMENTS(values); n++) {
            char *end = NULL;

            while (*p == ' ' || *p == '\t')
                p++;
            if (!*p || *p == '\n')
                break;
            values[n] = g_ascii_strtoull(p, &end, 10);
            if (end == p)
                break;
            p = end;
        }
        /* Нужно 12 полей, а не 10: ниже читаются values[10] и values[11]
         * (ошибки и потери передачи). Проверка на 10 пропускала строку из
         * ровно 10 полей, и эти два значения приходили из
         * неинициализированного стека.
         *
         * Значения сейчас нигде не отображаются, поэтому это латентный
         * баг: как только ошибки выведут на экран, он станет видимым
         * мусором. */
        if (n < 12)
            return FALSE;   /* обрезанная строка — это не данные */
        out->ifname = g_strdup(ifname);
        out->rx_bytes = values[0];
        out->rx_packets = values[1];
        out->rx_errors = values[2];
        out->rx_dropped = values[3];
        out->tx_bytes = values[8];
        out->tx_packets = values[9];
        out->tx_errors = values[10];
        out->tx_dropped = values[11];
        out->valid = TRUE;
        return TRUE;
    next_line:
        line = strchr(line, '\n');
        if (line)
            line++;
    }
    return FALSE;
}

guint nm_parse_netdev_all(const char *text, NmNetSample *out, guint max)
{
    const char *line;
    guint count = 0;

    if (!text || !out || max == 0)
        return 0;

    for (line = text; line && *line; ) {
        const char *colon;
        const char *name_start;
        const char *name_end;
        char name[64];
        gsize name_len;
        guint64 values[16];
        int n = 0;
        const char *p;

        while (*line == ' ' || *line == '\t')
            line++;
        if (!*line)
            break;
        name_start = line;
        colon = strchr(line, ':');
        if (!colon)
            break;
        name_end = colon;
        name_len = (gsize)(name_end - name_start);
        while (name_len > 0 &&
               (name_start[name_len - 1] == ' ' || name_start[name_len - 1] == '\t'))
            name_len--;
        if (name_len == 0 || name_len >= sizeof(name)) {
            line = strchr(line, '\n');
            if (line) line++;
            continue;
        }
        memcpy(name, name_start, name_len);
        name[name_len] = '\0';

        p = colon + 1;
        for (n = 0; n < (int)G_N_ELEMENTS(values); n++) {
            char *end = NULL;

            while (*p == ' ' || *p == '\t')
                p++;
            if (!*p || *p == '\n')
                break;
            values[n] = g_ascii_strtoull(p, &end, 10);
            if (end == p)
                break;
            p = end;
        }
        /* 12, а не 10: ниже читаются values[10] и values[11]. На 10
         * строка из ровно 10 полей проходила, и ошибки/потери приходили
         * из неинициализированного стека. */
        if (n >= 12 && count < max) {
            out[count].ifname = g_strdup(name);
            out[count].rx_bytes = values[0];
            out[count].rx_packets = values[1];
            out[count].rx_errors = values[2];
            out[count].rx_dropped = values[3];
            out[count].tx_bytes = values[8];
            out[count].tx_packets = values[9];
            out[count].tx_errors = values[10];
            out[count].tx_dropped = values[11];
            out[count].valid = TRUE;
            count++;
        }
        line = strchr(line, '\n');
        if (line)
            line++;
    }
    return count;
}

gint64 nm_rate_bytes_per_second(guint64 previous, guint64 current,
                                gint64 previous_us, gint64 now_us)
{
    guint64 elapsed_us, delta;

    if (previous_us <= 0 || now_us <= previous_us)
        return 0;
    /* Переполнение счётчика: current меньше предыдущего. Возвращаем
     * отрицательное значение как признак, что baseline надо сбросить —
     * писать ноль нельзя, это создаёт ложный провал в графике, а
     * подставлять дельту в 4 гигабайда при обороте — ложный spike. */
    if (current < previous)
        return -1;
    elapsed_us = (guint64)(now_us - previous_us);
    if (elapsed_us == 0)
        return 0;
    delta = current - previous;
    if (delta > (guint64)G_MAXINT64)
        return G_MAXINT64;
    {
        guint64 whole = delta / elapsed_us;
        guint64 remainder = delta % elapsed_us;
        guint64 fraction;

        if (whole > (guint64)G_MAXINT64 / 1000000)
            return G_MAXINT64;
        fraction = (guint64)((long double)remainder * 1000000.0L / elapsed_us);
        if (whole * 1000000 > (guint64)G_MAXINT64 - fraction)
            return G_MAXINT64;
        return (gint64)(whole * 1000000 + fraction);
    }
}

/* ------------------------------------------------------------------ */
/* Тип интерфейса и его свойства (sysfs — источник истины)            */
/* ------------------------------------------------------------------ */

static char *read_sysfs_line(const char *sysfs_root, const char *ifname,
                             const char *leaf)
{
    char path[512];
    char buf[64];
    FILE *fp;
    size_t n;

    if (!ifname || !*ifname)
        return NULL;
    g_snprintf(path, sizeof(path), "%s/%s/%s",
               sysfs_root ? sysfs_root : "/sys/class/net", ifname, leaf);
    fp = fopen(path, "r");
    if (!fp)
        return NULL;
    n = fread(buf, 1, sizeof(buf) - 1, fp);
    fclose(fp);
    buf[n] = '\0';
    while (n > 0 && (buf[n - 1] == '\n' || buf[n - 1] == '\r' ||
                     buf[n - 1] == ' ' || buf[n - 1] == '\t'))
        buf[--n] = '\0';
    return n ? g_strdup(buf) : NULL;
}

static gboolean sysfs_entry_equals(const char *root, const char *ifname,
                                   const char *leaf, const char *want)
{
    char *value = read_sysfs_line(root, ifname, leaf);
    gboolean equal;

    if (!value)
        return FALSE;
    equal = strcmp(value, want) == 0;
    g_free(value);
    return equal;
}

gboolean nm_link_detected(const char *ifname, const char *sysfs_root)
{
    return sysfs_entry_equals(sysfs_root, ifname, "operstate", "up");
}

NmInterfaceKind nm_interface_kind(const char *ifname, const char *sysfs_root)
{
    char *type_str;
    long type_value;
    char *address;
    NmInterfaceKind kind = NM_IF_UNKNOWN;

    if (!ifname || !*ifname)
        return NM_IF_UNKNOWN;
    if (strcmp(ifname, "lo") == 0)
        return NM_IF_LOOPBACK;

    type_str = read_sysfs_line(sysfs_root, ifname, "type");
    if (!type_str)
        return NM_IF_UNKNOWN;
    type_value = strtol(type_str, NULL, 10);
    g_free(type_str);

    /* ARPHRD_NONE (65534) — туннели и tap/wireguard: у них нет MAC
     * и нет физического смысла скорость линка. */
    if (type_value == 65534)
        return NM_IF_TUNNEL;

    address = read_sysfs_line(sysfs_root, ifname, "address");
    if (address) {
        if (strcmp(ifname, "lo") == 0)
            kind = NM_IF_LOOPBACK;
        else if (g_str_has_prefix(ifname, "br") ||
                 g_str_has_prefix(ifname, "virbr"))
            kind = NM_IF_BRIDGE;
        else
            kind = NM_IF_VIRTUAL;   /* уточним ниже по ifb/bridge-флагу */
    }
    g_free(address);

    /* Мост может быть назван как угодно, но у него есть sysfs-флаг
     * /sys/class/net/<if>/bridge — это надёжнее имени. */
    if (kind != NM_IF_LOOPBACK) {
        char path[512];
        struct stat sb;

        g_snprintf(path, sizeof(path), "%s/%s/bridge",
                   sysfs_root ? sysfs_root : "/sys/class/net", ifname);
        if (stat(path, &sb) == 0)
            return NM_IF_BRIDGE;
    }
    /* ifb: зеркало shaped-трафика. Имя — соглашение, но у ifb есть
     * характерный признак — модуль ifb в device-ссылке. */
    if (g_str_has_suffix(ifname, "_ifb") || strcmp(ifname, "ifb0") == 0)
        return NM_IF_VIRTUAL;

    /* Осталось отличить физический NIC от виртуального tap/veth: у
     * физического есть реальная скорость линка в ethtool. */
    if (nm_link_speed_mbit(ifname, sysfs_root) > 0 &&
        nm_link_detected(ifname, sysfs_root)) {
        /* ifb/tap/veth отвечают 10000/Unknown; настоящий NIC — 100,
         * 1000, 2500 с duplex full. Ориентируемся на operstate +
         * имя: br/tap/veth/ifb уже отсеяны выше. */
        if (g_str_has_prefix(ifname, "tap") || g_str_has_prefix(ifname, "veth"))
            return NM_IF_VIRTUAL;
        return NM_IF_PHYSICAL;
    }
    return kind == NM_IF_UNKNOWN ? NM_IF_VIRTUAL : kind;
}

gint64 nm_link_speed_mbit(const char *ifname, const char *sysfs_root)
{
    /* Скорость линка живёт НЕ в /sys/class/net, а в ethtool ioctl,
     * поэтому читаем вывод ethtool(8). Порождает процесс, поэтому
     * вызывается один раз на интерфейс при смене цели, а не каждый
     * тик. */
    char cmd[512];
    char line[256];
    FILE *fp;
    gint64 speed = 0;

    if (!ifname || !*ifname || strchr(ifname, '\''))
        return 0;
    g_snprintf(cmd, sizeof(cmd), "ethtool %s 2>/dev/null", ifname);
    fp = popen(cmd, "r");
    if (!fp)
        return 0;
    while (fgets(line, sizeof(line), fp)) {
        if (g_str_has_prefix(line, "\tSpeed:")) {
            speed = g_ascii_strtoll(line + 7, NULL, 10);
            break;
        }
    }
    pclose(fp);
    (void)sysfs_root;
    return speed > 0 ? speed : 0;
}

char *nm_interface_ipv4(const char *ifname)
{
    char cmd[512];
    char line[512];
    FILE *fp;
    char *result = NULL;

    if (!ifname || !*ifname || strchr(ifname, '\''))
        return NULL;
    /* Не запускаем ip/ifconfig ради каждого тика: вызывающий кэширует.
     * -o -4 -f inet печатает только IPv4 одной строкой на интерфейс. */
    g_snprintf(cmd, sizeof(cmd), "ip -o -4 addr show dev %s 2>/dev/null",
               ifname);
    fp = popen(cmd, "r");
    if (!fp)
        return NULL;
    while (fgets(line, sizeof(line), fp)) {
        const char *p = strstr(line, "inet ");
        if (!p)
            continue;
        p += 5;
        {
            const char *end = p;
            char buf[64];
            gsize n;
            while (*end && *end != '/' && *end != ' ' && *end != '\n')
                end++;
            n = (gsize)(end - p);
            if (n == 0 || n >= sizeof(buf))
                continue;
            memcpy(buf, p, n);
            buf[n] = '\0';
            result = g_strdup(buf);
            break;
        }
    }
    pclose(fp);
    return result;
}

/* ------------------------------------------------------------------ */
/* Форматирование                                                     */
/* ------------------------------------------------------------------ */

char *nm_format_rate(guint64 bytes_per_second)
{
    static const char *units[] = {"B/s", "KiB/s", "MiB/s", "GiB/s", "TiB/s"};
    double number = (double)bytes_per_second;
    guint unit = 0;

    while (number >= 1024.0 && unit < G_N_ELEMENTS(units) - 1) {
        number /= 1024.0;
        unit++;
    }
    if (unit == 0)
        return g_strdup_printf("%.0f %s", number, units[unit]);
    return g_strdup_printf(unit <= 2 ? "%.1f %s" : "%.2f %s",
                          number, units[unit]);
}

char *nm_format_bytes(guint64 total)
{
    static const char *units[] = {"B", "KiB", "MiB", "GiB", "TiB", "PiB"};
    double number = (double)total;
    guint unit = 0;

    while (number >= 1024.0 && unit < G_N_ELEMENTS(units) - 1) {
        number /= 1024.0;
        unit++;
    }
    if (unit == 0)
        return g_strdup_printf("%.0f %s", number, units[unit]);
    return g_strdup_printf("%.1f %s", number, units[unit]);
}

/* ------------------------------------------------------------------ */
/* Цвета (перенесено из disk_monitor)                                 */
/* ------------------------------------------------------------------ */

gboolean nm_parse_rgba(const char *text, gdouble rgba[4])
{
    char *end = NULL;
    gdouble values[4];
    const char *p;
    gboolean byted = FALSE;
    int i;

    if (!text || !*text)
        return FALSE;

    /* Формат rgba(r,g,b,a) — тот, что пишет GtkColorButton и который
     * лежит в примере конфига и в живых конфигах пользователя:
     *   series0_color=rgba(51,191,255,255)
     *
     * Раньше парсер сразу требовал, чтобы первый символ был цифрой или
     * точкой, а значения лежали в 0..1. Форма rgba(...) не проходила
     * даже по первому символу, то есть отвергалась целиком, и
     * nm_read_color молча брал дефолт. Все настройки цвета из конфига
     * на диске не применялись — без единого сообщения.
     *
     * Формат rgba() разбираем первым: у него значения 0..255 и он
     * однозначен. Если скобок нет, работает старый путь 0..1, который
     * пишет nm_format_rgba, — так оба формата живут вместе и старые
     * конфиги не ломаются. */
    p = text;
    while (*p == ' ' || *p == '\t')
        p++;
    if (g_ascii_strncasecmp(p, "rgba", 4) == 0) {
        p += 4;
        while (*p == ' ' || *p == '\t')
            p++;
        if (*p != '(')
            return FALSE;
        p++;
        byted = TRUE;
        text = p;
    } else if (g_ascii_strncasecmp(p, "rgb", 3) == 0) {
        /* rgb(r,g,b) — альфа 1. */
        p += 3;
        while (*p == ' ' || *p == '\t')
            p++;
        if (*p != '(')
            return FALSE;
        p++;
        byted = TRUE;
        text = p;
    } else {
        if (!g_ascii_isdigit(*p) && *p != '.' && *p != '-' && *p != '+')
            return FALSE;
    }

    for (i = 0; i < 4; i++) {
        while (*text == ' ' || *text == '\t')
            text++;
        if (byted && i == 3) {
            /* Для rgb(...) альфа подставляется единицей, а не
             * разбирается: четвёртого поля там нет. */
            if (*text == ')' || *text == '\0') {
                values[3] = 1.0;
                break;
            }
        }
        if (!*text)
            return FALSE;
        errno = 0;
        values[i] = g_ascii_strtod(text, &end);
        if (errno || end == text || !isfinite(values[i]))
            return FALSE;
        if (byted) {
            if (values[i] < 0.0 || values[i] > 255.0)
                return FALSE;
            values[i] /= 255.0;
        } else if (values[i] < 0.0 || values[i] > 1.0) {
            return FALSE;
        }
        if (i < 3) {
            /* Пробелы разрешены с обеих сторон от запятой: в rgba(...) их
             * писать естественно, и GtkColorButton так и делает. Раньше
             * здесь стоял запрет на пробел сразу после запятой, из-за
             * которого «rgba( 0, 128, 0, 255 )» не разбирался. */
            /* Для rgb(...) после третьего поля запятой нет: вместо неё
             * сразу закрывающая скобка. */
            while (*end == ' ' || *end == '\t')
                end++;
            if (*end != ',') {
                if (byted && i == 2 && *end == ')') {
                    /* rgb(r,g,b): альфы нет, подставляем единицу. */
                    values[3] = 1.0;
                    break;
                }
                return FALSE;
            }
            text = end + 1;
        } else {
            /* Четвёртое поле: после него допустимы только закрывающая
             * скобка (для rgba(...)/rgb(...)) и конец строки. */
            while (*end == ' ' || *end == '\t')
                end++;
            if (byted) {
                if (*end != ')')
                    return FALSE;
                end++;
                while (*end == ' ' || *end == '\t' || *end == '\n' ||
                       *end == '\r')
                    end++;
                if (*end)
                    return FALSE;
            } else if (*end) {
                return FALSE;
            }
        }
    }
    memcpy(rgba, values, sizeof(values));
    return TRUE;
}

char *nm_format_rgba(const gdouble rgba[4])
{
    char values[4][G_ASCII_DTOSTR_BUF_SIZE];
    int i;

    for (i = 0; i < 4; i++)
        g_ascii_formatd(values[i], sizeof(values[i]), "%.6f",
                        CLAMP(rgba[i], 0.0, 1.0));
    return g_strdup_printf("%s,%s,%s,%s", values[0], values[1], values[2],
                           values[3]);
}

void nm_fill_rgba(const gdouble input[4], double fill_alpha, gdouble output[4])
{
    output[0] = input[0];
    output[1] = input[1];
    output[2] = input[2];
    output[3] = CLAMP(fill_alpha, 0.0, 1.0);
}

/* Подставить значение в пользовательский формат.
 *
 * total<N>_label приходит из конфига и уходит в g_strdup_printf, а поле в
 * UI подписано «Format» — то есть пользователь законно ждёт там %s и
 * любых других спецификаторов. Ревью нашло, что «Total %d: %s» даёт
 * «Total 676520384: 1.5 MiB», а «100% done: %s» вообще мусор из стека:
 * лишний аргумент, а строка не проверяется на валидность.
 *
 * Здесь формат проверяется ДО подстановки, и неизвестные спецификаторы
 * не превращаются в чтение мусора. Единственный поддерживаемый
 * спецификатор — %s: он и есть смысл настройки. Остальные оставляем как
 * есть: пользовательский текст не должен превращаться в «%!d(...)».
 *
 * Если формат невалиден или содержит спецификатор кроме %s, значение
 * подставляется целиком: «Total: 1.5 MiB». Это читаемо и безопасно. */
char *nm_format_label(const char *fmt, const char *value)
{
    const char *p;
    gboolean has_s = FALSE;
    gboolean bad = FALSE;

    if (!fmt || !*fmt)
        return g_strdup(value);
    /* Спецификаторы разбираем вручную. Проверять валидность через
     * g_strdup_vprintf нельзя: для этого нужен va_list, а NULL вместо
     * него — неопределённое поведение, то есть краш. */
    for (p = fmt; *p; p++) {
        if (*p != '%')
            continue;
        p++;
        if (*p == '\0') {
            /* «Хвостовой» процент: строки формата не кончились, а
             * спецификатора нет. */
            bad = TRUE;
            break;
        }
        if (*p == '%')
            continue;
        /* Поддерживаем только %s — это и есть смысл настройки. Любой
         * другой спецификатор печатал бы мусор из стека. */
        if (*p == 's')
            has_s = TRUE;
        else
            bad = TRUE;
    }
    /* Нет %s — значит пользователь просто написал текст. Значение надо
     * дописать, иначе скорость просто исчезнет с экрана. */
    if (!has_s)
        return g_strdup_printf("%s: %s", fmt, value);
    if (bad)
        return g_strdup_printf("%s: %s", fmt, value);
    return g_strdup_printf(fmt, value);
}

/* ------------------------------------------------------------------ */
/* Геометрия: подгонка координат и скругление                          */
/* ------------------------------------------------------------------ */

int nm_scale_position(int value, int design_size, int live_size, int max)
{
    gint64 scaled;

    if (design_size <= 0 || live_size <= 0)
        return CLAMP(value, 0, MAX(0, max));
    scaled = ((gint64)value * live_size + (gint64)design_size / 2) /
             (gint64)design_size;
    if (max > 0 && scaled > max)
        scaled = max;
    if (scaled < 0)
        scaled = 0;
    return (int)scaled;
}

int nm_fit_text_coordinate(int anchor, int text_extent, int limit)
{
    int x = anchor;

    if (x < 0)
        x = 0;
    if (limit > 0 && x + text_extent > limit)
        x = limit - text_extent;
    if (x < 0)
        x = 0;
    return x;
}

double nm_corner_radius_value(int value)
{
    return value > 0 ? (double)value : 0.0;
}

gboolean nm_corner_radius_is_rounded(double radius)
{
    return radius > 0.5;
}

cairo_region_t *nm_rounded_region(int width, int height, int radius)
{
    const double r = nm_corner_radius_value(radius);
    cairo_region_t *region;
    cairo_rectangle_int_t box;
    double scaled;

    if (width <= 0 || height <= 0)
        return NULL;
    if (!nm_corner_radius_is_rounded(r))
        return NULL;

    scaled = MIN(r, MIN(width, height) / 2.0);
    region = cairo_region_create();
    if (!region)
        return NULL;

    for (int i = 0; i <= (int) ceil(scaled); i++) {
        /* Глубина среза угла МАКСИМАЛЬНА в угловой колонке (i=0) и
         * исчезает при i=scaled: окружность с центром (scaled, scaled).
         * Обратная формула съедала дугу рамки по всей окружности. */
        double d = fabs(i - scaled);
        int cut = 0;

        if (d <= scaled)
            cut = (int) floor(scaled - sqrt(scaled * scaled - d * d));
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
        int mid = (int) ceil(scaled);

        box.x = mid;
        box.y = 0;
        box.width = width - 2 * mid;
        box.height = height;
        if (box.width > 0)
            cairo_region_union_rectangle(region, &box);
    }
    return region;
}

/* ------------------------------------------------------------------ */
/* История и сглаживание                                              */
/* ------------------------------------------------------------------ */

void nm_push_history(guint64 *ring, guint *head, guint *count,
                     guint64 value, gint64 now_us)
{
    (void)now_us;
    ring[*head] = value;
    *head = (*head + 1) % NM_HISTORY_MAX;
    *count = MIN(*count + 1, NM_HISTORY_MAX);
}

guint64 nm_history_value(const guint64 *ring, guint head, guint count,
                         guint age_from_newest)
{
    guint index;

    if (age_from_newest >= count)
        return 0;
    index = (head + NM_HISTORY_MAX - 1 - age_from_newest) % NM_HISTORY_MAX;
    return ring[index];
}

guint nm_history_columns(int width, guint available)
{
    if (available == 0 || width <= 0)
        return 0;
    return MIN(available, (guint)width);
}

int nm_history_origin_x(int graph_width, guint columns)
{
    if (graph_width <= 0 || columns == 0)
        return 0;
    return MAX(0, graph_width - (int)MIN(columns, (guint)graph_width));
}

void nm_rate_smooth_reset(NmSmoothRing *smooth)
{
    if (!smooth)
        return;
    memset(smooth, 0, sizeof(*smooth));
}

void nm_rate_smooth_push(NmSmoothRing *smooth, gint64 rate)
{
    if (!smooth)
        return;
    /* Кладём только настоящую скорость: первое чтение лишь создаёт
     * baseline, и ноль в кольце размазал бы первую реальную величину. */
    smooth->ring[smooth->head] = (guint64)(rate > 0 ? rate : 0);
    smooth->head = (smooth->head + 1) % NM_RATE_SMOOTH_MAX;
    if (smooth->count < NM_RATE_SMOOTH_MAX)
        smooth->count++;
}

gint64 nm_rate_smoothed(const NmSmoothRing *smooth, guint window)
{
    guint i, n;
    guint64 sum = 0;

    if (!smooth || !smooth->count)
        return 0;
    n = MIN(window ? window : 1, smooth->count);
    /* Идём НАЗАД от свежего, чтобы короткое окно всегда считало последние
     * значения. Сумма насыщается: nm_rate_bytes_per_second() законно
     * возвращает G_MAXINT64, и сложение четырнадцати таких знаков —
     * переполнение со знаком, то есть UB, а не «просто большое число». */
    for (i = 0; i < n; i++) {
        guint idx = (smooth->head + NM_RATE_SMOOTH_MAX - 1 - i) %
                    NM_RATE_SMOOTH_MAX;
        guint64 value = smooth->ring[idx];

        if (sum > G_MAXUINT64 - value)
            return G_MAXINT64;
        sum += value;
    }
    return (gint64)(sum / n);
}

/* ------------------------------------------------------------------ */
/* Контекст диалога                                                    */
/* ------------------------------------------------------------------ */

/* Ссылается на имя инстанса, чтобы обработчики Properties находили живой
 * applet: GTK держит контролы после закрытия диалога, и без этого
 * коммит настроек ушёл бы в пустоту. */
NmDialogContext *nm_dialog_context_new(const char *instance_name)
{
    NmDialogContext *ctx = g_new0(NmDialogContext, 1);

    ctx->refcount = 1;
    ctx->instance_name = g_strdup(instance_name ? instance_name : "");
    return ctx;
}

NmDialogContext *nm_dialog_context_ref(NmDialogContext *ctx)
{
    if (ctx)
        g_atomic_int_inc(&ctx->refcount);
    return ctx;
}

void nm_dialog_context_unref(NmDialogContext *ctx)
{
    if (!ctx)
        return;
    if (!g_atomic_int_dec_and_test(&ctx->refcount))
        return;
    g_free(ctx->instance_name);
    g_free(ctx);
}

const char *nm_dialog_context_name(const NmDialogContext *ctx)
{
    return ctx ? ctx->instance_name : NULL;
}

gint nm_dialog_context_refcount(const NmDialogContext *ctx)
{
    return ctx ? ctx->refcount : 0;
}

/* ------------------------------------------------------------------ */
/* Геометрия страницы Properties                                        */
/* ------------------------------------------------------------------ */

/* Страница настроек длиннее окна диалога, поэтому она кладётся в
 * прокручиваемую область. Без propagate_natural_* scroller сообщает
 * странице её полный размер и окно Properties растёт под все строки
 * вместо прокрутки. */
GtkWidget *nm_properties_scroller(GtkWidget *page, int width, int height)
{
    GtkWidget *scroller, *viewport;

    if (!GTK_IS_WIDGET(page))
        return NULL;
    scroller = gtk_scrolled_window_new(NULL, NULL);
    gtk_scrolled_window_set_policy(GTK_SCROLLED_WINDOW(scroller),
                                   GTK_POLICY_NEVER, GTK_POLICY_AUTOMATIC);
    gtk_scrolled_window_set_propagate_natural_width(GTK_SCROLLED_WINDOW(scroller),
                                                    FALSE);
    gtk_scrolled_window_set_propagate_natural_height(GTK_SCROLLED_WINDOW(scroller),
                                                     FALSE);
    /* Один вызов: второй set_size_request сбросил бы первое измерение. */
    if (width > 0 && height > 0)
        gtk_widget_set_size_request(scroller, width, height);
    else if (width > 0)
        gtk_widget_set_size_request(scroller, width, -1);
    else if (height > 0)
        gtk_widget_set_size_request(scroller, -1, height);
    viewport = gtk_viewport_new(NULL, NULL);
    gtk_container_add(GTK_CONTAINER(viewport), page);
    gtk_container_add(GTK_CONTAINER(scroller), viewport);
    return scroller;
}

void nm_properties_size(const char *plugin_type, int *width, int *height)
{
    if (plugin_type && strcmp(plugin_type, "network_monitor") == 0) {
        if (width)
            *width = 620;
        if (height)
            *height = 780;
    } else {
        if (width)
            *width = 0;
        if (height)
            *height = 0;
    }
}
