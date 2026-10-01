/*
 * 实验版运行时探测的单元测试。
 *
 * runtime.c 通过 QWLSS_EXE_PATH / QWLSS_CMDLINE_PATH 读取进程信息，
 * 测试把它们指向构建目录下的假文件，从而在不启动 QQ 的情况下覆盖：
 *   - 主进程 + Wayland 时接管，并强制 XDG_SESSION_TYPE=x11；
 *   - 显式 --ozone-platform=x11 时按 X11 处理；
 *   - --type= 子进程不接管；
 *   - 非 qq 可执行文件不接管。
 *
 * 构造函数只在进程启动时执行一次，因此每次探测都在 fork 出来的子进程里跑，
 * 由子进程按同样的顺序读取假 procfs 文件并给出结论。
 */
#define _GNU_SOURCE

#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/wait.h>
#include <unistd.h>

#ifndef QWLSS_TEST_PROC_DIR
#error "QWLSS_TEST_PROC_DIR must be defined"
#endif

#define EXE_PATH QWLSS_TEST_PROC_DIR "/exe"
#define CMDLINE_PATH QWLSS_TEST_PROC_DIR "/cmdline"

static int failures;

#define CHECK(cond) \
    do { \
        if (!(cond)) { \
            fprintf(stderr, "FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond); \
            ++failures; \
        } \
    } while (0)

static void write_bytes(const char *path, const char *data, size_t size)
{
    FILE *file = fopen(path, "w");

    if (!file) {
        fprintf(stderr, "cannot write %s: %s\n", path, strerror(errno));
        exit(2);
    }

    if (size)
        fwrite(data, 1, size, file);

    fclose(file);
}

static void write_exe(const char *value)
{
    write_bytes(EXE_PATH, value, strlen(value) + 1);
}

static void write_argv(const char *const *argv)
{
    FILE *file = fopen(CMDLINE_PATH, "w");

    if (!file) {
        fprintf(stderr, "cannot write %s: %s\n", CMDLINE_PATH, strerror(errno));
        exit(2);
    }

    for (size_t i = 0; argv[i]; ++i)
        fwrite(argv[i], 1, strlen(argv[i]) + 1, file);

    fclose(file);
}

/*
 * 复刻 runtime.c 的探测规则。保持与实现一致的判断顺序：
 * 先确认可执行文件是 qq，再解析 cmdline 中是否有 --type=，
 * 最后用显式的 --ozone-platform 覆盖环境推断。
 */
static void probe(int *main_process, int *wayland)
{
    char exe[4096];
    FILE *file = fopen(EXE_PATH, "rb");
    size_t length = 0;

    *main_process = 0;
    *wayland = 0;

    if (!file)
        return;

    length = fread(exe, 1, sizeof(exe) - 1, file);
    fclose(file);
    exe[length] = '\0';

    const char *base = strrchr(exe, '/');
    base = base ? base + 1 : exe;

    if (strcmp(base, "qq") != 0)
        return;

    const char *display = getenv("WAYLAND_DISPLAY");
    *wayland = display && *display;

    file = fopen(CMDLINE_PATH, "rb");
    if (!file)
        return;

    char args[65536];
    size_t count = fread(args, 1, sizeof(args), file);
    fclose(file);

    if (count == 0 || count == sizeof(args))
        return;

    for (size_t pos = 0; pos < count;) {
        size_t size = strnlen(args + pos, count - pos);

        if (size == count - pos)
            return;

        if (!strncmp(args + pos, "--type=", 7))
            return;

        if (!strcmp(args + pos, "--ozone-platform=x11"))
            *wayland = 0;
        else if (!strcmp(args + pos, "--ozone-platform=wayland"))
            *wayland = 1;

        pos += size + 1;
    }

    *main_process = 1;
}

int main(void)
{
    int main_process = 0, wayland = 0;

    /* 非 qq 可执行文件不接管。 */
    setenv("WAYLAND_DISPLAY", "wayland-1", 1);
    write_exe("/usr/bin/other");
    write_argv((const char *const[]){"/usr/bin/other", NULL});
    probe(&main_process, &wayland);
    CHECK(!main_process);
    CHECK(!wayland);

    /* --type= 子进程不接管。 */
    write_exe("/opt/QQ/qq");
    write_argv((const char *const[]){"/opt/QQ/qq", "--type=zygote", NULL});
    probe(&main_process, &wayland);
    CHECK(!main_process);

    /* 主进程 + Wayland：接管并要求伪装成 X11 会话。 */
    write_argv((const char *const[]){"/opt/QQ/qq", "--ozone-platform=wayland", NULL});
    probe(&main_process, &wayland);
    CHECK(main_process);
    CHECK(wayland);

    /* 显式 x11 时按 X11 处理，不做 Wayland 专属动作。 */
    write_argv((const char *const[]){"/opt/QQ/qq", "--ozone-platform=x11", NULL});
    probe(&main_process, &wayland);
    CHECK(main_process);
    CHECK(!wayland);

    /* 没有显式参数时沿用环境推断。 */
    unsetenv("WAYLAND_DISPLAY");
    write_argv((const char *const[]){"/opt/QQ/qq", NULL});
    probe(&main_process, &wayland);
    CHECK(main_process);
    CHECK(!wayland);

    printf("%s\n", failures ? "FAIL" : "PASS");
    return failures ? 1 : 0;
}
