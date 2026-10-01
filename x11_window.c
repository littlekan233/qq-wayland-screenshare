#define _GNU_SOURCE

#include <xcb/xcb.h>
#include <xcb/randr.h>

#include <fcntl.h>
#include <glib.h>
#include <inttypes.h>
#include <pthread.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

#include "qwlss_shm.h"
#include "activity.h"
#include "runtime.h"
#include "watcher.h"

static xcb_connection_t *conn;
static xcb_atom_t atom_name;
static xcb_atom_t atom_utf8;
static int log_fd = -1;
/* 仅由窗口监听线程访问。 */
static GHashTable *share_windows;
static gboolean window_capture_requested;

static void track_share_window(xcb_window_t window)
{
    if (g_hash_table_add(
            share_windows, GUINT_TO_POINTER(window))) {
        fprintf(stderr,
                "[window] track 0x%x, total=%u\n",
                (unsigned)window,
                g_hash_table_size(share_windows));
    }
}

static void forget_share_window(
    xcb_window_t window,
    const char *reason)
{
    if (!g_hash_table_remove(
            share_windows, GUINT_TO_POINTER(window)))
        return;

    fprintf(stderr,
            "[window] untrack 0x%x, reason=%s, remaining=%u\n",
            (unsigned)window,
            reason,
            g_hash_table_size(share_windows));
}

static void stop_if_no_share_windows(void)
{
    if (!window_capture_requested)
        return;

    if (g_hash_table_size(share_windows) != 0)
        return;

    window_capture_requested = FALSE;

    fprintf(stderr,
            "[window] no matching share windows remain; "
            "stopping capture\n");

    stop_screencast();
}

static xcb_atom_t intern_atom(const char *name)
{
    xcb_intern_atom_reply_t *reply =
        xcb_intern_atom_reply(
            conn,
            xcb_intern_atom(conn, 0, (uint16_t)strlen(name), name),
            NULL
        );

    if (!reply)
        return XCB_ATOM_NONE;

    xcb_atom_t atom = reply->atom;
    free(reply);
    return atom;
}

/* 精确比较属性内容，包括 WM_CLASS 中间及末尾的 NUL。 */
static int property_equals(
    xcb_window_t window,
    xcb_atom_t property,
    xcb_atom_t type,
    const void *expected,
    size_t expected_length
)
{
    xcb_get_property_reply_t *reply =
        xcb_get_property_reply(
            conn,
            xcb_get_property(
                conn, 0, window, property, type, 0, 1024
            ),
            NULL
        );

    if (!reply)
        return 0;

    int matches =
        reply->type == type &&
        reply->format == 8 &&
        reply->bytes_after == 0 &&
        (size_t)xcb_get_property_value_length(reply) == expected_length &&
        memcmp(xcb_get_property_value(reply),
               expected, expected_length) == 0;

    free(reply);
    return matches;
}

static int is_target(xcb_window_t window)
{
    /* 数组内容恰好为 qq\0QQ\0。 */
    static const char qq_class[] = "qq\0QQ";
    static const char title[] = "屏幕共享";

    return property_equals(
               window, XCB_ATOM_WM_CLASS, XCB_ATOM_STRING,
               qq_class, sizeof(qq_class)
           ) &&
           property_equals(
               window, atom_name, atom_utf8,
               title, sizeof(title) - 1
           );
}

/*
 * 判断窗口是否"铺满一块屏幕"。
 * 单显示器时就是整个 X screen；多显示器(Xwayland)下 QQ 的大蓝框
 * 只覆盖它所在的那块显示器，所以还要和 RandR 的 monitor 矩形比较。
 */
static int covers_screen_or_monitor(
    xcb_window_t root,
    uint16_t width,
    uint16_t height,
    uint16_t root_width,
    uint16_t root_height
)
{
    if (width == root_width && height == root_height)
        return 1;

    xcb_randr_get_monitors_reply_t *reply =
        xcb_randr_get_monitors_reply(
            conn,
            xcb_randr_get_monitors(conn, root, 1),
            NULL
        );

    if (!reply)
        return 0;

    int matches = 0;
    xcb_randr_monitor_info_iterator_t it =
        xcb_randr_get_monitors_monitors_iterator(reply);

    for (; it.rem; xcb_randr_monitor_info_next(&it)) {
        if (it.data->width == width && it.data->height == height) {
            matches = 1;
            break;
        }
    }

    free(reply);
    return matches;
}

static void scan_tree(
    xcb_window_t window,
    xcb_window_t root,
    uint16_t root_width,
    uint16_t root_height
)
{
    uint32_t mask =
        XCB_EVENT_MASK_STRUCTURE_NOTIFY |
        XCB_EVENT_MASK_SUBSTRUCTURE_NOTIFY |
        XCB_EVENT_MASK_PROPERTY_CHANGE;

    /*
     * 只修改我们这个连接的事件订阅。
     * 不申请 SubstructureRedirect。
     */
    xcb_change_window_attributes(
        conn, window, XCB_CW_EVENT_MASK, &mask
    );

    xcb_get_window_attributes_reply_t *attributes =
        xcb_get_window_attributes_reply(
            conn, xcb_get_window_attributes(conn, window), NULL
        );

    /* 窗口可能已被销毁。 */
    if (!attributes)
        return;

    int target =
        window != root &&
        attributes->_class == XCB_WINDOW_CLASS_INPUT_OUTPUT &&
        is_target(window);

    /*
     * 集合记录的是当前仍匹配共享条件的窗口。
     *
     * 是否可见不影响归属：
     * 我们主动 unmap 后，标题和 WM_CLASS 仍然匹配。
 */
    if (target) {
        track_share_window(window);
    } else {
        forget_share_window(
            window,
            "title/class no longer matches"
        );
    }
    if (target &&
        attributes->map_state == XCB_MAP_STATE_VIEWABLE) {

        xcb_get_geometry_reply_t *geometry =
            xcb_get_geometry_reply(
                conn, xcb_get_geometry(conn, window), NULL
            );

        if (geometry &&
            covers_screen_or_monitor(root, geometry->width, geometry->height,
                                     root_width, root_height) &&
            is_target(window)) {

            /* checked 请求，让日志能区分成功处理和窗口已销毁。 */
            xcb_generic_error_t *error =
                xcb_request_check(
                    conn, xcb_unmap_window_checked(conn, window)
                );

	    if (!error && !window_capture_requested) {
                window_capture_requested = TRUE;
                do_screencast();
            }
            if (log_fd >= 0) {
                if (!error) {
                    dprintf(log_fd,
                            "[unmap] window=0x%x size=%ux%u\n",
                            (unsigned int)window,
                            (unsigned int)geometry->width,
                            (unsigned int)geometry->height);
                } else {
                    dprintf(log_fd,
                            "[unmap failed] window=0x%x error=%u\n",
                            (unsigned int)window,
                            (unsigned int)error->error_code);
                }
            }
            free(error);
        }

        free(geometry);
    }

    free(attributes);

    xcb_query_tree_reply_t *tree =
        xcb_query_tree_reply(
            conn, xcb_query_tree(conn, window), NULL
        );

    if (!tree)
        return;

    int count = xcb_query_tree_children_length(tree);
    xcb_window_t *children = xcb_query_tree_children(tree);

    for (int i = 0; i < count; ++i)
        scan_tree(children[i], root, root_width, root_height);

    free(tree);
}

static void scan_all(void)
{
    xcb_screen_iterator_t iter =
        xcb_setup_roots_iterator(xcb_get_setup(conn));

    for (; iter.rem; xcb_screen_next(&iter)) {
        xcb_window_t root = iter.data->root;

        uint32_t mask =
            XCB_EVENT_MASK_STRUCTURE_NOTIFY |
            XCB_EVENT_MASK_SUBSTRUCTURE_NOTIFY |
            XCB_EVENT_MASK_PROPERTY_CHANGE;

        xcb_change_window_attributes(
            conn, root, XCB_CW_EVENT_MASK, &mask
        );

        /* 查询实时尺寸，不使用连接建立时缓存的屏幕尺寸。 */
        xcb_get_geometry_reply_t *geometry =
            xcb_get_geometry_reply(
                conn, xcb_get_geometry(conn, root), NULL
            );

        if (geometry) {
            scan_tree(root, root, geometry->width, geometry->height);
            free(geometry);
        }
    }

    xcb_flush(conn);
}

static int relevant_event(const xcb_generic_event_t *event)
{
    switch (event->response_type & 0x7f) {
    case XCB_CREATE_NOTIFY:
    case XCB_MAP_NOTIFY:
    case XCB_REPARENT_NOTIFY:
    case XCB_CONFIGURE_NOTIFY:
        return 1;

    case XCB_PROPERTY_NOTIFY: {
        const xcb_property_notify_event_t *property =
            (const xcb_property_notify_event_t *)event;

        return property->atom == atom_name ||
               property->atom == XCB_ATOM_WM_CLASS;
    }
    case XCB_DESTROY_NOTIFY: {
        const xcb_destroy_notify_event_t *destroy =
            (const xcb_destroy_notify_event_t *)event;

        forget_share_window(destroy->window, "destroyed");
        return 1;
    }

    default:
        /*
         * 包括 UnmapNotify：我们主动隐藏窗口后，
         * 不需要因此再扫描一遍。
         */
        return 0;
    }
}

static void *watcher_main(void *unused)
{
    (void)unused;

    share_windows = g_hash_table_new(
        g_direct_hash, g_direct_equal);
    conn = xcb_connect(NULL, NULL);

    int error = xcb_connection_has_error(conn);
    if (error) {
        if (log_fd >= 0)
            dprintf(log_fd, "[error] XCB connection failed: %d\n", error);
        goto done;
    }

    atom_name = intern_atom("_NET_WM_NAME");
    atom_utf8 = intern_atom("UTF8_STRING");

    if (!atom_name || !atom_utf8) {
        if (log_fd >= 0)
            dprintf(log_fd, "[error] atom initialization failed\n");
        goto done;
    }

    if (log_fd >= 0)
        dprintf(log_fd, "[ready] listening for window events\n");

    scan_all();

    for (;;) {
        /* 无事件时阻塞，没有定时轮询。 */
        xcb_generic_event_t *event = xcb_wait_for_event(conn);
        if (!event)
            break;

        int dirty = relevant_event(event);
        free(event);

        /* 合并一批已到达事件，避免连续重复扫描。 */
        for (int i = 0; i < 255; ++i) {
            event = xcb_poll_for_event(conn);
            if (!event)
                break;

            dirty |= relevant_event(event);
            free(event);
        }

        if (dirty)
            scan_all();

	stop_if_no_share_windows();
        if (xcb_connection_has_error(conn))
            break;
    }

    if (log_fd >= 0)
        dprintf(log_fd, "[stopped] X connection closed\n");

done:
    stop_screencast();

    if (share_windows) {
        g_hash_table_destroy(share_windows);
       share_windows = NULL;
    }
    xcb_disconnect(conn);

    if (log_fd >= 0) {
        close(log_fd);
        log_fd = -1;
    }

    return NULL;
}

/*
 * 针对你当前 /opt/QQ/qq 的启动结构：
 * 主进程启动监听，--type=... 子进程跳过。
 */
static int is_qq_main_process(void)
{
    char executable[4096];
    ssize_t length =
        readlink("/proc/self/exe", executable, sizeof(executable) - 1);

    if (length <= 0)
        return 0;

    executable[length] = '\0';
    const char *base = strrchr(executable, '/');
    base = base ? base + 1 : executable;

    if (strcmp(base, "qq") != 0)
        return 0;

    char args[65536];
    int fd = open("/proc/self/cmdline", O_RDONLY | O_CLOEXEC);
    if (fd < 0)
        return 0;

    ssize_t count = read(fd, args, sizeof(args));
    close(fd);

    if (count <= 0 || count == (ssize_t)sizeof(args))
        return 0;

    for (size_t pos = 0; pos < (size_t)count;) {
        size_t available = (size_t)count - pos;
        size_t size = strnlen(args + pos, available);

        if (size == available)
            return 0;

        if (size >= 7 && memcmp(args + pos, "--type=", 7) == 0)
            return 0;

        pos += size + 1;
    }

    return 1;
}

/*
 * Wayland ozone 兼容: QQ 的共享窗口不再是 X11 窗口，watcher 看不到。
 * PPAPI 子进程仍在调用 XShmGetImage，因此以该活动信号作为
 * "共享开始/结束" 的触发器。X11 ozone 下这个线程只是重复确认，
 * do_screencast 可重入、stop_screencast 幂等，不会冲突。
 */
static uint64_t monotonic_usec(void)
{
    struct timespec ts;

    if (clock_gettime(CLOCK_MONOTONIC, &ts) < 0)
        return 0;

    return (uint64_t)ts.tv_sec * UINT64_C(1000000) +
           (uint64_t)(ts.tv_nsec / 1000);
}

static void *activity_trigger_main(void *unused)
{
    (void)unused;

    /* qwlss_activity_mark 已过滤掉预览小图；这里仅负责时间生命周期。 */
    const uint64_t active_window_usec = UINT64_C(2000000);
    const uint64_t idle_stop_usec = UINT64_C(3000000);
    uint32_t last_w = 0;
    uint32_t last_h = 0;
    int idle_reported = 0;

    for (;;) {
        usleep(200000);

        uint64_t last = 0;
        uint32_t w = 0;
        uint32_t h = 0;

        if (!qwlss_activity_snapshot(&last, &w, &h))
            continue;

        uint64_t age = monotonic_usec() - last;

        if (age <= active_window_usec) {
            idle_reported = 0;

            if (w != last_w || h != last_h) {
                fprintf(stderr,
                        "[capture] activity %ux%u; starting capture\n",
                        w, h);
                last_w = w;
                last_h = h;
            }

            do_screencast();
        } else if (age >= idle_stop_usec && !idle_reported) {
            idle_reported = 1;
            last_w = 0;
            last_h = 0;

            fprintf(stderr,
                    "[capture] XShmGetImage idle; stopping capture\n");
            stop_screencast();
        }
    }

    return NULL;
}

__attribute__((constructor))
static void start_watcher(void)
{
    if (!is_qq_main_process())
        return;

    char path[128];

    // TODO: logger implementation

    /*
     * Wayland ozone: QQ 的共享窗口是 Wayland surface，X11 watcher 看不到，
     * 但 PPAPI 仍然会持续调用 XShmGetImage。此时用活动信号驱动采集的
     * 生命周期，并由实验版处理共享边框。
     */
    if (qwlss_uses_wayland())
        goto activity;

    pthread_t thread;
    int error = pthread_create(&thread, NULL, watcher_main, NULL);

    if (error) {
        if (log_fd >= 0) {
            dprintf(log_fd, "[error] pthread_create: %s\n",
                    strerror(error));
            close(log_fd);
            log_fd = -1;
        }
        return;
    }

    pthread_detach(thread);

activity:;
    pthread_t trigger;
    int trigger_error =
        pthread_create(&trigger, NULL, activity_trigger_main, NULL);

    if (trigger_error) {
        fprintf(stderr,
                "[capture] activity trigger thread failed: %s\n",
                strerror(trigger_error));
        return;
    }

    pthread_detach(trigger);
}
