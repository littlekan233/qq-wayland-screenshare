/*
 * 稳定版占位实现。
 *
 * startup.c 的构造函数在两个版本中都会编译；稳定版不区分 ozone 后端、
 * 也不发布采集状态，因此这里提供最小实现，保持链接完整。
 */
#include "activity.h"
#include "runtime.h"

int qwlss_is_main_process(void) { return 0; }

int qwlss_uses_wayland(void) { return 0; }

const char *qwlss_executable(void) { return ""; }

void qwlss_set_capture_flag(const char *path)
{
    (void)path;
}

void qwlss_capture_ready(int ready)
{
    (void)ready;
}

/* 稳定版不区分 ozone 后端，活动通道不参与采集生命周期。 */
void qwlss_activity_mark(int width, int height)
{
    (void)width;
    (void)height;
}

int qwlss_activity_snapshot(uint64_t *last_usec,
                            uint32_t *width,
                            uint32_t *height)
{
    (void)last_usec;
    (void)width;
    (void)height;
    return 0;
}
