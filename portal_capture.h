#ifndef PORTAL_CAPTURE_H
#define PORTAL_CAPTURE_H

#include <stdint.h>

struct portal_capture;

struct portal_stream {
    uint32_t node_id;
    int has_serial;
    uint64_t serial;
};

enum portal_capture_status {
    PORTAL_CAPTURE_OK = 0,
    PORTAL_CAPTURE_CANCELLED = 1,
    PORTAL_CAPTURE_ERROR = 2
};

/*
 * 启动结果回调，至多调用一次。
 *
 * 成功：
 *   status == PORTAL_CAPTURE_OK
 *   pipewire_fd 的所有权交给调用方
 *   stream 只在回调期间有效，需要保存时复制
 *
 * 失败：
 *   pipewire_fd == -1
 *   stream == NULL
 *
 * 主动调用 stop 后，不再调用此回调。
 */
typedef void (*portal_ready_fn)(
    void *userdata,
    int status,
    int pipewire_fd,
    const struct portal_stream *stream
);

/*
 * 成功启动后，会话被用户/桌面关闭时调用。
 * 主动调用 stop 不触发该回调。
 */
typedef void (*portal_closed_fn)(void *userdata);

/*
 * 要求：
 *   start、set_closed_callback、stop 都在同一个
 *   GLib 控制线程中调用，该线程必须运行事件循环。
 *
 * 返回的对象在成功或失败后都需要调用 stop 释放。
 */
struct portal_capture *portal_capture_start(
    portal_ready_fn callback,
    void *userdata
);

void portal_capture_set_closed_callback(
    struct portal_capture *capture,
    portal_closed_fn callback,
    void *userdata
);

/*
 * 取消请求、关闭会话并释放调用方持有的对象引用。
 * 调用后 capture 指针无效，不得再次使用。
 *
 * 尚未完成的内部异步回调会自行清理；
 * 控制线程的事件循环需要继续运行以完成这些清理。
 */
void portal_capture_stop(struct portal_capture *capture);

/*
 * 必须在调用 start 的控制线程中执行。
 * 只能从外层控制逻辑调用，不能从 Portal 回调里调用。
 * 调用后 capture 指针无效。
 */
void portal_capture_stop_wait(struct portal_capture *capture);

#endif
