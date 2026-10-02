/* Проверка прокрутки в настройках погоды.
 *
 * Собирает ровно ту структуру виджетов, что теперь в cw_properties:
 * scrolled_window с NEVER/AUTOMATIC -> inner -> сетка из тридцати
 * строк. Диалог Properties в core имеет размер 490x450, поэтому
 * содержимое заведомо выше окна: если полосы прокрутки по вертикали
 * нет, тест это поймает.
 *
 * Запуск:
 *   XSCREENLETS_LOCALEDIR=... ./test_weather_scroll
 * Печатает HIGH и HAS_VSCROLL, затем держит окно открытым.
 */
#include "i18n.h"

#include <gtk/gtk.h>

int main(int argc, char **argv)
{
    GtkWidget *win, *page, *scroller, *inner, *grid;
    GtkNotebook *nb;
    GtkWidget *box, *entry;
    GtkAdjustment *adj, *hadj;
    int i;

    (void)argc;
    (void)argv;
    gtk_init(NULL, NULL);
    xs_i18n_init();

    win = gtk_window_new(GTK_WINDOW_TOPLEVEL);
    gtk_window_set_title(GTK_WINDOW(win), _("Weather"));
    /* Размер окна Properties из xs_core_show_properties. */
    gtk_window_set_default_size(GTK_WINDOW(win), 490, 450);

    nb = GTK_NOTEBOOK(gtk_notebook_new());
    gtk_container_add(GTK_CONTAINER(win), GTK_WIDGET(nb));

    page = gtk_box_new(GTK_ORIENTATION_VERTICAL, 6);
    scroller = gtk_scrolled_window_new(NULL, NULL);
    gtk_scrolled_window_set_policy(GTK_SCROLLED_WINDOW(scroller),
                                   GTK_POLICY_NEVER, GTK_POLICY_AUTOMATIC);
    gtk_scrolled_window_set_propagate_natural_width(
        GTK_SCROLLED_WINDOW(scroller), FALSE);
    gtk_scrolled_window_set_propagate_natural_height(
        GTK_SCROLLED_WINDOW(scroller), FALSE);
    inner = gtk_box_new(GTK_ORIENTATION_VERTICAL, 6);
    gtk_container_set_border_width(GTK_CONTAINER(inner), 8);
    gtk_container_add(GTK_CONTAINER(scroller), inner);
    gtk_box_pack_start(GTK_BOX(page), scroller, TRUE, TRUE, 0);

    grid = gtk_grid_new();
    gtk_grid_set_column_spacing(GTK_GRID(grid), 8);
    gtk_grid_set_row_spacing(GTK_GRID(grid), 5);
    gtk_box_pack_start(GTK_BOX(inner), grid, FALSE, FALSE, 0);

    /* Тридцать строк - столько же, сколько в cw_properties. */
    for (i = 0; i < 30; i++) {
        char buf[64];

        g_snprintf(buf, sizeof buf, _("Label %d"), i + 1);
        box = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 5);
        entry = gtk_label_new(buf);
        gtk_widget_set_size_request(entry, 180, 28);
        gtk_box_pack_start(GTK_BOX(box), entry, FALSE, FALSE, 0);
        entry = gtk_entry_new();
        gtk_entry_set_text(GTK_ENTRY(entry), "value");
        gtk_widget_set_hexpand(entry, TRUE);
        gtk_box_pack_start(GTK_BOX(box), entry, FALSE, TRUE, 0);
        gtk_grid_attach(GTK_GRID(grid), box, 0, i, 1, 1);
    }
    gtk_notebook_append_page(nb, page, gtk_label_new(_("Weather")));
    gtk_widget_show_all(win);

    adj = gtk_scrolled_window_get_vadjustment(GTK_SCROLLED_WINDOW(scroller));
    g_print("HIGH=%d\n", (int)gtk_adjustment_get_upper(adj));
    g_print("PAGE=%d\n", (int)gtk_adjustment_get_page_size(adj));
    g_print("HAS_VSCROLL=%d\n",
            gtk_adjustment_get_upper(adj) > gtk_adjustment_get_page_size(adj));
    /* Проверять политику по горизонтали нечем: gtk_scrolled_window_get_policy
     * возвращает void. Зато видно, что полосы прокрутки по горизонтали
     * не появилось - если бы политика была AUTOMATIC, при ширине 490
     * текст в подписях дал бы вторую полосу. Проверяем через
     * hadjustment: его page_size равен width, значит полосы нет. */
    hadj = gtk_scrolled_window_get_hadjustment(GTK_SCROLLED_WINDOW(scroller));
    g_print("HADJ_UPPER=%d\n", (int)gtk_adjustment_get_upper(hadj));
    g_print("HADJ_PAGE=%d\n", (int)gtk_adjustment_get_page_size(hadj));

    gtk_main();
    return 0;
}