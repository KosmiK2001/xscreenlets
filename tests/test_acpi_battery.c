/* Тест acpi_battery_core на фикстуре sysfs: проверяем все состояния,
 * которые обязан различать applet. Не модулем — в этом проекте тесты
 * собираются отдельной программой и печатают факты.
 *
 * Фикстуры повторяют раскладку настоящего /sys/class/power_supply:
 * каждый источник — каталог с файлами type/status/capacity/…
 */
#include <stdio.h>
#include <string.h>
#include <glib.h>

#include "acpi_battery_core.h"

static int failed;

static void check_str(const char *what, const char *got, const char *want)
{
    if ((got == NULL) != (want == NULL) || (got && strcmp(got, want) != 0)) {
        printf("  FAIL %-28s got='%s' want='%s'\n", what,
               got ? got : "(null)", want ? want : "(null)");
        failed++;
    } else {
        printf("  ok   %-28s '%s'\n", what, got ? got : "(null)");
    }
}

static void check_int(const char *what, gint got, gint want)
{
    if (got != want) {
        printf("  FAIL %-28s got=%d want=%d\n", what, got, want);
        failed++;
    } else {
        printf("  ok   %-28s %d\n", what, got);
    }
}

static void check_state(const char *what, AcpiBatteryState got,
                        AcpiBatteryState want)
{
    if (got != want) {
        printf("  FAIL %-28s got=%d want=%d\n", what, got, want);
        failed++;
    } else {
        printf("  ok   %-28s %d\n", what, got);
    }
}

static AcpiBattery *only(AcpiBatteryList *l)
{
    if (!l || l->items->len == 0)
        return NULL;
    return g_ptr_array_index(l->items, 0);
}

int main(int argc, char **argv)
{
    const char *root = argc > 1 ? argv[1] : NULL;
    /* Корень фикстур quirk-проверок: каталоги q1..q8 лежат внутри. */
    const char *root_base = argc > 2 ? argv[2] : root;
    AcpiBatteryList *list;
    AcpiBattery *bat;

    printf("== parse_status ==\n");
    check_state("Charging",  acpi_battery_parse_status("Charging"),
                ACPI_BATTERY_CHARGING);
    check_state("Discharging", acpi_battery_parse_status("Discharging"),
                ACPI_BATTERY_DISCHARGING);
    check_state("Full", acpi_battery_parse_status("Full"),
                ACPI_BATTERY_FULL);
    /* Not charging — отдельное состояние, а не Full: при заряде ниже 90 %
     * батарея продолжает разряжаться, и приравнивать её к «заряжена»
     * значит врать. Решение принимает quirk-правило по проценту. */
    check_state("Not charging", acpi_battery_parse_status("Not charging"),
                ACPI_BATTERY_PENDING_CHARGE);
    check_state("Unknown", acpi_battery_parse_status("Unknown"),
                ACPI_BATTERY_UNKNOWN);
    check_state("empty", acpi_battery_parse_status(""), ACPI_BATTERY_UNKNOWN);
    check_state("NULL", acpi_battery_parse_status(NULL), ACPI_BATTERY_UNKNOWN);
    /* регистр плавает между драйверами */
    check_state("lowercase charging", acpi_battery_parse_status("charging"),
                ACPI_BATTERY_CHARGING);

    printf("== format_minutes ==\n");
    /* Оригинал печатал "%02i:%02i", то есть и для часа ноль —
     * ведущий ноль. Формат менять незачем: тема и шрифт рассчитаны
     * на такую ширину. */
    {
        /* Результат освобождаем: под ASan тест обязан быть чистым
         * так же, как core, иначе утечка здесь маскирует утечку там. */
        char *a = acpi_battery_format_minutes(95);
        char *b = acpi_battery_format_minutes(600);
        char *c = acpi_battery_format_minutes(-1);
        check_str("95 -> 01:35",  a, "01:35");
        check_str("600 -> 10:00", b, "10:00");
        check_str("-1 -> NA",     c, "   NA");
        g_free(a); g_free(b); g_free(c);
    }

    printf("== list_read: пустой корень ==\n");
    list = acpi_battery_list_read("/nonexistent/power_supply");
    if (list != NULL) {
        printf("  FAIL отсутствующий каталог должен дать NULL\n");
        failed++;
    } else {
        printf("  ok   отсутствующий каталог -> NULL (не ошибка)\n");
    }
    /* NULL = батареи нет. Это НЕ повод падать: на сервере так и будет. */
    printf("  ok   NULL означает «источников нет», applet покажет No battery\n");

    printf("== list_read: батарея разряжается ==\n");
    list = acpi_battery_list_read(root);
    bat = only(list);
    if (!bat) {
        printf("  FAIL фикстура не прочитана\n");
        failed++;
    } else {
        check_str("name", bat->name, "BAT0");
        check_str("type", bat->type, "Battery");
        check_state("state", bat->state, ACPI_BATTERY_DISCHARGING);
        check_int("percent", bat->percent, 42);
        /* 30Wh полной, 12.6Wh сейчас => 42%; power_now 10W =>
         * 12.6/10*60 = 75 минут */
        check_int("minutes_left", bat->minutes_left, 75);
    }

    printf("== primary: при нескольких источниках ==\n");
    {
        AcpiBattery *p = acpi_battery_list_primary(list);
        check_str("выбран BAT0, не hidpp", p ? p->name : NULL, "BAT0");
    }
    acpi_battery_list_free(list);

    printf("== quirk-правила на фикстурах ==\n");

    /* Каждая фикстура — отдельный каталог рядом с BAT0: quirk-правила
     * проверяются на настоящих файлах sysfs, а не на подставленных
     * значениях структуры. Иначе тест проверит сам себя. */

    /* Q1: status=Charging при current_now<0. Драйверы axp20x пишут так при
     * разряде: состояние врёт, ток говорит правду. */
    {
        char *root = g_build_filename(root_base, "q1", NULL);
        list = acpi_battery_list_read(root);
        bat = only(list);
        if (!bat) { printf("  FAIL q1 не прочитан\n"); failed++; }
        else {
            check_str("q1 status в файле", bat->status, "Charging");
            check_int("q1 current_now", bat->current_now, -500000);
            check_state("q1 -> разряжается", bat->state,
                        ACPI_BATTERY_DISCHARGING);
        }
        acpi_battery_list_free(list); g_free(root);
    }

    /* Q2: заряд 95 % со статусом Discharging -> Full (батарея полна, но
     * драйвер продолжает писать discharging). */
    {
        char *root = g_build_filename(root_base, "q2", NULL);
        list = acpi_battery_list_read(root);
        bat = only(list);
        if (!bat) { printf("  FAIL q2 не прочитан\n"); failed++; }
        else {
            check_int("q2 percent", bat->percent, 95);
            check_state("q2 -> заряжена", bat->state, ACPI_BATTERY_FULL);
        }
        acpi_battery_list_free(list); g_free(root);
    }

    /* Q3: capacity=0 + capacity_level=Low. Ядро пишет 0, когда процент
     * посчитать не смогло. 0 без fallback дал бы «заряд кончился» и
     * аларм на батарее с нормальным зарядом. */
    {
        char *root = g_build_filename(root_base, "q3", NULL);
        list = acpi_battery_list_read(root);
        bat = only(list);
        if (!bat) { printf("  FAIL q3 не прочитан\n"); failed++; }
        else {
            check_int("q3 percent из level Low", bat->percent, 10);
            check_state("q3 -> разряжается", bat->state,
                        ACPI_BATTERY_DISCHARGING);
        }
        acpi_battery_list_free(list); g_free(root);
    }

    /* Q4: capacity=0 + capacity_level=Unknown. Данных нет, и подставлять
     * ноль нельзя: получится аларм на «заряд кончился». */
    {
        char *root = g_build_filename(root_base, "q4", NULL);
        list = acpi_battery_list_read(root);
        bat = only(list);
        if (!bat) { printf("  FAIL q4 не прочитан\n"); failed++; }
        else {
            check_int("q4 percent неизвестен", bat->percent, -1);
            check_state("q4 state как в файле", bat->state,
                        ACPI_BATTERY_DISCHARGING);
        }
        acpi_battery_list_free(list); g_free(root);
    }

    /* Q5: Not charging при 40 % -> разряжается, а не «заряжена».
     * Сеть подключена, зарядка не идёт, батарея всё равно садится. */
    {
        char *root = g_build_filename(root_base, "q5", NULL);
        list = acpi_battery_list_read(root);
        bat = only(list);
        if (!bat) { printf("  FAIL q5 не прочитан\n"); failed++; }
        else {
            check_state("q5 -> разряжается", bat->state,
                        ACPI_BATTERY_DISCHARGING);
        }
        acpi_battery_list_free(list); g_free(root);
    }

    /* Q6: Not charging при 95 % -> заряжена. Часть устройств вечно держит
     * этот статус, поэтому решает процент. */
    {
        char *root = g_build_filename(root_base, "q6", NULL);
        list = acpi_battery_list_read(root);
        bat = only(list);
        if (!bat) { printf("  FAIL q6 не прочитан\n"); failed++; }
        else {
            check_state("q6 -> заряжена", bat->state, ACPI_BATTERY_FULL);
        }
        acpi_battery_list_free(list); g_free(root);
    }

    /* Q7: present=no. Батарейка извлечена: applet обязан это отличать от
     * «заряд кончился» — здесь состояние NOT_PRESENT, а не 0 %. */
    {
        char *root = g_build_filename(root_base, "q7", NULL);
        list = acpi_battery_list_read(root);
        bat = only(list);
        if (!bat) { printf("  FAIL q7 не прочитан\n"); failed++; }
        else {
            check_state("q7 -> батарейки нет", bat->state,
                        ACPI_BATTERY_NOT_PRESENT);
        }
        acpi_battery_list_free(list); g_free(root);
    }

    /* Q8: Unknown при 92 % -> заряжена. Батарея под 100 % и не пишет
     * ничего внятное: UPower считает это полной. */
    {
        char *root = g_build_filename(root_base, "q8", NULL);
        list = acpi_battery_list_read(root);
        bat = only(list);
        if (!bat) { printf("  FAIL q8 не прочитан\n"); failed++; }
        else {
            check_state("q8 -> заряжена", bat->state, ACPI_BATTERY_FULL);
        }
        acpi_battery_list_free(list); g_free(root);
    }

    printf("\n%s: %d провалов\n", failed ? "ЕСТЬ ОШИБКИ" : "ВСЁ ЗЕЛЁНОЕ", failed);
    return failed ? 1 : 0;
}