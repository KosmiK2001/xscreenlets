/* Проверка переноса пояснений в настройках батареи.
 *
 * Вкладка «Батарея» содержит абзац из xs_prop_add_group_header. Раньше
 * он не переносился, и окно Properties (490x450) растягивалось по
 * ширине самого длинного слова в тексте. Тест повторяет структуру и
 * спрашивает у GTK фактическую ширину label.
 *
 * Печатает LABEL_WIDTH, WIN_WIDTH и WRAPPED, затем держит окно.
 */
#include "i18n.h"

#include <gtk/gtk.h>

int main(int argc, char **argv)
{
    static const char *const fields[] = {
        "Window width", "Window height", "Update interval (s)",
        "Low battery threshold (%)"
    };
    static const char *const checks[] = {
        "Show percentage", "Show remaining time"
    };
    GtkWidget *win, *nb, *page, *lbl, *entry, *cb;
    GtkNotebook *nbook;
    GtkAllocation al;
    GdkRectangle rect;
    gint lw = 0, ww = 0, wrapped = 0;
    int i;

    (void)argc;
    (void)argv;
    gtk_init(NULL, NULL);
    xs_i18n_init();

    win = gtk_window_new(GTK_WINDOW_TOPLEVEL);
    gtk_window_set_title(GTK_WINDOW(win), _("ACPI Battery"));
    gtk_window_set_default_size(GTK_WINDOW(win), 490, 450);

    nbook = GTK_NOTEBOOK(gtk_notebook_new());
    gtk_container_add(GTK_CONTAINER(win), GTK_WIDGET(nbook));

    page = gtk_box_new(GTK_ORIENTATION_VERTICAL, 4);
    gtk_container_set_border_width(GTK_CONTAINER(page), 10);

    lbl = gtk_label_new(_("Reads /sys/class/power_supply. Works with laptop "
                          "batteries and with devices that expose a battery "
                          "there (e.g. a Logitech mouse when the "
                          "hid-logitech-hidpp module is loaded). Shows "
                          "\"No battery\" when no source is found, instead "
                          "of failing."));
    /* Ровно как в xs_prop_add_group_header после правки. */
    gtk_label_set_line_wrap(GTK_LABEL(lbl), TRUE);
    gtk_label_set_line_wrap_mode(GTK_LABEL(lbl), PANGO_WRAP_WORD_CHAR);
    gtk_label_set_width_chars(GTK_LABEL(lbl), 46);
    gtk_label_set_max_width_chars(GTK_LABEL(lbl), 46);
    gtk_widget_set_halign(lbl, GTK_ALIGN_START);
    gtk_widget_set_valign(lbl, GTK_ALIGN_START);
    gtk_box_pack_start(GTK_BOX(page), lbl, FALSE, FALSE, 7);

    /* Поля как в ab_properties: ab_int_prop создаёт строку с подписью
     * шириной 180 и полем ввода, ab_bool_prop - чекбокс. Без них тест
     * проверял бы только пояснение и молчал бы про то, влезли ли поля. */
    for (i = 0; i < 4; i++) {
        GtkWidget *row = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 5);
        GtkWidget *rl = gtk_label_new(_(fields[i]));

        gtk_widget_set_halign(rl, GTK_ALIGN_START);
        gtk_widget_set_size_request(rl, 180, 28);
        gtk_box_pack_start(GTK_BOX(row), rl, FALSE, TRUE, 0);
        entry = gtk_entry_new();
        gtk_entry_set_text(GTK_ENTRY(entry), "1");
        gtk_widget_set_hexpand(entry, TRUE);
        gtk_box_pack_start(GTK_BOX(row), entry, FALSE, TRUE, 0);
        gtk_box_pack_start(GTK_BOX(page), row, FALSE, FALSE, 0);
    }
    for (i = 0; i < 2; i++) {
        cb = gtk_check_button_new_with_label(_(checks[i]));
        gtk_box_pack_start(GTK_BOX(page), cb, FALSE, FALSE, 0);
    }

    gtk_notebook_append_page(nbook, page, gtk_label_new(_("ACPI Battery")));
    gtk_widget_show_all(win);

    /* Ширины замеряем после показа: до realize GTK считает естественную
     * ширину иначе, и результат не имеет отношения к тому, что увидит
     * пользователь. */
    while (gtk_events_pending())
        gtk_main_iteration();

    /* gtk_widget_get_size в GTK3 нет, ширину берём из allocation,
     * а она заполнена только после размещения окна. */
    gtk_widget_get_allocation(lbl, &al);
    gtk_widget_get_allocation(win, &rect);
    lw = al.width;
    ww = rect.width;
    wrapped = (int)gtk_label_get_line_wrap(GTK_LABEL(lbl));

    g_print("LABEL_WIDTH=%d\n", lw);
    g_print("WIN_WIDTH=%d\n", ww);
    g_print("WRAPPED=%d\n", wrapped);
    g_print("LABEL_FITS=%d\n", lw <= ww);

    gtk_main();
    return 0;
}