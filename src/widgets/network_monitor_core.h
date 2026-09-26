#ifndef NETWORK_MONITOR_CORE_H
#define NETWORK_MONITOR_CORE_H

#include <glib.h>
#include <cairo.h>
#include <gtk/gtk.h>

/* --- Сетевой апплет: собственное core, НЕ зависящее от disk_monitor.
 *
 * disk_monitor используется как ОБРАЗЕЦ (каркас applet: init/tick/draw/
 * render/shutdown/Properties, округлённое окно, подгонка координат, серии
 * с историей), но его исходники не редактируются и не импортируются.
 * Дублирование обвязки осознанное: общий модуль стоит вынести, когда
 * потребителей будет два РАБОТАЮЩИХ, а не один предполагаемый. */

#define NM_HISTORY_MAX 256
#define NM_SERIES_MAX 2
#define NM_MAX_IFACES 64
#define NM_RATE_SMOOTH_MAX 14
#define NM_CTX_KEY "xs-network-monitor-ctx"

/* --- Тип интерфейса ------------------------------------------------
 * Определяется по sysfs, а не по имени: имя — соглашение, sysfs — факт.
 * На машине пользователя присутствуют все пять (проверено: wan0/lan0 —
 * физические, brlan0/virbr0 — мосты, tap0/tun10/fastvpn-* — софт,
 * *_ifb — программные зеркала shaped-трафика). */
typedef enum {
    NM_IF_UNKNOWN = 0,
    NM_IF_PHYSICAL,   /* type=1, есть MAC, link detected */
    NM_IF_BRIDGE,     /* type=1, мост: br*, virbr* */
    NM_IF_VIRTUAL,    /* type=1, программный: tap, veth, *_ifb */
    NM_IF_TUNNEL,     /* type=65534 (ARPHRD_NONE): tun/tap/wireguard/vpn */
    NM_IF_LOOPBACK,   /* lo */
} NmInterfaceKind;

/* Режим графика: один общий или два раздельных (как в conky, где
 * downspeedgraph и upspeedgraph — независимые объекты со своими шкалами). */
typedef enum {
    NM_GRAPH_COMBINED = 0,
    NM_GRAPH_SPLIT,
} NmGraphMode;

typedef struct {
    char *ifname;
    guint64 rx_bytes, tx_bytes;
    guint64 rx_packets, tx_packets;
    guint64 rx_errors, tx_errors;
    guint64 rx_dropped, tx_dropped;
    gboolean valid;
} NmNetSample;

/* Парсинг /proc/net/dev.
 *
 * ПЕРЕПОЛНЕНИЕ: счётчики 32-битные и переполняются (на wan0 набежало
 * 28 ГБ — это реально). При current < previous НЕЛЬЗЯ писать ноль: conky
 * присваивает current и не добавляет дельту, иначе в графике вылезает
 * ложный spike в гигабайты. Возвращает FALSE при обороте — вызывающий
 * обязан сбросить baseline. */
gboolean nm_parse_netdev(const char *text, const char *ifname,
                         NmNetSample *out);
guint nm_parse_netdev_all(const char *text, NmNetSample *out, guint max);

gint64 nm_rate_bytes_per_second(guint64 previous, guint64 current,
                                gint64 previous_us, gint64 now_us);

NmInterfaceKind nm_interface_kind(const char *ifname, const char *sysfs_root);
gboolean nm_link_detected(const char *ifname, const char *sysfs_root);
/* Скорость линка, Мбит/с; 0 = неизвестно (у ifb её не бывает). */
gint64 nm_link_speed_mbit(const char *ifname, const char *sysfs_root);
/* Первый IPv4; NULL если адресов нет — обычное дело для ifb и tun. */
char *nm_interface_ipv4(const char *ifname);

/* --- Обвязка applet-каркаса (перенесена из disk_monitor) ---------- */
gboolean nm_parse_rgba(const char *text, gdouble out[4]);
char *nm_format_label(const char *fmt, const char *value);
char *nm_format_rgba(const gdouble color[4]);
void nm_fill_rgba(const gdouble color[4], double alpha, gdouble out[4]);

int nm_scale_position(int value, int design_size, int live_size, int max);
int nm_fit_text_coordinate(int anchor, int text_extent, int limit);
double nm_corner_radius_value(int value);
gboolean nm_corner_radius_is_rounded(double radius);
cairo_region_t *nm_rounded_region(int width, int height, int radius);

void nm_push_history(guint64 *ring, guint *head, guint *count,
                     guint64 value, gint64 now_us);
guint64 nm_history_value(const guint64 *ring, guint head, guint count, guint i);
guint nm_history_columns(int plot_width, guint available);
int nm_history_origin_x(int graph_width, guint columns);

/* Сглаживание: отдельное кольцо на NM_RATE_SMOOTH_MAX отсчётов, НЕ
 * история графика. Ставить в кольцо графика нельзя: там 256 слотов, и
 * усреднение по всей длине забило бы свежие значения. */
typedef struct {
    guint64 ring[NM_RATE_SMOOTH_MAX];
    guint head;
    guint count;
} NmSmoothRing;

void nm_rate_smooth_reset(NmSmoothRing *smooth);
void nm_rate_smooth_push(NmSmoothRing *smooth, gint64 rate);
gint64 nm_rate_smoothed(const NmSmoothRing *smooth, guint window);

char *nm_format_rate(guint64 bytes_per_second);
char *nm_format_bytes(guint64 total);

/* Диалог-контекст с подсчётом ссылок: коммит настроек отложен, пока
 * живы все контролы. */
typedef struct {
    gint refcount;
    char *instance_name;
} NmDialogContext;

NmDialogContext *nm_dialog_context_new(const char *instance_name);
NmDialogContext *nm_dialog_context_ref(NmDialogContext *ctx);
void nm_dialog_context_unref(NmDialogContext *ctx);
const char *nm_dialog_context_name(const NmDialogContext *ctx);
gint nm_dialog_context_refcount(const NmDialogContext *ctx);

/* Страница Properties длиннее окна: кладём её в прокручиваемую область
 * и задаём размер окна диалога. */
GtkWidget *nm_properties_scroller(GtkWidget *page, int width, int height);
void nm_properties_size(const char *plugin_type, int *width, int *height);

#endif
