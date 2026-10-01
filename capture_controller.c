#include <unistd.h>

#include "portal_capture.h"
#include "capture_adapter.h"
#include "runtime.h"

extern void hook_clear_frame(void);

struct capture_controller {
    struct portal_capture *portal;
    struct pw_capture *pipewire;
    int starting;
    int stopping;
};

static void on_portal_ready(
    void *userdata,
    int status,
    int pipewire_fd,
    const struct portal_stream *stream)
{
    struct capture_controller *c = userdata;
    c->starting = 0;

    if (c->stopping || status != 0 || !stream) {
        if (pipewire_fd >= 0)
            close(pipewire_fd);
        return;
    }

    /*
     * 用户已选好内容。
     * 交给接收模块启动捕获；FD 所有权随之转移。
     */
    c->pipewire = pw_capture_start(pipewire_fd, stream);

    if (!c->pipewire) {
        hook_clear_frame();
        return;
    }

    /* 接收器已建立；供实验版判断是否可以处理共享边框。 */
    qwlss_capture_ready(1);
}

static void on_portal_closed(void *userdata)
{
    struct capture_controller *c = userdata;

    if (c->pipewire) {
        pw_capture_stop(c->pipewire);
        c->pipewire = NULL;
    }

    qwlss_capture_ready(0);
    hook_clear_frame();
}

/* 从控制事件循环线程调用。 */
int capture_controller_start(struct capture_controller *c)
{
    if (!c || c->portal || c->pipewire || c->starting)
        return -1;

    c->stopping = 0;
    c->starting = 1;

    c->portal = portal_capture_start(on_portal_ready, c);

    if (!c->portal) {
        c->starting = 0;
        return -1;
    }

    portal_capture_set_closed_callback(
        c->portal,
        on_portal_closed,
        c
    );

    return 0;
}

/* 从同一个控制事件循环线程调用。 */
void capture_controller_stop(struct capture_controller *c)
{
    if (!c)
        return;

    c->stopping = 1;

    if (c->pipewire) {
        pw_capture_stop(c->pipewire);
        c->pipewire = NULL;
    }

    if (c->portal) {
        portal_capture_stop(c->portal);
        c->portal = NULL;
    }

    c->starting = 0;
    qwlss_capture_ready(0);
    hook_clear_frame();
}
