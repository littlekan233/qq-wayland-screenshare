#define _GNU_SOURCE
#include "runtime.h"
#include "wlsanitizer_js.h"

#include <dlfcn.h>
#include <errno.h>
#include <fcntl.h>
#include <glib.h>
#include <limits.h>
#include <pthread.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

static int (*next_openat)(int, const char *, int, ...);
static pthread_once_t io_once = PTHREAD_ONCE_INIT;
static char *app_directory, *original_package, *private_package, *private_directory;
static int redirect_ready;

static void resolve_io(void)
{
    next_openat = dlsym(RTLD_NEXT, "openat");
}

/* No writes to installed resources. Only the main QQ process sees the temporary
 * package; children and unrelated applications retain the original filesystem. */
static const char *redirect_package(int dirfd, const char *name, int flags)
{
    if (!redirect_ready || !qwlss_is_main_process() || !name ||
        (flags & O_ACCMODE) != O_RDONLY || (flags & (O_CREAT | O_TRUNC)))
        return name;
    if (name[0] == '/') return strcmp(name, original_package) ? name : private_package;
    if (strcmp(name, "package.json") && strcmp(name, "./package.json")) return name;
    char directory[PATH_MAX];
    if (dirfd == AT_FDCWD) {
        if (!getcwd(directory, sizeof(directory))) return name;
    } else {
        char fdpath[64];
        snprintf(fdpath, sizeof(fdpath), "/proc/self/fd/%d", dirfd);
        ssize_t n = readlink(fdpath, directory, sizeof(directory) - 1);
        if (n < 0 || (size_t)n >= sizeof(directory) - 1) return name;
        directory[n] = 0;
    }
    return strcmp(directory, app_directory) ? name : private_package;
}

static int open_file(int dirfd, const char *name, int flags, mode_t mode)
{
    pthread_once(&io_once, resolve_io);
    if (!next_openat) { errno = ENOSYS; return -1; }
    return next_openat(dirfd, redirect_package(dirfd, name, flags), flags, mode);
}

static int needs_mode(int flags)
{
    return (flags & O_CREAT) || ((flags & O_TMPFILE) == O_TMPFILE);
}

#define DEFINE_OPEN(name) \
int name(const char *path, int flags, ...) \
{ \
    mode_t mode = 0; \
    if (needs_mode(flags)) { \
        va_list ap; va_start(ap, flags); mode = va_arg(ap, mode_t); va_end(ap); \
    } \
    return open_file(AT_FDCWD, path, flags, mode); \
}
#define DEFINE_OPENAT(name) \
int name(int dirfd, const char *path, int flags, ...) \
{ \
    mode_t mode = 0; \
    if (needs_mode(flags)) { \
        va_list ap; va_start(ap, flags); mode = va_arg(ap, mode_t); va_end(ap); \
    } \
    return open_file(dirfd, path, flags, mode); \
}
DEFINE_OPEN(open)
DEFINE_OPEN(open64)
DEFINE_OPENAT(openat)
DEFINE_OPENAT(openat64)

static char *json_string(const char *value)
{
    GString *s = g_string_new("\"");
    for (const unsigned char *p = (const unsigned char *)value; *p; ++p) {
        if (*p == '\\' || *p == '"') { g_string_append_c(s, '\\'); g_string_append_c(s, *p); }
        else if (*p < 32) g_string_append_printf(s, "\\u%04x", *p);
        else g_string_append_c(s, *p);
    }
    g_string_append_c(s, '"');
    return g_string_free(s, FALSE);
}

static void remove_private_files(void)
{
    if (!private_directory) return;
    const char *files[] = {"package.json", "entry.cjs", "config.json", "capture-ready"};
    for (size_t i = 0; i < G_N_ELEMENTS(files); ++i) {
        char *path = g_build_filename(private_directory, files[i], NULL);
        unlink(path);
        g_free(path);
    }
    rmdir(private_directory);
}

__attribute__((constructor(103)))
static void install_sanitizer(void)
{
    if (!qwlss_is_main_process() || !qwlss_uses_wayland()) return;
    char *exe_directory = g_path_get_dirname(qwlss_executable());
    app_directory = g_build_filename(exe_directory, "resources", "app", NULL);
    original_package = g_build_filename(app_directory, "package.json", NULL);
    g_free(exe_directory);

    char *package = NULL;
    GError *error = NULL;
    if (!g_file_get_contents(original_package, &package, NULL, &error)) goto failed;
    /* Supported QQ layout. Reject unfamiliar launchers instead of guessing. */
    const char *main_path = "application.asar/app_launcher/index.js";
    GRegex *pattern = g_regex_new("\"main\"[[:space:]]*:[[:space:]]*\"(\\./)?application\\.asar/app_launcher/index\\.js\"", 0, 0, &error);
    if (!pattern) goto failed;
    GMatchInfo *match = NULL;
    if (!g_regex_match(pattern, package, 0, &match)) {
        g_match_info_free(match); g_regex_unref(pattern);
        fprintf(stderr, "[qwlss-sanitizer] unsupported QQ package entry; capture remains available\n");
        g_free(package);
        return;
    }
    int first, last;
    g_match_info_fetch_pos(match, 0, &first, &last);
    int duplicate = g_match_info_next(match, NULL);
    g_match_info_free(match); g_regex_unref(pattern);
    if (duplicate) { g_free(package); return; }

    private_directory = g_dir_make_tmp("qwlss-wlsanitizer-XXXXXX", &error);
    if (!private_directory) goto failed;
    private_package = g_build_filename(private_directory, "package.json", NULL);
    char *entry = g_build_filename(private_directory, "entry.cjs", NULL);
    char *flag = g_build_filename(private_directory, "capture-ready", NULL);
    char *config_path = g_build_filename(private_directory, "config.json", NULL);
    char *original_main = g_build_filename(app_directory, main_path, NULL);
    char *quoted_main = json_string(original_main), *quoted_flag = json_string(flag);
    char *config = g_strdup_printf("{\"main\":%s,\"flag\":%s}\n", quoted_main, quoted_flag);

    GString *relative = g_string_new("");
    for (const char *p = app_directory; *p; ++p)
        if (*p == '/') g_string_append(relative, "../");
    g_string_append(relative, entry + 1);
    char *quoted_entry = json_string(relative->str);
    GString *patched = g_string_new_len(package, first);
    g_string_append_printf(patched, "\"main\":%s", quoted_entry);
    g_string_append(patched, package + last);

    int ok = g_file_set_contents(entry, (const char *)wlsanitizer_js, sizeof(wlsanitizer_js) - 1, &error) &&
             g_file_set_contents(config_path, config, -1, &error) &&
             g_file_set_contents(private_package, patched->str, patched->len, &error);
    if (ok) {
        qwlss_set_capture_flag(flag);
        redirect_ready = 1;
        fprintf(stderr, "[qwlss-sanitizer] QQ entry prepared (pid=%ld)\n", (long)getpid());
    }
    g_free(entry); g_free(flag); g_free(config_path); g_free(original_main);
    g_free(quoted_main); g_free(quoted_flag); g_free(config); g_free(quoted_entry);
    g_string_free(relative, TRUE); g_string_free(patched, TRUE);
    if (!ok) goto failed;
    g_free(package);
    return;
failed:
    fprintf(stderr, "[qwlss-sanitizer] entry setup failed: %s; capture remains available\n", error ? error->message : "unknown error");
    g_clear_error(&error);
    g_free(package);
    remove_private_files();
}

__attribute__((destructor))
static void cleanup_sanitizer(void)
{
    if (qwlss_is_main_process()) remove_private_files();
}
