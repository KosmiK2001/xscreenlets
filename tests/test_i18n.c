/* Проверка переводов: gettext должен вернуть русский текст из build/locale.
 * Компилируется отдельно, не входит в проект:
 *   gcc -o /tmp/tpo tests/test_i18n.c && /tmp/tpo
 */
#include <libintl.h>
#include <locale.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

int main(int argc, char **argv)
{
    const char *dir = argc > 1 ? argv[1] : "build/locale";
    const char *msgs[] = {
        "Applets", "Opacity", "Scale", "About", "Themes", "Window",
        "X-Position", "Y-Position", "User label", "Options",
        "No sensors found", "Network Monitor", "CPU Monitor", "Name",
        "Applet type for the new guest:", "Window title (optional)",
    };
    size_t i;
    int untranslated = 0;

    setlocale(LC_ALL, "");
    bindtextdomain("xscreenlets", dir);
    textdomain("xscreenlets");
    bind_textdomain_codeset("xscreenlets", "UTF-8");

    printf("LANG=%s LC_ALL=%s LANGUAGE=%s\n",
           getenv("LANG") ? getenv("LANG") : "(нет)",
           getenv("LC_ALL") ? getenv("LC_ALL") : "(нет)",
           getenv("LANGUAGE") ? getenv("LANGUAGE") : "(нет)");

    for (i = 0; i < sizeof(msgs) / sizeof(msgs[0]); i++) {
        const char *r = gettext(msgs[i]);
        int same = (strcmp(r, msgs[i]) == 0);
        if (same)
            untranslated++;
        printf("  %-30s -> %s%s\n", msgs[i], r, same ? "   (НЕ ПЕРЕВЕДЕНО)" : "");
    }
    printf("\nне переведено: %d из %d\n", untranslated,
           (int)(sizeof(msgs) / sizeof(msgs[0])));
    return untranslated == 0 ? 0 : 1;
}
