#ifndef QWLSS_ACTIVITY_H
#define QWLSS_ACTIVITY_H

#include <stdint.h>

/*
 * 活动通道: 由调用 XShmGetImage 的子进程(PPAPI)上报最近一次达到门限的取帧。
 * 选择共享目标阶段也可能调用 XShmGetImage；小预览帧不会刷新活动时间戳。
 */
void qwlss_activity_mark(int width, int height);

/*
 * 读取活动状态。
 * 返回 0: 尚无屏幕量级活动；1: 有活动，last_usec 为最后一次调用的
 * CLOCK_MONOTONIC 微秒时间戳，width/height 为该次 XImage 尺寸。
 */
int qwlss_activity_snapshot(uint64_t *last_usec,
                            uint32_t *width,
                            uint32_t *height);

#endif
