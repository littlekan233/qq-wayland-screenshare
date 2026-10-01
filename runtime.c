#define _GNU_SOURCE
#include "runtime.h"

#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#ifndef QWLSS_EXE_PATH
#define QWLSS_EXE_PATH "/proc/self/exe"
#endif
#ifndef QWLSS_CMDLINE_PATH
#define QWLSS_CMDLINE_PATH "/proc/self/cmdline"
#endif

static pid_t owner;
static int wayland;
static char executable[PATH_MAX];
static char capture_flag[PATH_MAX];

__attribute__((constructor(102)))
static void initialize_runtime(void)
{
    ssize_t n = readlink(QWLSS_EXE_PATH, executable, sizeof(executable) - 1);
    if (n <= 0 || n == sizeof(executable) - 1) return;
    executable[n] = 0;
    const char *base = strrchr(executable, '/');
    if (!base || strcmp(base + 1, "qq")) return;

    char args[65536];
    int fd = open(QWLSS_CMDLINE_PATH, O_RDONLY | O_CLOEXEC);
    if (fd < 0) return;
    n = read(fd, args, sizeof(args));
    close(fd);
    if (n <= 0 || n == sizeof(args)) return;

    const char *display = getenv("WAYLAND_DISPLAY");
    wayland = display && *display;
    for (size_t pos = 0; pos < (size_t)n;) {
        size_t len = strnlen(args + pos, (size_t)n - pos);
        if (len == (size_t)n - pos) return;
        if (!strncmp(args + pos, "--type=", 7)) return;
        if (!strcmp(args + pos, "--ozone-platform=x11")) wayland = 0;
        if (!strcmp(args + pos, "--ozone-platform=wayland")) wayland = 1;
        pos += len + 1;
    }
    owner = getpid();
    /* QQ's sharing UI tests this variable independently of its Ozone backend. */
    if (setenv("XDG_SESSION_TYPE", "x11", 1) < 0)
        fprintf(stderr, "[qwlss] cannot set session compatibility: %s\n", strerror(errno));
    else
        fprintf(stderr, "[qwlss] session compatibility set (ozone=%s)\n",
                wayland ? "wayland" : "x11");
}

int qwlss_is_main_process(void) { return owner && owner == getpid(); }
int qwlss_uses_wayland(void) { return wayland; }
const char *qwlss_executable(void) { return executable; }

void qwlss_set_capture_flag(const char *path)
{
    if (qwlss_is_main_process() && strlen(path) < sizeof(capture_flag))
        strcpy(capture_flag, path);
}

/* Called by the capture controller under its lock, in the owning main process.
 * This reports receiver initialization, not confirmation of a user share. */
void qwlss_capture_ready(int ready)
{
    if (!qwlss_is_main_process() || !capture_flag[0]) return;
    if (!ready) {
        if (unlink(capture_flag) < 0 && errno != ENOENT)
            fprintf(stderr, "[qwlss] cannot clear capture status: %s\n", strerror(errno));
        return;
    }
    int fd = open(capture_flag, O_WRONLY | O_CREAT | O_TRUNC | O_NOFOLLOW | O_CLOEXEC, 0600);
    if (fd < 0) {
        fprintf(stderr, "[qwlss] cannot publish capture status: %s\n", strerror(errno));
        return;
    }
    char text[32];
    int size = snprintf(text, sizeof(text), "%ld\n", (long)owner);
    ssize_t written;
    do { written = write(fd, text, (size_t)size); } while (written < 0 && errno == EINTR);
    if (written != size) {
        fprintf(stderr, "[qwlss] incomplete capture status write\n");
        unlink(capture_flag);
    }
    close(fd);
}
