/* Проверка переводов на настоящем окне GTK.
 *
 * Создаёт окно с теми же надписями, что и Properties апплета, и
 * ОСТАЁТСЯ ОТКРЫТЫМ, пока скрипт не сделает снимок. Это нужно потому,
 * что gettext читает .mo лениво - при первом вызове _(). Пока Properties
 * не открыт, файл .mo вообще не открывается процессом, и по lsof
 * судить о переводах нельзя.
 *
 * Запуск:
 *   XSCREENLETS_LOCALEDIR=build/locale ./build/test_i18n_window
 *   import -window <id> снимок.png
 */
#include "i18n.h"

#include <gtk/gtk.h>
/* GDK_WINDOW_XID объявлен в gdkx.h, а не в gtk.h: это X11-специфичный
 * макрос, и в Wayland-сессии его нет. Тест только для X11. */
#include <gdk/gdkx.h>

/* Те же строки, что и в xs_prop_row() и заголовках окон. */
static const char *const LABELS[] = {
    "Scale", "Opacity", "X-Position", "Y-Position",
    "User label", "Window", "Options", "Themes",
    "Applets", "Running", "Configs", "About",
    "No sensors found", "Network Monitor", "CPU Monitor",
};
#define N_LABELS (sizeof(LABELS) / sizeof(LABELS[0]))

int main(int argc, char **argv)
{
    GtkWidget *win;
    GtkWidget *box;
    GtkWidget *grid;
    size_t i;

    gtk_init(&argc, &argv);
    xs_i18n_init();

    g_printerr("localedir=%s\n",
               xs_i18n_localedir() ? xs_i18n_localedir() : "(null)");
    g_printerr("LANGUAGE=%s\n",
               getenv("LANGUAGE") ? getenv("LANGUAGE") : "-");

    win = gtk_window_new(GTK_WINDOW_TOPLEVEL);
    gtk_window_set_title(GTK_WINDOW(win), _("Options"));
    gtk_window_set_default_size(GTK_WINDOW(win), 360, 60 + (int)N_LABELS * 26);
    box = gtk_box_new(GTK_ORIENTATION_VERTICAL, 2);
    gtk_container_add(GTK_CONTAINER(win), box);

    grid = gtk_grid_new();
    gtk_grid_set_column_spacing(GTK_GRID(grid), 8);
    gtk_grid_set_row_spacing(GTK_GRID(grid), 4);
    gtk_box_pack_start(GTK_BOX(box), grid, FALSE, FALSE, 4);

    for (i = 0; i < N_LABELS; i++) {
        gtk_grid_attach(GTK_GRID(grid), gtk_label_new(_(LABELS[i])),
                        0, (int)i, 1, 1);
    }

    gtk_widget_show_all(win);

    /* Печатаем id окна: по нему снимок делается извне. */
    {
        GdkWindow *gw = gtk_widget_get_window(win);
        g_printerr("WINDOW=0x%lx\n", (unsigned long)GDK_WINDOW_XID(gw));
    }
    fflush(stderr);

    gtk_main();
    return 0;
}
