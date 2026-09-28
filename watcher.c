#include <glib.h>
#include <stdio.h>
#include <unistd.h>

#include "portal_capture.h"
#include "capture_adapter.h"

extern void hook_clear_frame(void);

struct capture_run {
    struct portal_capture *portal;
    struct pw_capture *pw;

    gboolean finished;
    gboolean failed;
};

static void on_portal_ready(
    void *userdata,
    int status,
    int pipewire_fd,
    const struct portal_stream *stream)
{
    struct capture_run *run = userdata;

    if (status != PORTAL_CAPTURE_OK || !stream) {
        if (pipewire_fd >= 0)
            close(pipewire_fd);

        fprintf(stderr, "Portal 选择失败或取消：%d\n", status);
        run->failed = TRUE;
        run->finished = TRUE;
        return;
    }

    /*
     * 按前面定义的接口约定：
     * 无论成功或失败，pw_capture_start 都接管 FD。
     */
    run->pw = pw_capture_start(pipewire_fd, stream);

    if (!run->pw) {
        fprintf(stderr, "PipeWire 初始化失败\n");
        run->failed = TRUE;
        run->finished = TRUE;
        return;
    }

    puts("PipeWire 接收已启动，等待视频帧");
}

static void on_portal_closed(void *userdata)
{
    struct capture_run *run = userdata;

    puts("用户或桌面关闭了共享会话");
    run->finished = TRUE;
}

/*
 * 此函数会阻塞，适合在专用控制线程中运行。
 *
 * 当 *idletime == 30 时（即 3 秒内无任何 XShmGetImage 调用），结束共享
 *
 * 所有跨线程访问 stop_flag 的代码都使用 GLib 原子操作。
 */
int capture_run_until_stop(gint *idletime)
{
    struct capture_run run = {0};

    GMainContext *context = g_main_context_new();
    g_main_context_push_thread_default(context);

    hook_clear_frame();

    if (g_atomic_int_get(idletime) == 30)
        goto cleanup;

    run.portal = portal_capture_start(on_portal_ready, &run);

    if (!run.portal) {
        run.failed = TRUE;
        goto cleanup;
    }

    portal_capture_set_closed_callback(
        run.portal, on_portal_closed, &run);

    for (;;) {
        /*
         * 处理 Portal 选择、启动、关闭等事件。
         * 限制每轮处理次数，确保会定期检查停止变量。
         */
        for (unsigned i = 0; i < 64; ++i) {
            if (!g_main_context_iteration(context, FALSE))
                break;
        }

        if (g_atomic_int_get(idletime) == 30)
            break;

        if (run.finished)
            break;

        /* 100000 微秒 = 0.1 秒。 */
        g_usleep(100000);
    }

cleanup:
    /*
     * 先停止接收，确保停止后不会再写入帧缓存。
     * pw_capture_stop 必须等待接收回调结束。
     */
    if (run.pw) {
        pw_capture_stop(run.pw);
        run.pw = NULL;
    }

    /*
     * 再关闭 Portal，并等待本模块的异步回调结束，
     * 然后才能释放 run 和 context。
     */
    if (run.portal) {
        portal_capture_stop_wait(run.portal);
        run.portal = NULL;
    }

    hook_clear_frame();

    g_main_context_pop_thread_default(context);
    g_main_context_unref(context);

    puts("屏幕共享已停止");
    return run.failed ? -1 : 0;
}

// 主注册逻辑
static gint xshm_idle_time = 0;
static gpointer _watchdog_worker(gpointer userdata) {
    gint *arg = userdata;

    int result = capture_run_until_stop(arg);
    return GINT_TO_POINTER(result);
}
void do_screencast() {
    GThread *watchdog = g_thread_new(
        "qwlss-screencast-daemon",
        _watchdog_worker,
        &xshm_idle_time
    );

    int result = GPOINTER_TO_INT(g_thread_join(watchdog));
    if (result == 0) {
        // TODO: gracefully exit log
    } else {
        // TODO: exit with error log
    }
    g_atomic_int_set(&xshm_idle_time, 0);
}
void watchdog_timer_reset() {
    g_atomic_int_set(&xshm_idle_time, 0);
}
