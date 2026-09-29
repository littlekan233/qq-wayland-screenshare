#include <glib.h>

#include <stdint.h>
#include <stdio.h>
#include <unistd.h>

#include "watcher.h"
#include "portal_capture.h"
#include "capture_adapter.h"

extern void hook_clear_frame(void);

/* 这些状态只在主进程使用，由 controller_lock 保护。 */
static GMutex controller_lock;
static GCond controller_cond;

static gboolean worker_started;
static gboolean capture_requested;
static uint64_t request_revision;

struct capture_run {
    struct portal_capture *portal;
    struct pw_capture *pw;

    uint64_t revision;
    gboolean finished;
};

static gboolean request_is_current(uint64_t revision)
{
    g_mutex_lock(&controller_lock);

    gboolean current =
        capture_requested &&
        request_revision == revision;

    g_mutex_unlock(&controller_lock);
    return current;
}

static void on_portal_ready(
    void *userdata,
    int status,
    int pipewire_fd,
    const struct portal_stream *stream)
{
    struct capture_run *run = userdata;

    /*
     * 用户选择期间，QQ 可能已经销毁共享窗口。
     * 此时不要再启动 PipeWire。
     */
    if (!request_is_current(run->revision)) {
        if (pipewire_fd >= 0)
            close(pipewire_fd);

        run->finished = TRUE;
        return;
    }

    if (status != PORTAL_CAPTURE_OK || !stream) {
        if (pipewire_fd >= 0)
            close(pipewire_fd);

        fprintf(stderr,
                "[capture] Portal cancelled/failed: %d\n",
                status);

        run->finished = TRUE;
        return;
    }

    /* pw_capture_start 无论成功与否都会接管 FD。 */
    run->pw = pw_capture_start(pipewire_fd, stream);

    if (!run->pw) {
        fprintf(stderr, "[capture] PipeWire start failed\n");
        run->finished = TRUE;
        return;
    }

    fprintf(stderr, "[capture] PipeWire receiver started\n");
}

static void on_portal_closed(void *userdata)
{
    struct capture_run *run = userdata;

    run->finished = TRUE;
    fprintf(stderr, "[capture] Portal session closed\n");
}

static void run_capture_session(uint64_t revision)
{
    struct capture_run run = {
        .revision = revision
    };

    GMainContext *context = g_main_context_new();
    g_main_context_push_thread_default(context);

    hook_clear_frame();

    if (!request_is_current(revision))
        goto cleanup;

    run.portal = portal_capture_start(on_portal_ready, &run);

    if (!run.portal) {
        fprintf(stderr, "[capture] Portal initialization failed\n");
        goto cleanup;
    }

    portal_capture_set_closed_callback(
        run.portal, on_portal_closed, &run);

    for (;;) {
        /* 处理 Portal 异步事件，同时定期检查停止请求。 */
        for (unsigned i = 0; i < 64; ++i) {
            if (!g_main_context_iteration(context, FALSE))
                break;
        }

        if (run.finished)
            break;

        if (!request_is_current(revision)) {
            fprintf(stderr, "[capture] stop requested by windows\n");
            break;
        }

        g_usleep(100000);
    }

cleanup:
    if (run.pw)
        pw_capture_stop(run.pw);

    if (run.portal)
        portal_capture_stop_wait(run.portal);

    hook_clear_frame();

    g_main_context_pop_thread_default(context);
    g_main_context_unref(context);

    fprintf(stderr, "[capture] session stopped\n");
}

/*
 * 控制线程在进程存活期间保留。
 * 同一时刻只运行一个捕获会话。
 */
static gpointer capture_worker(gpointer userdata)
{
    (void)userdata;

    uint64_t handled_revision = 0;

    for (;;) {
        g_mutex_lock(&controller_lock);

        while (!capture_requested ||
               request_revision == handled_revision) {
            g_cond_wait(&controller_cond, &controller_lock);
        }

        uint64_t revision = request_revision;
        handled_revision = revision;

        g_mutex_unlock(&controller_lock);

        run_capture_session(revision);
    }

    return NULL;
}

void do_screencast(void)
{
    g_mutex_lock(&controller_lock);

    if (capture_requested) {
        g_mutex_unlock(&controller_lock);
        return;
    }

    if (!worker_started) {
        GError *error = NULL;

        GThread *thread = g_thread_try_new(
            "qwlss-capture",
            capture_worker,
            NULL,
            &error);

        if (!thread) {
            fprintf(stderr,
                    "[capture] create control thread failed: %s\n",
                    error ? error->message : "unknown error");

            g_clear_error(&error);
            g_mutex_unlock(&controller_lock);
            return;
        }

        worker_started = TRUE;
        g_thread_unref(thread);
    }

    capture_requested = TRUE;
    ++request_revision;

    g_cond_signal(&controller_cond);
    g_mutex_unlock(&controller_lock);
}

void stop_screencast(void)
{
    g_mutex_lock(&controller_lock);

    if (capture_requested) {
        capture_requested = FALSE;
        ++request_revision;

        g_cond_signal(&controller_cond);
    }

    g_mutex_unlock(&controller_lock);
}
