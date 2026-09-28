#define _GNU_SOURCE

#include <xcb/xcb.h>

#include <fcntl.h>
#include <pthread.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include "watcher.h"

static xcb_connection_t *conn;
static xcb_atom_t atom_name;
static xcb_atom_t atom_utf8;
static int log_fd = -1;

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

    if (window != root &&
        attributes->_class == XCB_WINDOW_CLASS_INPUT_OUTPUT &&
        attributes->map_state == XCB_MAP_STATE_VIEWABLE) {

        xcb_get_geometry_reply_t *geometry =
            xcb_get_geometry_reply(
                conn, xcb_get_geometry(conn, window), NULL
            );

        if (geometry &&
            geometry->width == root_width &&
            geometry->height == root_height &&
            is_target(window)) {

            /* checked 请求，让日志能区分成功处理和窗口已销毁。 */
            xcb_generic_error_t *error =
                xcb_request_check(
                    conn, xcb_unmap_window_checked(conn, window)
                );

            if (!error) do_screencast();
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

        if (xcb_connection_has_error(conn))
            break;
    }

    if (log_fd >= 0)
        dprintf(log_fd, "[stopped] X connection closed\n");

done:
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

__attribute__((constructor))
static void start_watcher(void)
{
    if (!is_qq_main_process())
        return;

    char path[128];

    // TODO: logger implementation

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
}
