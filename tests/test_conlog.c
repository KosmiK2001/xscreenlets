/* Тесты conlog_core: границы порций, лимит строк, разбор уровней. */
#include <glib.h>
#include <stdio.h>
#include <string.h>
#include "conlog_core.h"

static int checks;
static int failures;

static void check(gboolean ok, const char *what)
{
    checks++;
    if (!ok) {
        failures++;
        printf("  FAIL  %s\n", what);
    }
}

static void t_basic(void)
{
    printf("=== базовые операции ===\n");
    ConLogBuffer *b = conlog_buffer_new(100);

    check(conlog_len(b) == 0, "новый буфер пуст");
    check(conlog_max_lines(b) == 100, "лимит из конструктора");

    conlog_append(b, "строка один", CONLOG_LEVEL_NORMAL);
    conlog_append(b, "строка два", CONLOG_LEVEL_ERROR);
    check(conlog_len(b) == 2, "две строки добавлены");
    check(g_strcmp0(conlog_text(b, 0), "строка один") == 0,
          "текст первой строки сохранён");
    check(conlog_level(b, 1) == CONLOG_LEVEL_ERROR,
          "уровень второй строки — ошибка");
    check(conlog_text(b, 2) == NULL, "индекс за пределами даёт NULL");
    check(conlog_get(b, 999) == NULL, "get за пределами даёт NULL");

    conlog_buffer_free(b);
    printf("  ok\n");
}

static void t_chunk_split(void)
{
    printf("=== порция режется по переводу строки ===\n");
    ConLogBuffer *b = conlog_buffer_new(100);

    conlog_append_chunk(b, "одна\nдва\nтри\n", -1);
    check(conlog_len(b) == 3, "три строки из трёх переводов");
    check(g_strcmp0(conlog_text(b, 0), "одна") == 0, "строка 1");
    check(g_strcmp0(conlog_text(b, 1), "два") == 0, "строка 2");
    check(g_strcmp0(conlog_text(b, 2), "три") == 0, "строка 3");
    check(!conlog_has_pending(b), "хвоста нет — всё завершено");

    conlog_buffer_free(b);
    printf("  ok\n");
}

/* Главный случай: порция приходит посреди строки. */
static void t_chunk_boundary(void)
{
    printf("=== граница порции посреди строки ===\n");
    ConLogBuffer *b = conlog_buffer_new(100);

    conlog_append_chunk(b, "первая\nвто", -1);   /* до конца строки */
    check(conlog_len(b) == 1, "показана только завершённая строка");
    check(conlog_has_pending(b), "хвост «вто» ждёт продолжения");
    check(conlog_text(b, 1) == NULL, "незавершённая строка НЕ показана");

    /* Порция доносит «вторая» и добавляет «третью»: всего 3 строки. */
    conlog_append_chunk(b, "рая\nтретья\n", -1);
    check(conlog_len(b) == 3, "после склейки три строки");
    check(g_strcmp0(conlog_text(b, 1), "вторая") == 0,
          "строка склеена из двух порций без разрыва");
    check(g_strcmp0(conlog_text(b, 2), "третья") == 0, "третья строка на месте");
    check(!conlog_has_pending(b), "хвоста больше нет");

    conlog_buffer_free(b);
    printf("  ok\n");
}

static void t_chunk_byte_at_a_time(void)
{
    printf("=== вывод по одному байту ===\n");
    ConLogBuffer *b = conlog_buffer_new(100);
    const char *src = "alpha\nbeta\ngamma\n";

    for (gsize i = 0; i < strlen(src); i++)
        conlog_append_chunk(b, src + i, 1);

    check(conlog_len(b) == 3, "три строки из побайтовой передачи");
    check(g_strcmp0(conlog_text(b, 0), "alpha") == 0, "alpha цела");
    check(g_strcmp0(conlog_text(b, 1), "beta") == 0, "beta цела");
    check(g_strcmp0(conlog_text(b, 2), "gamma") == 0, "gamma цела");
    check(!conlog_has_pending(b), "хвоста нет");

    conlog_buffer_free(b);
    printf("  ok\n");
}

static void t_crlf(void)
{
    printf("=== CRLF ===\n");
    ConLogBuffer *b = conlog_buffer_new(100);

    conlog_append_chunk(b, "первая\r\nвторая\r\n", -1);
    check(conlog_len(b) == 2, "две строки");
    check(g_strcmp0(conlog_text(b, 0), "первая") == 0,
          "CR убран из конца строки");
    check(strchr(conlog_text(b, 0), '\r') == NULL, "нет символа CR в строке");
    check(g_strcmp0(conlog_text(b, 1), "вторая") == 0, "вторая тоже без CR");

    conlog_buffer_free(b);
    printf("  ok\n");
}

static void t_no_trailing_newline(void)
{
    printf("=== вывод без перевода в конце ===\n");
    ConLogBuffer *b = conlog_buffer_new(100);

    conlog_append_chunk(b, "есть\nнет перевода", -1);
    check(conlog_len(b) == 1, "только завершённая строка");
    check(conlog_has_pending(b), "хвост ждёт");

    conlog_flush_pending(b);
    check(conlog_len(b) == 2, "после flush хвост стал строкой");
    check(g_strcmp0(conlog_text(b, 1), "нет перевода") == 0,
          "хвост сохранён целиком");
    check(!conlog_has_pending(b), "хвоста больше нет");

    conlog_buffer_free(b);
    printf("  ok\n");
}

static void t_empty_lines(void)
{
    printf("=== пустые строки ===\n");
    ConLogBuffer *b = conlog_buffer_new(100);

    conlog_append_chunk(b, "\n\nсередина\n\n", -1);
    check(conlog_len(b) == 4, "пустые строки считаются, а не теряются");
    check(g_strcmp0(conlog_text(b, 0), "") == 0, "пустая строка пустая");
    check(g_strcmp0(conlog_text(b, 2), "середина") == 0, "середина на месте");

    conlog_buffer_free(b);
    printf("  ok\n");
}

static void t_limit(void)
{
    printf("=== лимит строк ===\n");
    ConLogBuffer *b = conlog_buffer_new(5);

    for (int i = 0; i < 20; i++) {
        char *s = g_strdup_printf("строка %d", i);
        conlog_append(b, s, CONLOG_LEVEL_NORMAL);
        g_free(s);
    }
    check(conlog_len(b) == 5, "буфер не растёт сверх лимита");
    check(g_strcmp0(conlog_text(b, 0), "строка 15") == 0,
          "остались последние 5 строк, а не первые");
    check(g_strcmp0(conlog_text(b, 4), "строка 19") == 0, "последняя на месте");
    check(conlog_total_seen(b) == 20,
          "счётчик всего прочитанного не урезан лимитом");

    /* Уменьшение лимита должно подрезать сразу. */
    conlog_set_max_lines(b, 2);
    check(conlog_len(b) == 2, "лимит уменьшен — буфер подрезан сразу");
    check(g_strcmp0(conlog_text(b, 0), "строка 18") == 0,
          "остались самые свежие");

    conlog_set_max_lines(b, 1);
    check(conlog_max_lines(b) == 1,
          "ядро держит малый лимит: MIN_LINES — ограничение GUI, не ядра");
    conlog_set_max_lines(b, 999999);
    check(conlog_max_lines(b) == CONLOG_MAX_LINES, "лимит зажат максимумом");

    conlog_buffer_free(b);
    printf("  ok\n");
}

static void t_clear(void)
{
    printf("=== очистка ===\n");
    ConLogBuffer *b = conlog_buffer_new(100);

    conlog_append_chunk(b, "одна\nдве\n", -1);
    conlog_append_chunk(b, "хвост", -1);
    check(conlog_total_seen(b) == 2, "две строки до очистки");

    conlog_clear(b);
    check(conlog_len(b) == 0, "буфер пуст после очистки");
    check(!conlog_has_pending(b), "недописанный хвост тоже сброшен");
    check(conlog_total_seen(b) == 2,
          "счётчик прочитанного пережил очистку");

    conlog_append(b, "новая", CONLOG_LEVEL_NORMAL);
    check(conlog_get(b, 0)->seq == 2,
          "seq новой строки продолжает счёт, а не начинает с нуля");

    conlog_buffer_free(b);
    printf("  ok\n");
}

static void t_classify(void)
{
    printf("=== разбор уровней ===\n");

    check(conlog_classify("error: something broke") == CONLOG_LEVEL_ERROR,
          "«error:» — ошибка");
    check(conlog_classify("kernel: EXT4-fs error") == CONLOG_LEVEL_ERROR,
          "ошибка в сообщении ядра");
    check(conlog_classify("CRITICAL: cannot continue") == CONLOG_LEVEL_ERROR,
          "CRITICAL — ошибка");
    check(conlog_classify("segfault at 0x0") == CONLOG_LEVEL_NORMAL,
          "«segfault» без служебного слова — обычная строка");
    check(conlog_classify("warning: deprecated option") == CONLOG_LEVEL_WARN,
          "«warning:» — предупреждение");
    check(conlog_classify("WARN deprecated") == CONLOG_LEVEL_WARN,
          "WARN заглавными — предупреждение");
    check(conlog_classify("info: listening") == CONLOG_LEVEL_INFO,
          "«info:» — информация");
    check(conlog_classify("debug: trace start") == CONLOG_LEVEL_DEBUG,
          "«debug:» — отладка");
    check(conlog_classify("eth0: link is up") == CONLOG_LEVEL_NORMAL,
          "обычная строка без служебных слов");

    /* Границы слова: это главная ловушка наивного поиска. */
    check(conlog_classify("terror in the code") != CONLOG_LEVEL_ERROR,
          "«terror» — не «error»");
    check(conlog_classify("terrain mapping ok") != CONLOG_LEVEL_ERROR,
          "«terrain» — не «err»");
    check(conlog_classify("warningless setup") != CONLOG_LEVEL_WARN,
          "«warningless» — не «warn»");
    check(conlog_classify("errors=0 count") != CONLOG_LEVEL_ERROR,
          "«errors=» — не «error»: справа знак «=»");
    check(conlog_classify("") == CONLOG_LEVEL_NORMAL, "пустая строка — обычная");
    check(conlog_classify(NULL) == CONLOG_LEVEL_NORMAL, "NULL — обычная");

    printf("  ok\n");
}

static void t_colors(void)
{
    gdouble c[4];
    printf("=== цвета уровней ===\n");

    conlog_level_color(CONLOG_LEVEL_ERROR, c);
    check(c[0] > c[1] && c[0] > c[2], "ошибка — преимущественно красная");
    conlog_level_color(CONLOG_LEVEL_WARN, c);
    check(c[0] > 0.8 && c[1] > 0.5 && c[2] < 0.5, "предупреждение — жёлтое");
    conlog_level_color(CONLOG_LEVEL_NORMAL, c);
    check(c[0] == c[1] && c[1] == c[2], "обычная строка — серая");
    conlog_level_color(CONLOG_LEVEL_ERROR, NULL);   /* не должно падать */

    printf("  ok\n");
}

static void t_metrics(void)
{
    printf("=== метрики текста ===\n");
    int h = conlog_line_height("Monospace 9");
    int h_big = conlog_line_height("Monospace 20");
    int w = conlog_text_width("Monospace 9", "несколько символов");
    int w_more = conlog_text_width("Monospace 9",
                                   "намного больше символов здесь");

    check(h >= 8, "высота строки разумная (не 0 и не тысячи)");
    check(h < 1000, "высота в пикселях, а не в единицах Pango");
    check(h_big > h, "крупный шрифт выше мелкого");
    check(w > 0, "ширина непустой строки положительна");
    check(w_more > w, "длинная строка шире короткой");
    check(conlog_text_width("Monospace 9", "") == 0, "пустая строка — нулевая ширина");
    check(conlog_text_width("Monospace 9", NULL) == 0, "NULL — нулевая ширина");

    printf("  ok\n");
}

static void t_scroll(void)
{
    ConLogBuffer *b = conlog_buffer_new(100);
    printf("=== нужна ли прокрутка ===\n");

    for (int i = 0; i < 3; i++)
        conlog_append(b, "строка", CONLOG_LEVEL_NORMAL);
    /* 4 + 3*14 = 46 <= 100 — влезает; 46 > 50 — влезает;
     * в окно 40 уже нет. */
    check(!conlog_needs_scroll(b, 100, 4, 14),
          "3 строки в окно 100px влезают");
    check(!conlog_needs_scroll(b, 50, 4, 14),
          "3 строки в окно 50px ещё влезают");
    check(conlog_needs_scroll(b, 40, 4, 14),
          "3 строки в окно 40px не влезают");
    check(!conlog_needs_scroll(NULL, 50, 4, 14), "NULL-буфер не требует");
    check(!conlog_needs_scroll(b, 50, 4, 0), "нулевой шаг не считается");

    conlog_buffer_free(b);
    printf("  ok\n");
}

static void t_null_safety(void)
{
    printf("=== NULL-безопасность ===\n");

    conlog_buffer_free(NULL);
    conlog_clear(NULL);
    conlog_append(NULL, "x", CONLOG_LEVEL_NORMAL);
    conlog_append_chunk(NULL, "x", 1);
    conlog_flush_pending(NULL);
    conlog_set_max_lines(NULL, 10);
    check(conlog_len(NULL) == 0, "conlog_len(NULL) — 0");
    check(conlog_max_lines(NULL) == 0, "conlog_max_lines(NULL) — 0");
    check(conlog_total_seen(NULL) == 0, "conlog_total_seen(NULL) — 0");
    check(conlog_text(NULL, 0) == NULL, "conlog_text(NULL) — NULL");
    check(!conlog_has_pending(NULL), "conlog_has_pending(NULL) — FALSE");

    printf("  ok\n");
}

/* Реальный сценарий: dmesg-подобный поток, где строки приходят
 * неравномерно, а буфер ограничен. */
static void t_realistic_stream(void)
{
    ConLogBuffer *b = conlog_buffer_new(50);
    const char *chunks[] = {
        "[    0.000000] Linux version 6.18",
        " [    0.000001] Command line: BOOT_IMAGE=/vmlinuz root=/dev/sda\n",
        "root=/dev/sda ro\n",
        "[    1.234567] EXT4-fs error (device sda1): error 5\n"
    };
    int writes = 0;

    printf("=== поток как из реальной команды ===\n");

    for (gsize c = 0; c < G_N_ELEMENTS(chunks); c++)
        for (int step = 0; step <= (int) strlen(chunks[c]); step += 7) {
            int n = MIN(7, (int) strlen(chunks[c]) - step);

            conlog_append_chunk(b, chunks[c] + step, n);
            writes++;
        }
    conlog_flush_pending(b);

    /* ТРИ строки, а не четыре: первые два фрагмента разделены лишь
     * пробелом, между ними нет перевода строки. Ядро склеивает их в одну
     * строку — и это правильно, именно для такого случания оно и нужно.
     * Ошибка была в ожидании теста, а не в разборе. */
    check(conlog_len(b) == 3, "собрано 3 строки из рваных порций");
    check(g_str_has_prefix(conlog_text(b, 0), "[    0.000000] Linux version 6.18"),
          "строка не потеряла начало из первой порции");
    check(g_str_has_suffix(conlog_text(b, 0), "root=/dev/sda"),
          "строка не потеряла хвост на стыке порций");
    check(g_strcmp0(conlog_text(b, 1), "root=/dev/sda ro") == 0,
          "следующая строка на месте");
    check(conlog_level(b, 2) == CONLOG_LEVEL_ERROR,
          "ошибка EXT4 распознана и покрашена");

    /* Много строк при малом лимите — буфер не должен ни расти, ни падать.
     * Строки завершаются переводом строки: без него накопленный хвост
     * не является строкой и правильно не показывается. */
    for (int i = 0; i < 5000; i++) {
        char *s = g_strdup_printf("поток %d\n", i);
        conlog_append_chunk(b, s, -1);
        g_free(s);
    }
    check(conlog_len(b) == 50, "после 5000 строк буфер держит лимит");
    check(conlog_total_seen(b) == 5003, "счётчик прочитанного честный");
    check(!conlog_has_pending(b), "хвоста не осталось");
    check(g_str_has_prefix(conlog_text(b, 0), "поток 495"),
          "остались самые свежие строки, а не первые");

    conlog_buffer_free(b);
    printf("  %d записей порций\n", writes);
    printf("  ok\n");
}


/* Регрессия: буфер порции НЕ NUL-терминирован (как после
 * g_io_channel_read_chars). Сканер, полагающийся на strlen(), уходит за
 * пределы порции, теряет переводы строк и копит «хвост» в мегабайты —
 * на живом journalctl окно оставалось пустым. */
static void t_no_nul_terminator(void)
{
    char *raw;
    char *clean;
    ConLogBuffer *b = conlog_buffer_new(50);
    gsize n = 0;

    /* Порция без NUL на конце, с тремя переводами строки внутри. */
    raw = g_malloc(16);
    memcpy(raw, "aaa\nbbb\nccc\n", 12);
    n = 12;

    clean = conlog_strip_ansi(raw, (gssize) n);
    check(strcmp(clean, "aaa\nbbb\nccc\n") == 0,
          "сканер не выходит за пределы порции");
    g_free(clean);

    conlog_append_chunk(b, raw, (gssize) n);
    check(conlog_len(b) == 3, "все три строки из порции приняты");
    check(conlog_get(b, 0) && strcmp(conlog_get(b, 0)->text, "aaa") == 0,
          "первая строка цела");
    check(conlog_get(b, 2) && strcmp(conlog_get(b, 2)->text, "ccc") == 0,
          "третья строка цела");
    check(!conlog_has_pending(b), "хвост пуст — переводы строк не потеряны");

    g_free(raw);
    conlog_buffer_free(b);
    printf("  t_no_nul_terminator: ок\n");
}

/* Граница порции ровно на escape-последовательности: недописанный CSI
 * не должен проглатывать следующую строку. */
static void t_ansi_split_across_chunks(void)
{
    ConLogBuffer *b = conlog_buffer_new(50);
    char *c1, *c2;

    c1 = conlog_strip_ansi("\033[3", 4);
    check(c1 && strcmp(c1, "") == 0, "недописанный CSI пока пуст");
    g_free(c1);

    /* Продолжение, склеенное с хвостом: в ядре такая склейка
     * происходит в conlog_append_chunk(), поэтому собираем строку
     * целиком и чистим её одной. */
    c2 = conlog_strip_ansi("\033[31mкрасный\n", -1);
    check(c2 && strcmp(c2, "красный\n") == 0,
          "полная CSI убирает и остаток строки");
    g_free(c2);

    conlog_buffer_free(b);
    printf("  t_ansi_split_across_chunks: ок\n");
}

/* Регрессия: очистка применяется к СОБРАННОЙ строке. Порция с
 * недописанным CSI, пришедшая границами чтения, не должна оставлять
 * на экране мусор вида «31mкрасный». */
static void t_strip_on_assembled_line(void)
{
    ConLogBuffer *b = conlog_buffer_new(50);
    char *c;

    conlog_buffer_set_strip_ansi(b, TRUE);
    /* Первая порция обрывается на середине последовательности. */
    conlog_append_chunk(b, "\033[", 2);
    /* Вторая дописывает остаток и саму строку. */
    /* -1, а не счёт символов: порция измеряется в БАЙТАХ, и «красный»
     * — это 14 байт, а не 7. Ошибка на пару байт тихо оставляла строку
     * недописанной, conlog_get() возвращал NULL — и тест падал в segfault
     * вместо внятного сообщения. */
    conlog_append_chunk(b, "31m\xd0\xba\xd1\x80\xd0\xb0\xd1\x81\xd0\xbd\xd1\x8b\xd0\xb9\n", -1);
    check(conlog_len(b) == 1, "строка собрана из двух порций");

    if (conlog_len(b) != 1) {
        printf("  t_strip_on_assembled_line: ПРОВАЛ — строк %u, ожидалась 1\n",
               conlog_len(b));
        conlog_buffer_free(b);
        return;
    }
    c = (char *) conlog_get(b, 0)->text;
    check(strcmp(c, "красный") == 0, "CSI, разорванная порциями, убрана целиком");

    conlog_buffer_free(b);
    printf("  t_strip_on_assembled_line: ок\n");
}


/* Регрессия: char на платформе знаковый, а байты кириллицы в UTF-8
 * лежат в 0xC0..0xFF и дают отрицательные значения. Условие
 * «*p >= 0x20» на signed char выбрасывало ВЕСЬ русский текст —
 * на живом журнале это выглядело почти правильно, потому что там
 * почти нет управляющих символов, и дефект не бросался в глаза. */
static void t_utf8_survives_filter(void)
{
    ConLogBuffer *b = conlog_buffer_new(50);
    char *c;

    c = conlog_strip_ansi("\xd0\xba\xd1\x80\xd0\xb0\xd1\x81\xd0\xbd\xd1\x8b\xd0\xb9", -1);
    check(c && strlen(c) == 14, "кириллица проходит без искажений");
    g_free(c);

    /* Полная цветная последовательность вместе с русским текстом. */
    c = conlog_strip_ansi("\033[31m\xd0\xba\xd1\x80\xd0\xb0\xd1\x81\xd0\xbd\xd1\x8b\xd0\xb9\n", -1);
    check(c && strcmp(c, "\xd0\xba\xd1\x80\xd0\xb0\xd1\x81\xd0\xbd\xd1\x8b\xd0\xb9\n") == 0,
          "escape убран, кириллица и перевод строки целы");
    g_free(c);

    /* Управляющие символы при этом всё-таки вырезаются. */
    c = conlog_strip_ansi("a\001b\033c", -1);
    check(c && strcmp(c, "ab") == 0, "управляющие символы вырезаны");
    g_free(c);

    conlog_buffer_free(b);
    printf("  t_utf8_survives_filter: ок\n");
}


/* Регрессия отрисовки: при полном буфере и scroll_top == 0 ДОЛЖНА
 * рисоваться первая строка. Прежняя проверка отсекала все строки
 * условием «y - step >= first_row_y» (6 >= 6), и апплет показывал
 * только подсказку «ожидание вывода…». */
static void t_visible_lines(void)
{
    ConLogView v;

    /* Полный буфер, окно 200 px, шаг 14, отступ 6. */
    v = conlog_visible_lines(200, 6, 14, 200, 0);
    check(v.first == 0, "первая строка — с начала");
    check(v.count > 0, "хотя бы одна строка рисуется");
    check(v.count == 14, "в окно 200 px помещается 14 строк при шаге 14");

    /* Прокрутка в конец. */
    /* Регрессия: базовая линия не должна зависеть от scroll_top,
     * иначе прокрутка в конец выносила последние строки за окно и
     * пользователь видел пустое окно вместо хвоста журнала. */
    v = conlog_visible_lines(200, 6, 14, 200, 199);
    check(v.first == 199, "прокрутка показывает последнюю строку");
    check(v.count == 1, "последняя строка видна, а не пустое окно");

    /* Пустой буфер. */
    v = conlog_visible_lines(0, 6, 14, 200, 0);
    check(v.count == 0, "пустой буфер не рисует ничего");

    /* scroll_top за пределами буфона — не читаем за границей. */
    v = conlog_visible_lines(5, 6, 14, 200, 100);
    check(v.first == 4, "выход за конец зажат на последней строке");
    check(v.count == 1, "из пяти строк видна одна");

    printf("  t_visible_lines: ок\n");
}


/* Реальные формы окончаний. Матчер требует границу слова, поэтому
 * «fail» не находит «failed», а «warn» — «warned»: строки systemd
 * вида «FAILED to start …» оставались серыми, хотя это ошибки. */
static void t_level_endings(void)
{
    struct { const char *line; ConLogLevel want; } cases[] = {
        { "сен 27 xenoserver systemd[1]: FAILED to start NetworkManager",
          CONLOG_LEVEL_ERROR },
        { "сен 27 kernel: EXT4-fs error (device sda1): failed to read",
          CONLOG_LEVEL_ERROR },
        { "сен 27 dhcp[1200]: warning: lease expired",
          CONLOG_LEVEL_WARN },
        { "сен 27 kernel: CRITICAL: root filesystem readonly",
          CONLOG_LEVEL_ERROR },
        { "сен 27 app[900]: DEBUG entering dispatch loop",
          CONLOG_LEVEL_DEBUG },
        { "сен 27 xray[6928]: taking platform detour",
          CONLOG_LEVEL_NORMAL },
        /* Слова внутри других — не уровень: иначе «errorless» стал бы
         * ошибкой, а это обычное сообщение. */
        { "сен 27 app: errorless operation completed",
          CONLOG_LEVEL_NORMAL },
    };

    for (guint i = 0; i < G_N_ELEMENTS(cases); i++) {
        ConLogLevel got = conlog_classify(cases[i].line);

        check(got == cases[i].want,
              cases[i].want == CONLOG_LEVEL_NORMAL
                  ? "обычная строка не окрашивается"
                  : "уровень распознан по окончанию");
        if (got != cases[i].want)
            printf("    неверно для: %s\n", cases[i].line);
    }
    printf("  t_level_endings: ок\n");
}


/* Заголовок окна резервирует зону, и строки журнала начинаются НИЖЕ
 * неё. Проверяем конц��ольную арифметику: без заголовка first=0,
 * с заголовком 20 px строки сдвигаются, а число видимых строк
 * на ту же высоту уменьшается. */
static void t_title_zone(void)
{
    ConLogView plain = conlog_visible_lines(200, 6, 14, 200, 0);
    ConLogView titled = conlog_visible_lines(200, 6 + 20, 14, 200, 0);

    check(plain.first == 0 && titled.first == 0,
          "начало списка не сдвигается — сдвигается только рисуемая зона");
    check(plain.count > titled.count,
          "заголовок отнимает высоту у журнала");
    /* Заголовок 20 px при шаге 14 отнимает ОДНУ строку, а не две:
     * округление идёт по целой строке, а не по пикселям. */
    check(plain.count == 14 && titled.count == 13,
          "без заголовка 14 строк, с заголовком 20 px — 13");
    printf("  t_title_zone: ок\n");
}


/* Ascender и descender берутся из МЕТРИК ШРИФТА, а не из
 * pango_layout_get_extents(): extents отсчитываются от верха строки,
 * поэтому ascent = -logical.y давал 1, descender считался как вс��
 * высота, и зона заголовка выходила вдвое больше нужного — под
 * разделителем появлялась пустая строка. */
static void t_font_metrics(void)
{
    int a = conlog_text_ascent("Monospace 9", "системный журнал");
    int d = conlog_text_descent("Monospace 9", "системный журнал");
    int h = conlog_text_height("Monospace 9", "системный журнал");

    check(a > 5, "ascender — настоящая высота вверх от базовой линии");
    check(d > 0, "descender есть: в кириллице «р» и «ц» уходят вниз");
    check(h == a + d, "высота строки равна ascent + descent");
    check(a != h, "ascender не равен всей высоте — это была ошибка");

    check(conlog_text_height("Monospace 9", "") == 0,
          "пустой текст не резервирует зону");
    check(conlog_text_ascent("Monospace 9", "") == 0,
          "ascender пустого текста нулевой");
    printf("  t_font_metrics: ок\n");
}


/* ── Разбор SGR: чужой цвет вывода должен выживать ───────────────
 *
 * Раньше escape вырезался целиком, и раскраска ls --color терялась.
 * Здесь контракт: строка без SGR — один прогон со стилем строки;
 * SGR делит строку на прогоны и задаёт стиль каждому. */
static void t_sgr_plain(void)
{
    gdouble base[4] = { 0.85, 0.85, 0.85, 1.0 };
    guint n = 0;
    ConLogRun *runs = conlog_sgr_parse("hello", base, &n);

    check(runs != NULL, "разбор без SGR возвращает результат");
    check(n == 1, "строка без SGR — ровно один прогон");
    if (runs && n == 1) {
        check(g_strcmp0(runs[0].text, "hello") == 0, "текст прогона сохранён");
        check(!runs[0].style.has_fg, "свой цвет не задан");
    }
    conlog_sgr_free(runs, n);
    printf("  t_sgr_plain: ок\n");
}

static void t_sgr_splits(void)
{
    gdouble base[4] = { 0.85, 0.85, 0.85, 1.0 };
    guint n = 0;
    ConLogRun *runs =
        conlog_sgr_parse("\033[31mкрас\033[0m обычный", base, &n);

    check(runs != NULL, "разбор SGR возвращает результат");
    check(n == 2, "SGR посередине делит строку на два прогона");
    if (runs && n == 2) {
        check(g_strcmp0(runs[0].text, "крас") == 0,
              "текст до сброса без escape-последовательностей");
        check(runs[0].style.has_fg, "красный задан явно");
        check(g_strcmp0(runs[1].text, " обычный") == 0, "текст после сброса цел");
        check(!runs[1].style.has_fg, "после сброса цвет по умолчанию");
    }
    conlog_sgr_free(runs, n);
    printf("  t_sgr_splits: ок\n");
}

static void t_sgr_state_carries(void)
{
    gdouble base[4] = { 0.85, 0.85, 0.85, 1.0 };
    guint n = 0;
    ConLogRun *runs =
        conlog_sgr_parse("\033[1;32mжирный зелёный", base, &n);

    check(runs && n == 1, "комбинация 1;32 — один прогон");
    if (runs && n == 1) {
        check(runs[0].style.bold, "жирность из кода 1");
        check(runs[0].style.has_fg, "зелёный из кода 32");
        check(g_strcmp0(runs[0].text, "жирный зелёный") == 0, "текст цел");
    }
    conlog_sgr_free(runs, n);
    printf("  t_sgr_state_carries: ок\n");
}

static void t_sgr_reset_forms(void)
{
    gdouble base[4] = { 0.85, 0.85, 0.85, 1.0 };
    guint n = 0;
    ConLogRun *runs = conlog_sgr_parse("\033[39mдефолт", base, &n);

    check(runs && n == 1, "код 39 — один прогон");
    if (runs && n == 1)
        check(!runs[0].style.has_fg, "39 снимает собственный цвет");
    conlog_sgr_free(runs, n);

    runs = conlog_sgr_parse("\033[0;39;49mчисто", base, &n);
    check(runs && n == 1, "комбинация сброса — один прогон");
    conlog_sgr_free(runs, n);
    printf("  t_sgr_reset_forms: ок\n");
}

static void t_sgr_bright_and_256(void)
{
    gdouble base[4] = { 0.85, 0.85, 0.85, 1.0 };
    guint n = 0;
    ConLogRun *runs;

    runs = conlog_sgr_parse("\033[91mяркокрасный", base, &n);
    check(runs && n == 1, "код 91 разобран");
    if (runs && n == 1) {
        check(runs[0].style.has_fg, "у 91 есть свой цвет");
        check(runs[0].style.fg[0] > 0.9 && runs[0].style.fg[1] < 0.5,
              "91 красно-оранжевый, а не красный");
    }
    conlog_sgr_free(runs, n);

    runs = conlog_sgr_parse("\033[38;5;208m256", base, &n);
    check(runs && n == 1, "код 38;5;N разобран");
    if (runs && n == 1) {
        check(runs[0].style.has_fg, "у 256 есть свой цвет");
        check(runs[0].style.fg[0] > runs[0].style.fg[2],
              "оранжевый 208 красно-оранжевый");
    }
    conlog_sgr_free(runs, n);
    printf("  t_sgr_bright_and_256: ок\n");
}

static void t_sgr_truecolor(void)
{
    gdouble base[4] = { 0.85, 0.85, 0.85, 1.0 };
    guint n = 0;
    ConLogRun *runs =
        conlog_sgr_parse("\033[38;2;255;128;0mградиент", base, &n);

    check(runs && n == 1, "код 38;2;R;G;B разобран");
    if (runs && n == 1) {
        check(runs[0].style.has_fg, "truecolor задаёт свой цвет");
        check(runs[0].style.fg[0] > 0.99
              && runs[0].style.fg[1] > 0.49 && runs[0].style.fg[1] < 0.51
              && runs[0].style.fg[2] < 0.01, "цвет точно 255,128,0");
    }
    conlog_sgr_free(runs, n);
    printf("  t_sgr_truecolor: ок\n");
}

static void t_sgr_no_escape_leak(void)
{
    gdouble base[4] = { 0.85, 0.85, 0.85, 1.0 };
    guint n = 0;
    ConLogRun *runs =
        conlog_sgr_parse("\033[1mbold\033[0m plain \033[4munderline",
                         base, &n);

    check(runs && n == 3, "три прогона: bold, plain, underline");
    if (runs && n == 3) {
        check(g_strcmp0(runs[0].text, "bold") == 0, "первый прогон без мусора");
        check(g_strcmp0(runs[1].text, " plain ") == 0, "второй с пробелами");
        check(g_strcmp0(runs[2].text, "underline") == 0, "третий без мусора");
        check(runs[0].style.bold, "стиль первого прогона");
        check(!runs[1].style.bold, "стиль сброшен после 0");
        check(runs[2].style.underline, "подчёркивание в третьем");
    }
    conlog_sgr_free(runs, n);
    printf("  t_sgr_no_escape_leak: ок\n");
}

static void t_sgr_has_sgr(void)
{
    check(!conlog_has_sgr("обычный текст"), "без escape — нет SGR");
    check(conlog_has_sgr("\033[31mда"), "31m — это SGR");
    check(!conlog_has_sgr("\033]0;title\007текст"),
          "OSC-последовательность не SGR");
    printf("  t_sgr_has_sgr: ок\n");
}

static void t_sgr_empty_and_edges(void)
{
    gdouble base[4] = { 0.85, 0.85, 0.85, 1.0 };
    guint n = 0;
    ConLogRun *runs = conlog_sgr_parse("", base, &n);

    check(n == 0, "пустая строка — ни одного прогона");
    conlog_sgr_free(runs, n);

    /* SGR без единого символа текста не даёт прогона: видеть нечего,
     * а пустые прогоны в отрисовке стоили бы лишнего мерки. */
    runs = conlog_sgr_parse("\033[31m", base, &n);
    check(n == 0, "SGR без текста — ни одного прогона");
    conlog_sgr_free(runs, n);

    runs = conlog_sgr_parse("\033[31m\033[32m", base, &n);
    check(n == 0, "два SGR подряд без текста — тоже ноль");
    conlog_sgr_free(runs, n);

    /* Смена стиля при наличии текста — наоборот, делит строку. */
    runs = conlog_sgr_parse("\033[31mA\033[32mB", base, &n);
    check(runs && n == 2, "смена стиля посреди текста даёт два прогона");
    if (runs && n == 2) {
        check(g_strcmp0(runs[0].text, "A") == 0, "первый прогон");
        check(g_strcmp0(runs[1].text, "B") == 0, "второй прогон");
        /* Красный 31 = яркий доминирует по R, зелёный 32 — по G.
         * Сравнивать каналы надо по доминирующему, иначе проверка
         * проходит случайно. */
        check(runs[0].style.fg[0] > 0.9 && runs[0].style.fg[1] < 0.6,
              "первый прогон красный");
        check(runs[1].style.fg[1] > 0.9 && runs[1].style.fg[0] < 0.6,
              "второй прогон зелёный");
    }
    conlog_sgr_free(runs, n);
    printf("  t_sgr_empty_and_edges: ок\n");
}


/* ── Кэш разбивки строк (bounded) ───────────────────────────────
 *
 * Разбор SGR на каждый кадр стоил 99% CPU. Строка в буфере
 * неизменна после добавления, поэтому кэшировать разбивку можно
 * по её порядковому номеру (seq) — он монотонный и не
 * переиспользуется, так что случайных совпадений не бывает.
 *
 * Кэш обязан быть ОГРАНИЧЕН: с журналом на 5000 строк неограниченный
 * кэш съедает память процесса и не отдаёт её никогда.
 */
static void t_runcache_bounded(void)
{
    /* Лимит меньше CONLOG_RUNCACHE_MIN зажимается, поэтому беру
     * явно разрешённый минимум, а не произвольное число: иначе
     * проверка «не превышает лимит» мерила бы не кэш, а зажим. */
    const guint lim = CONLOG_RUNCACHE_MIN;
    ConLogRunCache *c = conlog_runcache_new(lim);
    gdouble base[4] = { 0.85, 0.85, 0.85, 1.0 };
    const char *font = "Sans 9";
    const ConLogLineRuns *got;
    guint peak = 0;

    check(c != NULL, "кэш создан");
    if (!c)
        return;

    check(conlog_runcache_size(c) == 0, "новый кэш пуст");

    /* Строк вчетверо больше лимита: кэш обязан остаться в лимите.
     * Без вытеснения он рос бы неограниченно. */
    for (guint64 seq = 1; seq <= lim * 4; seq++) {
        const ConLogLineRuns *r =
            conlog_runcache_get(c, seq, "\033[31mкрас\033[0m обычный",
                               base, font);
        check(r != NULL, "строка разобралась при переполнении");
        if (conlog_runcache_size(c) > peak)
            peak = conlog_runcache_size(c);
    }
    check(conlog_runcache_size(c) <= lim,
          "кэш не превышает лимит");
    check(peak <= lim, "кэш ни разу не превысил лимит по ходу");
    check(conlog_runcache_size(c) == lim,
          "после вытеснений кэш заполнен под лимит");

    /* Первая запись вытеснена: разбор повторный, но результат годен. */
    got = conlog_runcache_get(c, 1, "\033[32mзелёный\033[0m хвост",
                              base, font);
    check(got != NULL, "вытесненная строка разбирается заново, не NULL");
    check(got && got->n == 2, "зелёный прогон + обычный хвост — два");

    conlog_runcache_free(c);
    printf("  t_runcache_bounded: ок\n");
}

static void t_runcache_hit_and_width(void)
{
    ConLogRunCache *c = conlog_runcache_new(16);
    gdouble base[4] = { 0.85, 0.85, 0.85, 1.0 };
    const char *font = "Sans 9";
    const ConLogLineRuns *a, *b;

    check(c != NULL, "кэш создан");
    if (!c)
        return;

    a = conlog_runcache_get(c, 7, "\033[31mA\033[0mB", base, font);
    check(a != NULL, "первый разбор вернул прогоны");
    check(a && a->n == 2, "«A» красным и «B» обычным — два прогона");
    check(a && a->runs[0].text && a->runs[0].text[0] == 'A',
          "текст прогона «A» не содержит escape");
    check(a && a->width > 0.0, "ширина посчитана и больше нуля");
    /* Ширина каждого прогона нужна для позиционирования: без неё
     * отрисовка меряет все прогоны заново на каждом кадре, и кэш
     * избавляет только от разбора, то есть от половины нагрузки. */
    check(a && a->run_width && a->run_width[0] > 0,
          "ширина прогона «A» посчитана и больше нуля");
    check(a && a->run_width && a->run_width[1] > 0,
          "ширина прогона «B» посчитана и больше нуля");
    check(a && a->run_width
             && ABS((a->run_width[0] + a->run_width[1]) - a->width) < 0.001,
          "сумма ширин прогонов равна общей ширине строки");

    b = conlog_runcache_get(c, 7, "\033[31mA\033[0mB", base, font);
    check(a == b, "повторный запрос вернул тот же указатель (кэш сработал)");
    check(conlog_runcache_size(c) == 1,
          "повторный запрос не добавил запись");

    conlog_runcache_free(c);
    printf("  t_runcache_hit_and_width: ок\n");
}

static void t_runcache_null_safety(void)
{
    ConLogRunCache *c = conlog_runcache_new(4);
    gdouble base[4] = { 0.85, 0.85, 0.85, 1.0 };
    const ConLogLineRuns *r;

    check(conlog_runcache_new(0) != NULL || TRUE, "лимит 0 не падает");
    if (!c)
        return;

    r = conlog_runcache_get(c, 1, NULL, base, "Sans 9");
    check(r != NULL, "NULL-текст не роняет разбор");
    r = conlog_runcache_get(c, 2, "текст", NULL, "Sans 9");
    check(r != NULL, "NULL-цвет не роняет разбор");
    r = conlog_runcache_get(c, 3, "текст", base, NULL);
    check(r != NULL, "NULL-шрифт не роняет разбор");

    conlog_runcache_free(c);
    conlog_runcache_free(NULL);
    conlog_runcache_clear(NULL);
    printf("  t_runcache_null_safety: ок\n");
}

int main(void)
{
    t_basic();
    t_chunk_split();
    t_chunk_boundary();
    t_chunk_byte_at_a_time();
    t_crlf();
    t_no_trailing_newline();
    t_empty_lines();
    t_limit();
    t_clear();
    t_classify();
    t_colors();
    t_metrics();
    t_scroll();
    t_null_safety();
    t_realistic_stream();
    t_no_nul_terminator();
    t_ansi_split_across_chunks();
    t_strip_on_assembled_line();
    t_utf8_survives_filter();
    t_visible_lines();
    t_level_endings();
    t_title_zone();
    t_font_metrics();
    t_sgr_plain();
    t_sgr_splits();
    t_sgr_state_carries();
    t_sgr_reset_forms();
    t_sgr_bright_and_256();
    t_sgr_truecolor();
    t_sgr_no_escape_leak();
    t_sgr_has_sgr();
    t_sgr_empty_and_edges();
    t_runcache_bounded();
    t_runcache_hit_and_width();
    t_runcache_null_safety();
    printf("\n");
    if (failures)
        printf("CONLOG_FAIL: %d проверок, %d провалов\n", checks, failures);
    else
        printf("CONLOG_OK: %d проверок, 0 провалов\n", checks);
    return failures ? 1 : 0;
}
