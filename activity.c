#define _GNU_SOURCE
#include "activity.h"

#include <errno.h>
#include <fcntl.h>
#include <pthread.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <sys/mman.h>
#include <time.h>
#include <unistd.h>

#include "qwlss_shm.h"

/*
 * 活动通道。
 *
 * 仅在实验版中使用：Wayland ozone 下 QQ 的共享窗口是 Wayland surface，
 * X11 watcher 感知不到共享开始/结束；但 PPAPI 子进程在两种 ozone 下
 * 都会调用 XShmGetImage。子进程在这里打时间戳，主进程的触发线程据此
 * 启动/结束采集。
 */
struct activity_state {
    uint32_t active;
    uint32_t width;
    uint32_t height;
    uint32_t pad;
    uint64_t last_usec;
};

static struct activity_state *activity_map;
static pthread_once_t activity_once = PTHREAD_ONCE_INIT;
static uint64_t activity_min_pixels;

static uint64_t monotonic_usec(void)
{
    struct timespec ts;

    if (clock_gettime(CLOCK_MONOTONIC, &ts) < 0)
        return 0;

    return (uint64_t)ts.tv_sec * UINT64_C(1000000) +
           (uint64_t)(ts.tv_nsec / 1000);
}

static void activity_init_once(void)
{
    /*
     * 尺寸门槛只用于过滤明显的小预览；选择共享源时的全屏取帧与正式共享
     * 无法靠尺寸区分，这一点由实验版的应用层处理共享边框来弥补。
     * 默认值保持保守，避免在小屏上完全无法启动采集。
     */
    activity_min_pixels = UINT64_C(1280) * 720;

    const char *min_env = getenv("QWLSS_MIN_PIXELS");
    if (min_env && *min_env) {
        char *end = NULL;
        errno = 0;
        unsigned long long value = strtoull(min_env, &end, 0);

        if (errno == 0 && end && *end == '\0' && value > 0)
            activity_min_pixels = (uint64_t)value;
    }

    const char *base = qwlss_shm_name();

    if (!base)
        return;

    char name[256];
    int n = snprintf(name, sizeof(name), "%s.act", base);

    if (n < 0 || (size_t)n >= sizeof(name))
        return;

    int fd = shm_open(name, O_RDWR | O_CREAT, 0600);

    if (fd < 0)
        return;

    if (ftruncate(fd, (off_t)sizeof(struct activity_state)) < 0) {
        close(fd);
        return;
    }

    void *map = mmap(NULL, sizeof(struct activity_state),
                     PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);

    close(fd);

    if (map == MAP_FAILED)
        return;

    activity_map = map;
}

static void activity_init(void)
{
    (void)pthread_once(&activity_once, activity_init_once);
}

void qwlss_activity_mark(int width, int height)
{
    activity_init();

    if (!activity_map)
        return;

    if (width < 0)
        width = 0;
    if (height < 0)
        height = 0;

    if ((uint64_t)(uint32_t)width * (uint64_t)(uint32_t)height <
        activity_min_pixels)
        return;

    __atomic_store_n(&activity_map->active, 0u, __ATOMIC_RELEASE);
    __atomic_store_n(&activity_map->width, (uint32_t)width,
                     __ATOMIC_RELEASE);
    __atomic_store_n(&activity_map->height, (uint32_t)height,
                     __ATOMIC_RELEASE);
    __atomic_store_n(&activity_map->last_usec,
                     monotonic_usec(), __ATOMIC_RELEASE);
    __atomic_store_n(&activity_map->active, 1u, __ATOMIC_RELEASE);
}

int qwlss_activity_snapshot(uint64_t *last_usec,
                            uint32_t *width,
                            uint32_t *height)
{
    activity_init();

    if (!activity_map)
        return 0;

    uint32_t active =
        __atomic_load_n(&activity_map->active, __ATOMIC_ACQUIRE);

    if (!active)
        return 0;

    if (last_usec)
        *last_usec =
            __atomic_load_n(&activity_map->last_usec, __ATOMIC_ACQUIRE);

    if (width)
        *width = __atomic_load_n(&activity_map->width, __ATOMIC_ACQUIRE);

    if (height)
        *height = __atomic_load_n(&activity_map->height, __ATOMIC_ACQUIRE);

    return 1;
}
