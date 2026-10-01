/* i18n.c - инициализация переводов интерфейса.
 *
 * Вызывается из main() демона ДО создания плагинов: каждый плагин
 * загружается через dlopen() отдельным .so, но gettext использует
 * процесс-глобальное состояние (LC_MESSAGES, textdomain), поэтому
 * одного вызова в демоне достаточно для всех.
 */
#include "i18n.h"

#include <glib.h>
#include <gtk/gtk.h>
#include <locale.h>
#include <stdlib.h>
#include <string.h>

/* Пустое значение (LOCALEDIR="" при сборке из исходников) означает
 * "искать в $HOME", см. xs_i18n_localedir(). */
#ifndef XS_LOCALEDIR
#define XS_LOCALEDIR ""
#endif

#ifndef XS_TEXTDOMAIN
#define XS_TEXTDOMAIN "xscreenlets"
#endif

const char *xs_i18n_localedir(void)
{
    const char *env = g_getenv("XSCREENLETS_LOCALEDIR");

    /* Переопределение нужно для запуска из build/ без установки:
     * переводы лежат в исходнике, а не в /usr/share/locale. */
    if (env && *env)
        return env;
    if (XS_LOCALEDIR && *XS_LOCALEDIR)
        return XS_LOCALEDIR;
    /* Каталог не задан: возвращаем NULL, и вызывающая сторона подставит
     * /usr/share/locale. Полагаться на путь, скомпилированный в libc,
     * нельзя - на нестандартной сборке он указывает не туда. */
    return NULL;
}

void xs_i18n_init(void)
{
    const char *lang;
    const char *dir;

    /* setlocale(LC_ALL, "") обязателен: без него gettext() работает, но
     * GTK рисует шрифты с кодировкой C и русский текст превращается в
     * квадраты. Символ "" означает "взять локаль из окружения". */
    if (!setlocale(LC_ALL, ""))
        g_warning("setlocale(LC_ALL, \"\") failed: "
                  "translations may be unavailable");

#ifdef HAVE_GETTEXT
    /* gtk_get_default_language() возвращает PangoLanguage*, а не строку.
     * Именно это значение и нужно: GLib согласует LANG, LC_MESSAGES и
     * G_MESSAGES_DOMAINS и отдаёт уже разобранный язык. getenv("LANG")
     * на Gentoo часто даёт C при русском интерфейсе, потому что
     * LC_MESSAGES выставлен в C. */
    lang = pango_language_to_string(gtk_get_default_language());

    dir = xs_i18n_localedir();
    if (!dir)
        dir = "/usr/share/locale";

    bindtextdomain(XS_TEXTDOMAIN, dir);
    bind_textdomain_codeset(XS_TEXTDOMAIN, "UTF-8");
    textdomain(XS_TEXTDOMAIN);

    if (lang && *lang) {
        char *bare = g_strdup(lang);
        char *dot = strchr(bare, '.');
        char *at;
        char *sep;

        /* "ru-ru" -> "ru", но "sr-Latn" -> "sr-Latn".
         *
         * Pango отдаёт код с территорией через дефис, а каталог перевода
         * называется коротким кодом. Без этого LANGUAGE="ru-ru" не найдёт
         * ничего, и glibc, не дойдя до LC_MESSAGES, вернёт английский.
         *
         * Отличаем территорию от сценария по длине: территория - две
         * буквы (ru-ru, en-gb), сценарий - четыре (sr-Latn, zh-Hant). Для
         * сценария короткий код неверен: sr-Latn означает "латиница", и
         * каталог sr дал бы кириллицу. */
        sep = strchr(bare, '-');
        if (sep && strlen(sep + 1) != 4)
            *sep = '\0';
        if (dot)
            *dot = '\0';
        at = strchr(bare, '@');
        if (at)
            *at = '\0';

        /* LANGUAGE выставляется ТОЛЬКО кодом языка, без списка.
         *
         * Проверено на этой машине: LANGUAGE="ru,xscreenlets" даёт
         * английский интерфейс, хотя каталог ru/ лежит на месте. glibc
         * перебирает список по очереди, не находит каталога для второго
         * элемента и СДАЁТСЯ - к первому не возвращается. Добавление
         * имени textdomain в список (логичное на вид, чтобы был запасной
         * вариант) молча ломает перевод полностью.
         *
         * Поэтому здесь ровно один код. Если перевода для языка нет,
         * gettext корректно отдаст исходную английскую строку. */
        if (*bare && !g_getenv("LANGUAGE"))
            g_setenv("LANGUAGE", bare, FALSE);
        g_free(bare);
    }
#else
    (void)lang;
    (void)dir;
#endif
}
