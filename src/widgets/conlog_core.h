/* conlog_core.h — ядро апплета «вывод команды».
 *
 * GTK-free: всё, что можно вынести в обычный C, живёт здесь, и
 * покрывается тестами без запуска X. Апплет conlog.c занимается
 * только окном, конфигом и отрисовкой.
 *
 * Разделение не формальность. Две вещи в этом плагине легко ломаются
 * и не видны на глаз: разделение вывода команды на строки с учётом того,
 * что команда может писать частичными порциями, и то, что буфер прокрутки
 * ограничен, а строки не перестают приходить. Обе проверяются тестами.
 */
#ifndef CONLOG_CORE_H
#define CONLOG_CORE_H

#include <glib.h>

/* Уровень строки. Определяется по ТЕКСТУ строки, а не по коду выхода
 * команды: у journalctl и dmesg «ошибка» — это слово в сообщении, и
 * разбирать вывод внешней команды по exit-коду бессмысленно. */
typedef enum {
    CONLOG_LEVEL_NORMAL = 0,
    CONLOG_LEVEL_WARN,          /* warn, warning */
    CONLOG_LEVEL_ERROR,         /* error, err, fail, critical, fatal, panic */
    CONLOG_LEVEL_INFO,          /* info */
    CONLOG_LEVEL_DEBUG          /* debug, trace */
} ConLogLevel;

/* Одна строка вывода. Хранится копия текста — указатель на буфер
 * процесса недолговечен, а копия нужна для прокрутки назад. */
typedef struct {
    char       *text;
    ConLogLevel level;
    guint64     seq;        /* порядковый номер: монотонный, для «новое» */
} ConLogLine;

typedef struct _ConLogBuffer ConLogBuffer;

/* Лимит строк по умолчанию и границы, в которые он зажимается. */
#define CONLOG_DEFAULT_LINES  200
#define CONLOG_MIN_LINES       20
#define CONLOG_MAX_LINES      5000

/* Вырезать escape-последовательности и управляющие символы.
 *
 * Работает по ЯВНОЙ длине, а не по strlen: вход приходит из
 * g_io_channel_read_chars() и не NUL-терминирован. Возвращает новую
 * строку в g_malloc(), её нужно освободить через g_free(). */
char *conlog_strip_ansi(const char *text, gssize len);

/* Включить вырезание escape-последовательностей. ВАЖНО: чистка
 * применяется к СОБРАННОЙ строке, а не к сырой порции. Иначе CSI,
 * разорванный границей порции («ESC[» в одной, «31m» в следующей),
 * терял бы смысл и на экране оставался бы мусор «31m». */
void conlog_buffer_set_strip_ansi(ConLogBuffer *b, gboolean on);

/* Какие строки помещаются в окно. Чистая арифметика, вынесенная из
 * отрисовки, чтобы её можно было проверить тестом. */
typedef struct {
    guint first;      /* индекс первой рисуемой строки */
    guint count;      /* сколько строк помещается */
} ConLogView;

ConLogView conlog_visible_lines(guint total, int first_row_y, int step,
                                int height, int scroll_top);

ConLogBuffer *conlog_buffer_new(guint max_lines);
void          conlog_buffer_free(ConLogBuffer *b);

/* Лимит строк; при превышении хвост отбрасывается. */
void      conlog_set_max_lines(ConLogBuffer *b, guint max_lines);
guint     conlog_max_lines(const ConLogBuffer *b);
guint     conlog_len(const ConLogBuffer *b);
guint64   conlog_total_seen(const ConLogBuffer *b);

/* Добавить одну строку. Если буфер полон, самая старая выбрасывается. */
void      conlog_append(ConLogBuffer *b, const char *text, ConLogLevel level);

/* Добавить сырой кусок вывода команды: разбивает на строки сам.
 *
 * Ключевая часть контракта: кусок может прийти по ЛЮБОЙ границе — посреди
 * слова, посреди строки, несколькими строками сразу. Незавершённый хвост
 * не показывается и запоминается до следующего вызова. Без этого
 * journalctl -f, режущий строки по 4096 байт, давал бы на экране
 * обрывки слов. */
void      conlog_append_chunk(ConLogBuffer *b, const char *data, gssize len);

/* Дописать незавершённый хвост как последнюю строку. Вызывается при
 * остановке команды: иначе последняя строка без перевода строки
 * потерялась бы. */
void      conlog_flush_pending(ConLogBuffer *b);

/* Есть ли недописанная строка. */
gboolean  conlog_has_pending(const ConLogBuffer *b);

/* Доступ к строке по индексу; вне диапазона — NULL. */
const ConLogLine *conlog_get(const ConLogBuffer *b, guint index);
const char *conlog_text(const ConLogBuffer *b, guint index);
ConLogLevel conlog_level(const ConLogBuffer *b, guint index);

/* Очистить всё, но сохранить счётчик total_seen: он нужен, чтобы
 * «новые строки» не переиграли с нуля после очистки. */
void      conlog_clear(ConLogBuffer *b);

/* Классификация строки по её тексту. Порядок проверок важен: «error»
 * внутри «terror» — ложное срабатывание, поэтому сравнение идёт по
 * границам слова, а не через strstr. */
ConLogLevel conlog_classify(const char *text);

/* Цвет уровня в формате rgba(r,g,b,a), как его ждёт conf_dbl-ключи
 * плагинов xscreenlets. Диапазоны 0..1. */
void        conlog_level_color(ConLogLevel lvl, gdouble rgba[4]);

/* Сколько символов занимает строка в пикселях при данном шрифте —
 * вынесено, чтобы тест мог проверить расчёт без X. */
int         conlog_text_width(const char *font_desc, const char *text);

/* Высота строки по шрифту, в пикселях (не в единицах Pango!). */
int         conlog_line_height(const char *font_desc);

/* Высота ОДНОЙ строки текста по Pango: нужна для зоны заголовка, где
 * высота строки журнала не подходит — заголовок рисуется выше и
 * разделитель под ним не должен попадать на глифы. */
int         conlog_text_height(const char *font_desc, const char *text);

/* Ascender и descender по МЕТРИКАМ ШРИФТА (pango_font_get_metrics),
 * а не по прямоугольнику layout. Extents отсчитываются от верха строки
 * и ничего не говорят о базовой линии, поэтому читать ascent из
 * logical.y — ошибка, дающая 1 вместо ~10.
 *
 * Нужны, чтобы поставить базовую линию метки так, чтобы её descender
 * гарантированно не дошёл до разделителя под заголовком. */
int         conlog_text_ascent(const char *font_desc, const char *text);
int         conlog_text_descent(const char *font_desc, const char *text);



/* Нужна ли прокрутка: строк больше, чем влезет в окно. */
gboolean    conlog_needs_scroll(const ConLogBuffer *b, int window_height,
                                int first_row_y, int line_step);

#endif /* CONLOG_CORE_H */
