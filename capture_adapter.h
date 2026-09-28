#ifndef CAPTURE_ADAPTER_H
#define CAPTURE_ADAPTER_H

#include "portal_capture.h"

struct pw_capture;

/*
 * 无论成功或失败，都接管 pipewire_fd。
 * 调用后，调用方不得再次关闭这个 FD。
 *
 * 返回非 NULL 表示接收线程已经启动，
 * 不代表已经完成格式协商或收到首帧。
 */
struct pw_capture *pw_capture_start(
    int pipewire_fd,
    const struct portal_stream *source
);

/*
 * 在控制线程调用，不能在 PipeWire 回调中调用。
 * 等待接收线程停止后释放资源。
 */
void pw_capture_stop(struct pw_capture *capture);

#endif
