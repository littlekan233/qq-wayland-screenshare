#include <glib.h>

#include <errno.h>
#include <inttypes.h>
#include <stdatomic.h>
#include <stdio.h>
#include <string.h>
#include <unistd.h>

#include "watcher.h"
#include "portal_capture.h"
#include "capture_adapter.h"
#include "qwlss_shm.h"

extern void hook_clear_frame(void);

#define STARTUP_TIMEOUT_US (15 * G_USEC_PER_SEC)
#define IDLE_TIMEOUT_US    (3 * G_USEC_PER_SEC)

struct capture_run {
    struct portal_capture *portal;
    struct pw_capture *pw;

    uint64_t session;
    uint64_t last_ticks;
    gint64 last_change_us;

    gboolean armed;
    gboolean observed_request;
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

        fprintf(stderr, "[watchdog] Portal cancelled/failed: %d\n",
                status);

        run->failed = TRUE;
        run->finished = TRUE;
        return;
    }

    /* 此调用无论成功与否都接管 FD。 */
    run->pw = pw_capture_start(pipewire_fd, stream);

    if (!run->pw) {
        fprintf(stderr, "[watchdog] PipeWire start failed\n");
        run->failed = TRUE;
        run->finished = TRUE;
        return;
    }

    int result = qwlss_wd_arm(run->session);

    if (result != 1) {
        fprintf(stderr, "[watchdog] arm failed: %s\n",
                result < 0 ? strerror(errno) : "session changed");

        run->failed = TRUE;
        run->finished = TRUE;
        return;
    }

    run->armed = TRUE;
    run->last_ticks = 0;
    run->observed_request = FALSE;
    run->last_change_us = g_get_monotonic_time();

    fprintf(stderr,
            "[watchdog pid=%ld] armed session=%" PRIu64 "\n",
            (long)getpid(), run->session);
}

static void on_portal_closed(void *userdata)
{
    struct capture_run *run = userdata;
    run->finished = TRUE;

    fprintf(stderr, "[watchdog] Portal session closed\n");
}

/* 返回 TRUE 表示应结束捕获。 */
static gboolean check_watchdog(struct capture_run *run)
{
    if (!run->armed)
        return FALSE;

    uint64_t ticks = 0;
    int result = qwlss_wd_poll(run->session, &ticks);

    if (result != 1) {
        if (result < 0) {
            fprintf(stderr, "[watchdog] poll failed: %s\n",
                    strerror(errno));
            run->failed = TRUE;
        }
        return TRUE;
    }

    gint64 now = g_get_monotonic_time();

    if (ticks != run->last_ticks) {
        if (!run->observed_request) {
            fprintf(stderr,
                    "[watchdog] first screenshot request, "
                    "session=%" PRIu64 "\n",
                    run->session);
        }

        run->observed_request = TRUE;
        run->last_ticks = ticks;
        run->last_change_us = now;
        return FALSE;
    }

    gint64 timeout = run->observed_request
        ? IDLE_TIMEOUT_US
        : STARTUP_TIMEOUT_US;

    if (now - run->last_change_us < timeout)
        return FALSE;

    /*
     * 再次在共享锁内核对序号。
     * 若期间收到新心跳，下轮循环重新计时。
     */
    result = qwlss_wd_expire(run->session, ticks);

    if (result < 0) {
        fprintf(stderr, "[watchdog] expire failed: %s\n",
                strerror(errno));
        run->failed = TRUE;
        return TRUE;
    }

    if (result == 1) {
        fprintf(stderr, "[watchdog] %s; stopping capture\n",
                run->observed_request
                    ? "no screenshot requests for 3 seconds"
                    : "no screenshot requests during startup");

        return TRUE;
    }

    return FALSE;
}

/*
 * 专用控制线程执行。
 * stop_flag 可为 NULL；非 NULL 时，值为 1 表示主动停止。
 */
int capture_run_until_stop(gint *stop_flag)
{
    struct capture_run run = {0};

    GMainContext *context = g_main_context_new();
    g_main_context_push_thread_default(context);

    hook_clear_frame();

    if (stop_flag && g_atomic_int_get(stop_flag) == 1)
        goto cleanup;

    if (qwlss_wd_begin(&run.session) != 1) {
        fprintf(stderr, "[watchdog] begin failed: %s\n",
                strerror(errno));
        run.failed = TRUE;
        goto cleanup;
    }

    run.portal = portal_capture_start(on_portal_ready, &run);

    if (!run.portal) {
        run.failed = TRUE;
        goto cleanup;
    }

    portal_capture_set_closed_callback(
        run.portal, on_portal_closed, &run);

    for (;;) {
        for (unsigned i = 0; i < 64; ++i) {
            if (!g_main_context_iteration(context, FALSE))
                break;
        }

        if (run.finished)
            break;

        if (stop_flag && g_atomic_int_get(stop_flag) == 1)
            break;

        if (check_watchdog(&run))
            break;

        g_usleep(100000);
    }

cleanup:
    /*
     * 先禁止继续喂本次会话。
     * 此函数返回时已释放共享锁，不持锁清理 PipeWire。
     */
    if (run.session)
        (void)qwlss_wd_end(run.session);

    if (run.pw)
        pw_capture_stop(run.pw);

    if (run.portal)
        portal_capture_stop_wait(run.portal);

    hook_clear_frame();

    g_main_context_pop_thread_default(context);
    g_main_context_unref(context);

    fprintf(stderr, "[watchdog] capture stopped\n");
    return run.failed ? -1 : 0;
}

static gint capture_running = 0;

static gpointer capture_worker(gpointer userdata)
{
    (void)userdata;

    int result = capture_run_until_stop(NULL);

    g_atomic_int_set(&capture_running, 0);
    return GINT_TO_POINTER(result);
}

void do_screencast(void)
{
    /* 主进程内防止重复启动同一捕获会话。 */
    if (!g_atomic_int_compare_and_exchange(
            &capture_running, 0, 1))
        return;

    GError *error = NULL;

    GThread *thread = g_thread_try_new(
        "qwlss-capture",
        capture_worker,
        NULL,
        &error);

    if (!thread) {
        fprintf(stderr, "[watchdog] create thread: %s\n",
                error ? error->message : "unknown error");

        g_clear_error(&error);
        g_atomic_int_set(&capture_running, 0);
        return;
    }

    /* 不阻塞调用 do_screencast() 的窗口监听线程。 */
    g_thread_unref(thread);
}

void watchdog_timer_reset(void)
{
    int result = qwlss_wd_beat();

    if (result < 0) {
        int saved = errno;

        static atomic_uint errors = ATOMIC_VAR_INIT(0);

        if (atomic_fetch_add(&errors, 1) < 3) {
            fprintf(stderr,
                    "[watchdog pid=%ld] heartbeat failed: %s\n",
                    (long)getpid(), strerror(saved));
        }
    }
}
