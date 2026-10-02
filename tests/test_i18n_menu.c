/* Проверка переводов пунктов меню: трей и контекстное меню апплета.
 *
 * Отдельный от tests/test_i18n.c: там свойства, здесь меню. Меню
 * строится в двух разных файлах (tray.c и common.c), и оба обязаны
 * быть русифицированы - на это жаловались отдельно.
 */
#include <locale.h>
#include <libintl.h>
#include <stdio.h>
#include <string.h>

#define _(S) gettext(S)

static const char *const MENU[] = {
    /* меню трея (tray.c) */
    "Running Instances", "Applet management", "Restart Applets",
    "Stop all Applets", "About", "Quit",
    /* контекстное меню апплета (common.c) */
    "Size", "Window", "Properties...", "Info...",
    /* подписи полей и заголовки секций: они передаются литералами в
     * хелперы cl_row/sen_row/nm_grid_add_label, а не через _() прямо,
     * поэтому проверяются здесь отдельно от меню. */
    "Show", "Appearance", "Layout", "Window", "Command", "Behaviour",
    "Level colours", "Strip colours (ANSI)", "Lines in buffer",
    "Per-level colours", "Keep output colours", "First-line indent",
    "Corner rounding", "Label colour", "Label font", "Offset X", "Offset Y",
    "Refresh, ms", "Value position", "Label position", "Value right-aligned",
    "Line step", "First line", "Value", "Units",
    "Start", "Reset settings", "Delete config", "Edit",
    "Delete", "Cancel", "Select", "Choose a config in .plugins",
    "Clear output", "Feeds", "Get Clock Skins", "Text settings",
    "Header font and size", "Text font and size", "Header color",
    /* для контроля: свойства */
    "Opacity", "Themes", "No sensors found",
};
#define N (int)(sizeof(MENU) / sizeof(MENU[0]))

int main(int argc, char **argv)
{
    const char *dir = argc > 1 ? argv[1] : "build/locale";
    int i;
    int untranslated = 0;

    setlocale(LC_ALL, "");
    bindtextdomain("xscreenlets", dir);
    bind_textdomain_codeset("xscreenlets", "UTF-8");
    textdomain("xscreenlets");

    for (i = 0; i < N; i++) {
        const char *r = _(MENU[i]);
        int same = (strcmp(r, MENU[i]) == 0);

        if (same)
            untranslated++;
        printf("  %-22s -> %s%s\n", MENU[i], r,
               same ? "   (НЕ ПЕРЕВЕДЕНО)" : "");
    }
    printf("\nне переведено: %d из %d\n", untranslated, N);
    return untranslated == 0 ? 0 : 1;
}
