#include "portal_capture.h"

#include <gio/gio.h>
#include <libportal/portal.h>
#include <unistd.h>

struct portal_capture {
    /*
     * 所有操作均在同一个控制线程中执行，
     * 所以这里不使用原子引用计数。
     */
    unsigned refs;

    XdpPortal *portal;
    XdpSession *session;
    GCancellable *cancel;

    gulong closed_handler;

    gboolean stopped;
    gboolean ended;
    gboolean ready_called;

    portal_ready_fn ready_cb;
    void *ready_data;

    portal_closed_fn closed_cb;
    void *closed_data;
};

static struct portal_capture *
capture_ref(struct portal_capture *c)
{
    ++c->refs;
    return c;
}

static void
capture_unref(struct portal_capture *c)
{
    if (--c->refs != 0)
        return;

    if (c->session && c->closed_handler) {
        g_signal_handler_disconnect(
            c->session, c->closed_handler);
    }

    g_clear_object(&c->session);
    g_clear_object(&c->portal);
    g_clear_object(&c->cancel);
    g_free(c);
}

/*
 * 主动关闭已有会话。
 * 先断开 closed 信号，避免把主动关闭通知成外部关闭。
 */
static void
close_session(struct portal_capture *c)
{
    if (!c->session)
        return;

    if (c->closed_handler) {
        g_signal_handler_disconnect(
            c->session, c->closed_handler);

        c->closed_handler = 0;
    }

    xdp_session_close(c->session);
}

/*
 * 调用本函数时，调用者必须持有自己的对象引用，
 * 因为用户回调中可能调用 portal_capture_stop()。
 */
static void
notify_ready(struct portal_capture *c,
             int status,
             int fd,
             const struct portal_stream *stream)
{
    if (c->stopped || c->ready_called) {
        if (fd >= 0)
            close(fd);
        return;
    }

    c->ready_called = TRUE;

    /* fd 所有权从这里交给用户。 */
    c->ready_cb(c->ready_data, status, fd, stream);
}

static void
fail_capture(struct portal_capture *c,
             const GError *error,
             const char *fallback)
{
    if (c->stopped || c->ended)
        return;

    c->ended = TRUE;

    int status = PORTAL_CAPTURE_ERROR;

    if (error &&
        g_error_matches(error,
                        G_IO_ERROR,
                        G_IO_ERROR_CANCELLED)) {
        status = PORTAL_CAPTURE_CANCELLED;
    } else {
        g_printerr("portal capture: %s\n",
                   error ? error->message : fallback);
    }

    close_session(c);
    notify_ready(c, status, -1, NULL);
}

static void
on_session_closed(XdpSession *session, gpointer userdata)
{
    (void)session;

    struct portal_capture *c = capture_ref(userdata);

    if (c->stopped || c->ended)
        goto done;

    c->ended = TRUE;
    g_cancellable_cancel(c->cancel);

    if (!c->ready_called) {
        /*
         * 选择/启动过程中会话被关闭。
         * 后续异步完成回调会看到 ended 并退出。
         */
        notify_ready(c, PORTAL_CAPTURE_ERROR, -1, NULL);
    } else if (c->closed_cb) {
        c->closed_cb(c->closed_data);
    }

done:
    capture_unref(c);
}

static void
on_session_started(GObject *source,
                   GAsyncResult *result,
                   gpointer userdata)
{
    struct portal_capture *c = userdata;
    GError *error = NULL;

    gboolean ok = xdp_session_start_finish(
        XDP_SESSION(source), result, &error);

    if (c->stopped || c->ended)
        goto done;

    if (!ok) {
        fail_capture(c, error, "无法启动 ScreenCast 会话");
        goto done;
    }

    /*
     * get_streams() 返回拥有引用的 GVariant，
     * 格式为 a(ua{sv})。
     */
    GVariant *streams = xdp_session_get_streams(c->session);

    if (!streams) {
        fail_capture(c, NULL, "Portal 没有返回视频流");
        goto done;
    }

    if (!g_variant_is_of_type(
            streams, G_VARIANT_TYPE("a(ua{sv})")) ||
        g_variant_n_children(streams) != 1) {
        g_variant_unref(streams);
        fail_capture(c, NULL, "预期获得一个视频流");
        goto done;
    }

    GVariant *entry = g_variant_get_child_value(streams, 0);
    GVariant *properties = NULL;
    guint32 node_id = 0;

    g_variant_get(entry, "(u@a{sv})",
                  &node_id, &properties);

    struct portal_stream stream = {
        .node_id = node_id,
        .has_serial = 0,
        .serial = 0
    };

    guint64 serial = 0;

    if (g_variant_lookup(properties,
                         "pipewire-serial",
                         "t",
                         &serial)) {
        stream.has_serial = 1;
        stream.serial = serial;
    }

    g_variant_unref(properties);
    g_variant_unref(entry);
    g_variant_unref(streams);

    /*
     * 此 API 是同步获取 FD 的调用。
     * 应在控制线程执行，不应放进 XShmGetImage hook。
     */
    int fd = xdp_session_open_pipewire_remote(c->session);

    if (fd < 0) {
        fail_capture(c, NULL, "无法获取 PipeWire remote FD");
        goto done;
    }

    notify_ready(c, PORTAL_CAPTURE_OK, fd, &stream);

done:
    g_clear_error(&error);
    capture_unref(c);
}

static void
on_session_created(GObject *source,
                   GAsyncResult *result,
                   gpointer userdata)
{
    struct portal_capture *c = userdata;
    GError *error = NULL;

    XdpSession *session =
        xdp_portal_create_screencast_session_finish(
            XDP_PORTAL(source), result, &error);

    /*
     * 用户可能已经调用 stop，但异步创建刚刚完成。
     * 此时仍要接收并关闭返回的会话。
     */
    if (c->stopped || c->ended) {
        if (session) {
            xdp_session_close(session);
            g_object_unref(session);
        }

        goto done;
    }

    if (!session) {
        fail_capture(c, error, "无法创建 ScreenCast 会话");
        goto done;
    }

    c->session = session;

    c->closed_handler = g_signal_connect(
        c->session,
        "closed",
        G_CALLBACK(on_session_closed),
        c
    );

    /*
     * parent == NULL：没有父窗口。
     * 通常在这个阶段显示选择屏幕/窗口的界面。
     */
    xdp_session_start(
        c->session,
        NULL,
        c->cancel,
        on_session_started,
        capture_ref(c)
    );

done:
    g_clear_error(&error);
    capture_unref(c);
}

struct portal_capture *
portal_capture_start(portal_ready_fn callback,
                     void *userdata)
{
    if (!callback)
        return NULL;

    struct portal_capture *c =
        g_new0(struct portal_capture, 1);

    c->refs = 1;
    c->ready_cb = callback;
    c->ready_data = userdata;

    c->cancel = g_cancellable_new();
    c->portal = xdp_portal_new();

    if (!c->portal) {
        capture_unref(c);
        return NULL;
    }

    xdp_portal_create_screencast_session(
        c->portal,

        /* 允许选择屏幕或窗口。 */
        XDP_OUTPUT_MONITOR | XDP_OUTPUT_WINDOW,

        /* 单个源，不使用 MULTIPLE。 */
        XDP_SCREENCAST_FLAG_NONE,

        /* 第一版不捕获光标。 */
        XDP_CURSOR_MODE_HIDDEN,

        /* 每次重新选择，不持久化授权。 */
        XDP_PERSIST_MODE_NONE,
        NULL,

        c->cancel,
        on_session_created,
        capture_ref(c)
    );

    return c;
}

void
portal_capture_set_closed_callback(
    struct portal_capture *c,
    portal_closed_fn callback,
    void *userdata)
{
    if (!c || c->stopped)
        return;

    c->closed_cb = callback;
    c->closed_data = userdata;
}

void
portal_capture_stop(struct portal_capture *c)
{
    if (!c)
        return;

    /*
     * 调用后指针无效，调用方只能执行一次 stop。
     */
    c->stopped = TRUE;

    c->ready_cb = NULL;
    c->ready_data = NULL;
    c->closed_cb = NULL;
    c->closed_data = NULL;

    g_cancellable_cancel(c->cancel);
    close_session(c);

    /* 释放调用方的引用；异步回调有自己的引用。 */
    capture_unref(c);
}

void portal_capture_stop_wait(struct portal_capture *c)
{
    if (!c)
        return;

    GMainContext *context =
        g_main_context_ref_thread_default();

    /*
     * 临时保留一个引用，避免 stop 后对象被释放。
     * 复用前面实现中的 capture_ref/capture_unref。
     */
    capture_ref(c);

    /* 释放调用方引用，并取消尚未完成的操作。 */
    portal_capture_stop(c);

    /*
     * 等待本模块持有引用的异步回调全部返回。
     * 最后只剩上面临时保留的引用。
     */
    while (c->refs > 1)
        g_main_context_iteration(context, TRUE);

    capture_unref(c);
    g_main_context_unref(context);
}
