/* _GNU_SOURCE ДОЛЖЕН идти первым: ucontext_t в gregs и REG_RIP видны
 * только с ним. common.h его уже втягивает, поэтому объявляем заранее. */
#ifndef _GNU_SOURCE
#define _GNU_SOURCE
#endif

#include "common.h"
#include "tray.h"

#include <glib-unix.h>
#include <gtk/gtk.h>
#include <glib.h>
#include <gmodule.h>
#ifdef XS_MEM_DEBUG
#include <malloc.h>
#endif
#include <stdlib.h>
#include <string.h>
#include <signal.h>
#include <unistd.h>
#include <fcntl.h>
#include <execinfo.h>
#include <ucontext.h>
#include <stdarg.h>

/* Путь crash-лога из --crash-log. Объявлен ВНЕ #ifdef: сборка без
 * USE=debug тоже принимает и утирает эту опцию, чтобы один и тот же
 * unit-файл не ломался о неизвестный ключ. */
static char *g_crash_log = NULL;

#ifdef XS_ENABLE_CRASH_LATCHER
/* --- Ловец падений -------------------------------------------------
 * Компилируется только с -DXS_ENABLE_CRASH_LATCHER (в ebuild это будет
 * soft-debug USE-флаг). Без него ловец не существует вовсе: обработчика
 * сигналов нет, поведение демона полностью штатное.
 * Ядро собрано без CONFIG_ELF_CORE, coredump невозможен, а gdb на
 * живом демоне не ловит: под gdb гонка не проявляется. Поэтому ловим
 * SIGSEGV сами и печатаем backtrace в отдельный файл.
 *
 * Обработчик НИЧЕГО не чинит и не продолжает: после segfault состояние
 * процесса не определено. Только диагностика, затем честный выход.
 * Всё через write(2) — async-signal-safe, без malloc/printf.
 */
static int g_crash_fd = -1;

static void crash_write(const char *s)
{
    if (g_crash_fd < 0)
        return;
    ssize_t n = write(g_crash_fd, s, strlen(s));
    (void)n;
}

static void crash_handler(int sig, siginfo_t *si, void *uc)
{
    char buf[256];
    void *frames[64];
    int n;
    int len;

    /* Только write() и обратно: ничего, что может залочиться. */
    crash_write("\n==== XSCREENLETSD CRASH ====\n");
    len = snprintf(buf, sizeof(buf), "signal %d (%s) at addr %p\n",
                   sig, strsignal(sig), si ? si->si_addr : NULL);
    if (len > 0) {
        ssize_t n = write(g_crash_fd, buf, (size_t)len);
        (void)n;
    }
    /* Регистры контекста, если платформа их даёт */
    if (uc) {
        ucontext_t *c = (ucontext_t *)uc;
        len = snprintf(buf, sizeof(buf),
                       "pc=%llx sp=%llx rax=%llx rbx=%llx rcx=%llx "
                       "rdx=%llx rsi=%llx rdi=%llx rbp=%llx\n",
                       (unsigned long long)c->uc_mcontext.gregs[REG_RIP],
                       (unsigned long long)c->uc_mcontext.gregs[REG_RSP],
                       (unsigned long long)c->uc_mcontext.gregs[REG_RAX],
                       (unsigned long long)c->uc_mcontext.gregs[REG_RBX],
                       (unsigned long long)c->uc_mcontext.gregs[REG_RCX],
                       (unsigned long long)c->uc_mcontext.gregs[REG_RDX],
                       (unsigned long long)c->uc_mcontext.gregs[REG_RSI],
                       (unsigned long long)c->uc_mcontext.gregs[REG_RDI],
                       (unsigned long long)c->uc_mcontext.gregs[REG_RBP]);
        if (len > 0) {
            ssize_t n = write(g_crash_fd, buf, (size_t)len);
            (void)n;
        }
    }
    /* Стек вызовов. backtrace() может аллоцировать при первом вызове,
     * поэтому дополнительно прогреваем буфер заранее в install. */
    n = backtrace(frames, 64);
    /* backtrace_symbols_fd пишет через fd, без malloc — безопасно. */
    backtrace_symbols_fd(frames, n, g_crash_fd);
    crash_write("==== END CRASH ====\n");

    /* Восстанавливаем поведение по умолчанию и умираем честно,
     * чтобы dmesg увидел настоящий segfault. */
    signal(sig, SIG_DFL);
    raise(sig);
    _exit(139);
}

static void install_crash_handler(void)
{
    /* Прогрев: первый backtrace() аллоцирует, в обработчике это опасно. */
    void *warm[8];
    (void)backtrace(warm, 8);

    /* Приоритет: --crash-log, потом XS_CRASH_LOG, потом дефолт. */
    const char *path = g_crash_log;
    if (!path || !path[0])
        path = g_getenv("XS_CRASH_LOG");
    if (!path || !path[0])
        path = "/tmp/xscreenletsd-crash.log";
    /* O_APPEND: каждый процесс дописывает, не затирая чужое. */
    g_crash_fd = open(path, O_WRONLY | O_CREAT | O_APPEND | O_CLOEXEC, 0644);
    if (g_crash_fd < 0)
        g_crash_fd = STDERR_FILENO;

    struct sigaction sa;
    memset(&sa, 0, sizeof(sa));
    sa.sa_sigaction = crash_handler;
    sa.sa_flags = SA_SIGINFO | SA_NODEFER;
    sigemptyset(&sa.sa_mask);
    sigaction(SIGSEGV, &sa, NULL);
    sigaction(SIGBUS, &sa, NULL);
    sigaction(SIGILL, &sa, NULL);
    sigaction(SIGFPE, &sa, NULL);
    sigaction(SIGABRT, &sa, NULL);
}

#endif /* XS_ENABLE_CRASH_LATCHER */

static char *g_conf_path = NULL;
static char *g_plugdir = NULL;

const char *xs_core_plugdir(void)
{
    return g_plugdir;
}
static GPtrArray *g_loaded_modules = NULL;
static guint g_sighup_source_id = 0;

/* Описание загруженного типа плагина: модуль + дескриптор.
 * Инстансов может быть много на один тип. (XsLoadedPlugin в common.h) */
static gboolean on_sighup(gpointer data)
{
    (void)data;
    xs_log_impl("SIGHUP received, reloading plugins");
    xs_core_reload();
    return G_SOURCE_CONTINUE;
}

#ifdef XS_MEM_DEBUG
/* Отладочный крючок для диагностики памяти. Собирается ТОЛЬКО с
 * -DXS_MEM_DEBUG и в обычную сборку не попадает: обработчик не
 * вешается и malloc_trim никто не зовёт.
 *
 * Зачем: malloc_trim(0) отдаёт ОС свободные страницы кучи, которые
 * glibc иначе держит. Если после него RSS падает почти к стартовому —
 * память не утекает, а лежит во внутренних кэшах и фрагментации. Если
 * остаётся высоким — это настоящая утечка, объекты живые, и дальше
 * нужна атрибуция по апплетам. Один сигнал отвечает на вопрос, на
 * который иначе нужен суточный замер. */
static guint g_sigusr1_source_id = 0;

static gsize xs_mem_rss_bytes(void)
{
	char buf[256];
	gsize total = 0, resident = 0;
	int fd;
	ssize_t n;

	fd = open("/proc/self/statm", O_RDONLY);
	if (fd < 0)
		return 0;
	n = read(fd, buf, sizeof(buf) - 1);
	close(fd);
	if (n <= 0)
		return 0;
	buf[n] = '\0';
	if (sscanf(buf, "%" G_GSIZE_FORMAT " %" G_GSIZE_FORMAT,
	           &total, &resident) != 2)
		return 0;
	return resident * (gsize)sysconf(_SC_PAGESIZE);
}

static gboolean on_sigusr1(gpointer data)
{
	gsize before, after;

	(void)data;
	before = xs_mem_rss_bytes();
	malloc_trim(0);
	after = xs_mem_rss_bytes();
	xs_log_impl("SIGUSR1: malloc_trim(0) — RSS %.1f -> %.1f MB, ОС вернулось %.1f MB",
	            before / 1048576.0, after / 1048576.0,
	            (before > after ? before - after : 0) / 1048576.0);
	return G_SOURCE_CONTINUE;
}

static guint g_mem_poll_source_id = 0;

/* Периодический замер RSS.
 *
 * Зачем он нужен рядом с SIGUSR1: сигнал снимает память по команде, а за
 * ночь никто не пошлёт SIGUSR1 - демон работает без присмотра часами, и
 * кривая просто не появится. Таймер же сам пишет замеры в лог, и утром
 * получается готовый график роста без чьего-либо участия.
 *
 * Сравниваются два числа: RSS "как есть" и RSS после malloc_trim(0).
 * Разница между ними и есть мусор в куче, который glib держит у себя.
 * Если trim стабильно возвращает десятки мегабайт - память не течёт, она
 * лежит во внутренних кэшах. Если после trim RSS продолжает расти -
 * это настоящая утечка, объекты живые.
 */
/* Интервал замера памяти, секунды. XSCREENLETS_MEM_POLL задаёт его извне. */
static guint g_mem_poll_interval(void)
{
	const char *e = g_getenv("XSCREENLETS_MEM_POLL");
	gint64 v = e ? g_ascii_strtoll(e, NULL, 10) : 300;

	if (v < 5 || v > 86400)
		return 300;
	return (guint)v;
}

static gboolean on_mem_poll(gpointer data)
{
	gsize before, after;
	struct mallinfo2 mi;

	(void)data;
	before = xs_mem_rss_bytes();
	/* Снимок ДО trim: что реально занято (uordblks) против того, что
	 * malloc считает свободным внутри арен (fordblks). */
	mi = mallinfo2();
	malloc_trim(0);
	after = xs_mem_rss_bytes();
	/* uordblks - байты, ВЫДЕЛЕННЫЕ живьём и не освобождённые. Если он
	 * растёт так же, как RSS, то растущая память удерживается объектами.
	 * Если он стоит, а растёт только RSS - это арены и фрагментация, то
	 * есть ничего не удерживает, память просто не возвращается ОС.
	 * Разница malloc_trim (before - after) этого не различает - она
	 * показывает только свободный хвост кучи. */
	xs_log_impl("MEM: RSS %.1f MB, после trim %.1f MB, в куче %.1f MB | "
	            "uordblks %.1f MB (занято), fordblks %.1f MB (свободно), "
	            "arena %.1f MB, mmap %.1f MB",
	            before / 1048576.0, after / 1048576.0,
	            (before > after ? before - after : 0) / 1048576.0,
	            (double)mi.uordblks / 1048576.0,
	            (double)mi.fordblks / 1048576.0,
	            (double)mi.arena / 1048576.0,
	            (double)mi.hblkhd / 1048576.0);
	return G_SOURCE_CONTINUE;
}

static void setup_sigusr1_handler(void)
{
	if (g_sigusr1_source_id) {
		g_source_remove(g_sigusr1_source_id);
		g_sigusr1_source_id = 0;
	}
	g_sigusr1_source_id = g_unix_signal_add(SIGUSR1, on_sigusr1, NULL);
	if (!g_sigusr1_source_id)
		xs_log_impl("cannot install SIGUSR1 handler");

	/* Замер каждые 5 минут. Чаще - лог распухнет и за ночь съест диск;
	 * реже - по кривой нельзя отличить утечку от разового рабочего набора.
	 * malloc_trim здесь дорогой (обходит кучу), поэтому интервал
	 * сознательно не меньше нескольких минут. */
	if (g_mem_poll_source_id) {
		g_source_remove(g_mem_poll_source_id);
		g_mem_poll_source_id = 0;
	}
	/* Интервал задаётся переменной: для короткого A/B-замера в 1 час
	 * нужно разрезать замеры (5 минут дали бы всего 12 точек - регрессия
	 * по ним ненадёжна), а для многочасового прогона 300 с как раз
	 * правильный порядок. */
	g_mem_poll_source_id = g_timeout_add_seconds(
	    g_mem_poll_interval(), on_mem_poll, NULL);
	if (!g_mem_poll_source_id)
		xs_log_impl("cannot start memory poll timer");
	else
		xs_log_impl("memory poll: every %us", g_mem_poll_interval());
}

static void uninstall_sigusr1_handler(void)
{
	if (g_sigusr1_source_id) {
		g_source_remove(g_sigusr1_source_id);
		g_sigusr1_source_id = 0;
	}
	if (g_mem_poll_source_id) {
		g_source_remove(g_mem_poll_source_id);
		g_mem_poll_source_id = 0;
	}
}
#endif /* XS_MEM_DEBUG */

static void setup_sighup_handler(void)
{
    if (g_sighup_source_id) {
        g_source_remove(g_sighup_source_id);
        g_sighup_source_id = 0;
    }
    g_sighup_source_id = g_unix_signal_add(SIGHUP, on_sighup, NULL);
    if (!g_sighup_source_id)
        xs_log_impl("cannot install SIGHUP handler");
}

static void uninstall_sighup_handler(void)
{
    if (g_sighup_source_id) {
        g_source_remove(g_sighup_source_id);
        g_sighup_source_id = 0;
    }
}

static void free_loaded_modules(void)
{
    if (!g_loaded_modules)
        return;
    for (gsize i = 0; i < g_loaded_modules->len; i++) {
        XsLoadedPlugin *lp = g_ptr_array_index(g_loaded_modules, i);

        g_module_close(lp->mod);
        g_free(lp);
    }
    g_ptr_array_free(g_loaded_modules, TRUE);
    g_loaded_modules = NULL;
}

/* Загрузить все .so из plugdir (только модули+дескрипторы, без инстансов). */
static void load_plugin_modules(void)
{
    GDir *d = g_dir_open(g_plugdir, 0, NULL);

    if (!d) {
        xs_log_impl("plugin dir %s: cannot open", g_plugdir);
        return;
    }
    const char *fn;
    while ((fn = g_dir_read_name(d))) {
        char *path;
        GModule *mod;
        XsPluginDescFn fn_desc;
        XsLoadedPlugin *lp;

        if (!g_str_has_suffix(fn, ".so"))
            continue;
        path = g_build_filename(g_plugdir, fn, NULL);
        mod = g_module_open(path, G_MODULE_BIND_LAZY | G_MODULE_BIND_LOCAL);
        if (!mod) {
            xs_log_impl("load %s: %s", path, g_module_error());
            g_free(path);
            continue;
        }
        fn_desc = NULL;
        if (!g_module_symbol(mod, "xs_plugin_desc", (gpointer *)&fn_desc) ||
            !fn_desc) {
            xs_log_impl("load %s: no xs_plugin_desc", path);
            g_module_close(mod);
            g_free(path);
            continue;
        }
        lp = g_new0(XsLoadedPlugin, 1);
        lp->desc = fn_desc();
        if (!lp->desc || lp->desc->api_version != XS_API_VERSION) {
            xs_log_impl("load %s: api_version mismatch (%u != %u)",
                        path, lp->desc ? lp->desc->api_version : 0,
                        XS_API_VERSION);
            g_free(lp);
            g_module_close(mod);
            g_free(path);
            continue;
        }
        if (!g_loaded_modules)
            g_loaded_modules = g_ptr_array_new();
        lp->mod = mod; /* нужен для free_loaded_modules() */
        g_ptr_array_add(g_loaded_modules, lp);
        xs_log_impl("module %s: type '%s' ready", path, lp->desc->name);
        g_free(path);
    }
    g_dir_close(d);
}

static XsLoadedPlugin *find_loaded(const char *type)
{
    if (!g_loaded_modules || !type)
        return NULL;
    for (gsize i = 0; i < g_loaded_modules->len; i++) {
        XsLoadedPlugin *lp = g_ptr_array_index(g_loaded_modules, i);

        if (lp->desc && strcmp(lp->desc->name, type) == 0)
            return lp;
    }
    return NULL;
}

/* Создать инстанс типа type с именем iname. */
static XsPlugin *create_instance(const char *type, const char *iname)
{
    XsLoadedPlugin *lp = find_loaded(type);
    XsPlugin *p;

    if (!lp) {
        xs_log_impl("instance '%s': plugin type '%s' not loaded",
                    iname, type);
        return NULL;
    }
    p = g_new0(XsPlugin, 1);
    p->name = g_strdup(iname);
    p->type = g_strdup(type);
    p->host = xs_host_api();
    p->ops = lp->desc->ops;
    p->desc = lp->desc->desc;
    p->author = lp->desc->author;
    p->version = lp->desc->version;
    p->priv = NULL;
    if (p->ops && p->ops->init) {
        GKeyFile *kf = xs_core_plugin_conf(p->name);

        /* xs_type: тип в конфиге (нужен для запуска гостей). */
        g_key_file_set_string(kf, p->name, "xs_type", type);
        if (p->ops->init(p, kf) != 0 || !p->win) {
            xs_log_impl("instance %s: init failed", p->name);
            xs_core_free_plugin(p);
            return NULL;
        }
    }
    xs_core_register_plugin(p);
    /* гость рамки не показывается в трее (started_by=plugin):
     * флаг ставится позже (при хостинге), поэтому проверяем конфиг */
    {
        GKeyFile *kf0 = xs_core_plugin_conf(p->name);
        char *sb0 = g_key_file_get_string(kf0, p->name, "started_by",
                                          NULL);

        if (!sb0 || strcmp(sb0, "main_daemon") == 0)
            xs_tray_add_plugin(p);
        else
            xs_log_impl("tray: %s скрыт (started_by=%s)", p->name, sb0);
        g_free(sb0);
    }
    xs_log_impl("loaded %s (api %u)", p->name, lp->desc->api_version);
    return p;
}

/* Запустить инстанс из имени конфига (после выбора в диалоге мёртвого
 * symlink'а): тип определяем по загруженным модулям. */
static void create_instance_from_conf(const char *iname)
{
    if (!g_loaded_modules || !iname)
        return;
    for (gsize i = 0; i < g_loaded_modules->len; i++) {
        XsLoadedPlugin *lp = g_ptr_array_index(g_loaded_modules, i);
        gsize tl;

        if (!lp || !lp->desc || !lp->desc->name)
            continue;
        tl = strlen(lp->desc->name);
        if (strncmp(iname, lp->desc->name, tl) == 0 && iname[tl] == '-') {
            create_instance(lp->desc->name, iname);
            xs_tray_rebuild();
            return;
        }
    }
    xs_log_impl("instance '%s': no plugin type matches", iname);
}

/* Список инстансов: каталог plugins_on (symlink'и на конфиги в .plugins).
 * Имя инстанса = имя symlink'а без .conf, тип = часть до первого "-UUID".
 * Мёртвые symlink'и: диалог (удалить / указать другой конфиг). */
static void create_instances(void)
{
    const char *onoff = xs_core_onoff_dir();
    GDir *d;
    const char *fn;

    if (!onoff) {
        xs_tray_rebuild();
        return;
    }
    d = g_dir_open(onoff, 0, NULL);
    if (!d) {
        xs_tray_rebuild();
        return;
    }
    while ((fn = g_dir_read_name(d)) != NULL) {
        char *linkpath, *target;
        char *type, *iname;
        GKeyFile *kf;
        char *lab;

        if (!g_str_has_suffix(fn, ".conf"))
            continue;
        linkpath = g_build_filename(onoff, fn, NULL);
        target = g_file_read_link(linkpath, NULL);
        if (target) {
            /* Относительный symlink резолвим относительно каталога
             * самого symlink'а (не CWD демона!). */
            if (!g_path_is_absolute(target)) {
                char *ldir = g_path_get_dirname(linkpath);
                char *abs = g_build_filename(ldir, target, NULL);

                g_free(target);
                target = abs;
                g_free(ldir);
            }
        }
        if (!target || !g_file_test(target, G_FILE_TEST_EXISTS)) {
            char *newname = NULL;
            int res = xs_dead_link_dialog(NULL, fn, &newname);

            if (res == 1) {
                unlink(linkpath);
                xs_log_impl("dead symlink removed: %s", fn);
            } else if (res == 0 && newname && newname[0]) {
                /* пересоздать symlink на выбранный конфиг и запустить */
                char *target2 = g_build_filename(
                    g_path_get_dirname(onoff), ".plugins", newname, NULL);

                unlink(linkpath);
                if (symlink(target2, linkpath) == 0)
                    create_instance_from_conf(newname);
                else
                    xs_log_impl("relink %s failed", linkpath);
                g_free(target2);
            }
            g_free(newname);
            g_free(target);
            g_free(linkpath);
            continue;
        }
        g_free(target);
        iname = g_strndup(fn, strlen(fn) - 5); /* без .conf */
        /* тип = имя до "-UUID8-label": имя плагина <тип>-... —
         * определяем по загруженным модулям. */
        type = NULL;
        if (g_loaded_modules) {
            for (gsize i = 0; i < g_loaded_modules->len; i++) {
                XsLoadedPlugin *lp = g_ptr_array_index(g_loaded_modules, i);
                gsize tl;

                if (!lp || !lp->desc || !lp->desc->name)
                    continue;
                tl = strlen(lp->desc->name);
                if (strncmp(iname, lp->desc->name, tl) == 0 &&
                    iname[tl] == '-') {
                    type = g_strdup(lp->desc->name);
                    break;
                }
            }
        }
        if (!type) {
            xs_log_impl("instance '%s': no plugin type matches", iname);
            g_free(iname);
            g_free(linkpath);
            continue;
        }
        /* user_label из конфига (не обязателен для запуска) */
        kf = xs_core_plugin_conf(iname);
        lab = kf ? g_key_file_get_string(kf, iname, "user_label", NULL)
                 : NULL;
        g_free(lab);
        /* started_by: main_daemon | plugin. 'plugin' демон не запускает
         * — это гость какого-то frame_launcher'а. */
        {
            char *sb = kf ? g_key_file_get_string(kf, iname,
                                                  "started_by", NULL)
                          : NULL;

            if (sb && sb[0] && strcmp(sb, "main_daemon") != 0) {
                xs_log_impl("skip %s (started_by=%s)", iname, sb);
                g_free(sb);
                g_free(type);
                g_free(iname);
                g_free(linkpath);
                continue;
            }
            g_free(sb);
        }
        create_instance(type, iname);
        g_free(type);
        g_free(iname);
        g_free(linkpath);
    }
    g_dir_close(d);
    /* Собрать меню трея целиком (Launch Applet, Running Instances,
     * Restart, Quit...). */
    xs_tray_rebuild();
}

/* Записать текущий набор инстансов обратно в [instances] (после
 * add/delete, чтобы список переживал перезапуск). */
void xs_core_save_instances(void)
{
    GKeyFile *cf = xs_core_conf();
    gsize n = xs_core_plugin_count();

    g_key_file_remove_group(cf, "instances", NULL);
    for (gsize i = 0; i < n; i++) {
        XsPlugin *p = xs_core_plugin_at(i);

        if (p && p->name && xs_core_plugin_type(p))
            g_key_file_set_string(cf, "instances", p->name,
                                  xs_core_plugin_type(p));
    }
    xs_core_conf_flush();
}

int main(int argc, char **argv)
{
    g_free(g_conf_path);
    g_conf_path = g_build_filename(g_get_user_config_dir(),
                                   "xscreenlets", "xscreenletsd.conf", NULL);
    /* Плагины по умолчанию — наш install-каталог (/usr/lib64/... доступен
     * через --plugdir при системной установке). */
    g_plugdir = g_build_filename(g_get_home_dir(), "lib", "xscreenlets",
                                 "plugins", NULL);

    int want_debug = 0;

    for (int i = 1; i < argc; i++) {
        if (g_strcmp0(argv[i], "--conf") == 0 && i + 1 < argc) {
            g_free(g_conf_path);
            g_conf_path = g_strdup(argv[++i]);
        } else if (g_strcmp0(argv[i], "--plugdir") == 0 && i + 1 < argc) {
            g_free(g_plugdir);
            g_plugdir = g_strdup(argv[++i]);
        } else if (g_strcmp0(argv[i], "--debug") == 0) {
            /* Под USE=debug сюда же попадает установка ловца падений:
             * одна опция — «диагностика», без отдельного --soft-debug. */
            want_debug = 1;
        } else if (g_strcmp0(argv[i], "--crash-log") == 0 && i + 1 < argc) {
            g_free(g_crash_log);
            g_crash_log = g_strdup(argv[++i]);
        }
    }

#ifdef XS_ENABLE_CRASH_LATCHER
    /* Ловец падений ставится до gtk_init: он может упасть сам, и тогда
     * без раннего перехвата мы потеряем стек. Включается --debug
     * (USE=debug) либо непустым XS_CRASH_LOG — так переборы могут
     * запускать демон с ловцом, не добавляя флагов. */
    {
        const char *env_log = g_getenv("XS_CRASH_LOG");
        int want_latch = want_debug || (env_log && env_log[0]);

        if (want_latch) {
            install_crash_handler();
            xs_log_impl("debug: crash latch enabled");
        }
    }
#else
    (void)want_debug;
    /* Сборка без USE=debug: --crash-log принимается и игнорируется,
     * чтобы один и тот же unit-файл работал с обоими билдами. */
    g_clear_pointer(&g_crash_log, g_free);
#endif
    xs_core_set_debug(want_debug);

    /* SIGHUP reload setup */
    setup_sighup_handler();
#ifdef XS_MEM_DEBUG
    setup_sigusr1_handler();
#endif

    gtk_init(&argc, &argv);
    xs_core_init(g_conf_path);
    xs_tray_init();
    load_plugin_modules();
    xs_core_set_loaded_modules_ref(&g_loaded_modules);
    create_instances();

    gtk_main();

    xs_core_shutdown_all();
    xs_tray_shutdown();
    uninstall_sighup_handler();
#ifdef XS_MEM_DEBUG
    uninstall_sigusr1_handler();
#endif
    free_loaded_modules();
    g_free(g_conf_path);
    g_free(g_plugdir);
    return 0;
}
