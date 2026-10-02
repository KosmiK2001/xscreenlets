/* Проверка переводов контекстного меню и оконных флагов.
 *
 * Проверяет то, что нельзя увидеть в POT:
 *  - форматные строки "Add one more %s" / "Delete this %s" из правого
 *    клика, где переводится формат, а тип апплета подставляется как есть;
 *  - подписи оконных флагов из таблиц {label, what}, помеченных N_():
 *    перевод должен происходить при показе, а не в статической
 *    инициализации;
 *  - что два вызова _("Running Instances") дают одну и ту же строку:
 *    от этого зависит g_strcmp0 в tray.c, который ищет подменю
 *    по подписи пункта меню.
 *
 * Печатает по строке на msgid и PASS/FAIL.
 */
#include "i18n.h"

#include <glib.h>
#include <locale.h>
#include <stdio.h>
#include <string.h>

static int failures;

static void check(const char *id, const char *expect_ru)
{
    const char *t = gettext(id);
    int ok;

    /* Перевод считаем успешным, если строка изменилась, либо если
     * ожидание совпало (для "%1$s: %2$s" перевод совпадает с оригиналом
     * намеренно - порядок аргументов менять нельзя). */
    if (expect_ru)
        ok = !strcmp(t, expect_ru);
    else
        ok = strcmp(t, id) != 0;

    /* Обрезаем g_strlcpy, а не префиксом точности в printf: %.44s
     * задаёт МИНИМАЛЬНУЮ ширину и ничего не обрезает, так что
     * длинные msgid разъезжались бы на всю колонку. */
    char a[23], b[45];

    g_strlcpy(a, id, sizeof a);
    g_strlcpy(b, t, sizeof b);
    printf("  %-22s -> %-44s  %s\n", a, b, ok ? "РУС" : "АНГЛ");
    if (!ok)
        failures++;
}

int main(int argc, char **argv)
{
    (void)argc;
    setlocale(LC_ALL, "");
    bindtextdomain("xscreenlets", argv[1]);
    textdomain("xscreenlets");

    check("Add one more %s", "Добавить ещё %s");
    check("Delete this %s", "Удалить %s");
    check("%1$s: %2$s", "%1$s: %2$s");
    check("Lock position", "Закрепить положение");
    check("Lock", "Закрепить");
    check("Sticky", "Липкий");
    check("Widget", "Виджет");
    check("Keep above", "Поверх всех окон");
    check("Keep below", "Под всеми окнами");
    check("Window", "Окно");
    check("Running Instances", "Запущенные апплеты");

    /* Подписи блоков серий в настройках монитора диска. Раньше они
     * доходили до пользователя английским через gtk_frame_new() из
     * spec->title без перевода. */
    check("Graph label", "Подпись графика");
    check("Read text", "Текст чтения");
    check("Read history", "История чтения");
    check("Write text", "Текст записи");
    check("Write history", "История записи");
    check("Temperature text", "Текст температуры");
    check("Temperature history", "История температуры");

    /* Описания апплетов в About: берутся из XsPluginDesc, это статические
     * данные, поэтому помечены N_(), а переводятся в common.c. */
    check("Per-disk I/O and temperature history monitor",
          "Монитор диска: ввод-вывод и история температуры по каждому диску");
    check("Top processes sampled directly from /proc",
          "Список процессов, снимаемый прямо из /proc");

    /* Новая галка календаря: кнопки смены месяца в шапке. */
    check("Month buttons", "Кнопки месяца");

    /* Два вызова подряд обязаны дать одну и ту же строку: tray.c
     * сравнивает подпись пункта меню с _("Running Instances"). */
    if (strcmp(_("Running Instances"), _("Running Instances")) != 0) {
        printf("  РАЗНЫЕ РЕЗУЛЬТАТЫ ДВУХ ВЫЗОВОВ - g_strcmp0 сломается\n");
        failures++;
    }

    printf("\nпровалов: %d\n", failures);
    return failures ? 1 : 0;
}