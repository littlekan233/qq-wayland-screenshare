#include <X11/Xlib.h>
#include <X11/extensions/XShm.h>

#include <pthread.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <stdatomic.h>
#include <stdio.h>
#include <unistd.h>
#include <errno.h>
#include <inttypes.h>

#include "qwlss_shm.h"
#include "activity.h"
#include "watcher.h"

struct frame {
    uint8_t *pixels;
    int width;
    int height;
    size_t stride;
};

/*
 * 发布一帧 BGRx。
 *
 * available：从 src 开始连续可读的字节数。
 * stride：源图每行字节数，必须为正。
 *
 * 函数内部复制数据，不持有源 buffer。
 * 成功返回 0，失败返回 -1。
 */
int hook_publish_bgrx(
    const void *src,
    size_t available,
    int width,
    int height,
    size_t stride)
{
    int result = qwlss_shm_publish(
        src, available, width, height, stride);

    int saved = errno;

    static atomic_ulong publications = ATOMIC_VAR_INIT(0);
    unsigned long n = atomic_fetch_add(&publications, 1) + 1;

    if (n <= 5 || n % 120 == 0) {
        const char *name = getenv("QWLSS_SHM_NAME");

        if (result == 0) {
            fprintf(stderr,
                    "[shm-write pid=%ld] %s frame=%lu %dx%d\n",
                    (long)getpid(),
                    name ? name : "(unset)",
                    n, width, height);
        } else {
            fprintf(stderr,
                    "[shm-write pid=%ld] %s failed: %s\n",
                    (long)getpid(),
                    name ? name : "(unset)",
                    strerror(saved));
        }
    }

    errno = saved;
    return result;
}

/*
 * 停止捕获、流断开或协商格式变化时调用。
 * 与发布操作应由捕获线程按顺序执行。
 */
void hook_clear_frame(void)
{
    if (qwlss_shm_clear() < 0) {
        fprintf(stderr, "[shm-clear pid=%ld] %s\n",
                (long)getpid(), strerror(errno));
    }
}

static int supported_image(const XImage *image)
{
    if (!image || !image->data)
        return 0;

    if (image->width <= 0 ||
        image->height <= 0 ||
        image->bytes_per_line <= 0)
        return 0;

    if (image->format != ZPixmap ||
        image->depth != 24 ||
        image->bits_per_pixel != 32 ||
        image->byte_order != LSBFirst ||
        image->xoffset != 0 ||
        image->red_mask != 0x00ff0000UL ||
        image->green_mask != 0x0000ff00UL ||
        image->blue_mask != 0x000000ffUL)
        return 0;

    /* 每行至少能存放 width 个像素。 */
    if ((size_t)image->width >
        (size_t)image->bytes_per_line / 4)
        return 0;

    if ((size_t)image->height >
        SIZE_MAX / (size_t)image->bytes_per_line)
        return 0;

    return 1;
}

/*
 * 等比缩放 + 居中 + 黑边。
 * 调用方必须持有 frame_lock。
 */
static void render_contain(const struct frame *src,
                           XImage *dst,
                           unsigned long plane_mask)
{
    int dw = dst->width;
    int dh = dst->height;

    int out_w;
    int out_h;

    /* 整数计算，避免浮点舍入导致越界。 */
    if ((int64_t)dw * src->height <=
        (int64_t)dh * src->width) {
        out_w = dw;
        out_h = (int)(
            (int64_t)src->height * dw / src->width
        );

        if (out_h < 1)
            out_h = 1;
    } else {
        out_h = dh;
        out_w = (int)(
            (int64_t)src->width * dh / src->height
        );

        if (out_w < 1)
            out_w = 1;
    }

    int offset_x = (dw - out_w) / 2;
    int offset_y = (dh - out_h) / 2;

    uint32_t mask =
        (uint32_t)plane_mask & 0x00ffffffu;

    /*
     * 1:1 且源/目标布局一致时整块拷贝。
     * 共享画面通常就是同尺寸同 stride，这条路径占绝大多数，
     * 可省掉逐像素循环(原先每帧约 width*height 次整数除法)。
     */
    if (out_w == dw && out_h == dh &&
        out_w == src->width && out_h == src->height &&
        dst->bytes_per_line == (int)src->stride &&
        mask == 0x00ffffffu) {
        memcpy(dst->data, src->pixels,
               (size_t)src->stride * (size_t)src->height);
        return;
    }

    /* 先填黑，也清零行尾 padding。 */
    memset(dst->data, 0,
           (size_t)dst->bytes_per_line * dh);

    /* 无缩放但有黑边: 按行拷贝，不做逐像素映射。 */
    if (out_w == src->width && out_h == src->height) {
        for (int y = 0; y < out_h; ++y) {
            const uint8_t *src_row =
                src->pixels + (size_t)y * src->stride;

            uint8_t *dst_row =
                (uint8_t *)dst->data +
                (size_t)(offset_y + y) * dst->bytes_per_line +
                (size_t)offset_x * 4;

            if (mask == 0x00ffffffu) {
                memcpy(dst_row, src_row, (size_t)out_w * 4);
                continue;
            }

            for (int x = 0; x < out_w; ++x) {
                const uint8_t *p = src_row + (size_t)x * 4;
                uint8_t *q = dst_row + (size_t)x * 4;

                uint32_t pixel =
                    (uint32_t)p[0] |
                    ((uint32_t)p[1] << 8) |
                    ((uint32_t)p[2] << 16);

                pixel &= mask;

                q[0] = (uint8_t)pixel;
                q[1] = (uint8_t)(pixel >> 8);
                q[2] = (uint8_t)(pixel >> 16);
                q[3] = 0;
            }
        }

        return;
    }

    /*
     * 缩放路径: 每列的源坐标只与 x 有关，先算一次，
     * 避免逐像素整数除法。
     */
    int *xmap = malloc(sizeof(int) * (size_t)out_w);

    if (!xmap) {
        /* 极端情况: 分配失败则保持已清零的黑帧。 */
        return;
    }

    for (int x = 0; x < out_w; ++x) {
        xmap[x] = (int)(
            (int64_t)x * src->width / out_w
        );
    }

    for (int y = 0; y < out_h; ++y) {
        int sy = (int)(
            (int64_t)y * src->height / out_h
        );

        const uint8_t *src_row =
            src->pixels + (size_t)sy * src->stride;

        uint8_t *dst_row =
            (uint8_t *)dst->data +
            (size_t)(offset_y + y) * dst->bytes_per_line +
            (size_t)offset_x * 4;

        for (int x = 0; x < out_w; ++x) {
            const uint8_t *p = src_row + (size_t)xmap[x] * 4;
            uint8_t *q = dst_row + (size_t)x * 4;

            uint32_t pixel =
                (uint32_t)p[0] |
                ((uint32_t)p[1] << 8) |
                ((uint32_t)p[2] << 16);

            pixel &= mask;

            /* 目标布局是 LSBFirst 的 32 位像素。 */
            q[0] = (uint8_t)pixel;
            q[1] = (uint8_t)(pixel >> 8);
            q[2] = (uint8_t)(pixel >> 16);
            q[3] = 0;
        }
    }

    free(xmap);
}

Bool XShmGetImage(
    Display *display,
    Drawable drawable,
    XImage *image,
    int x,
    int y,
    unsigned long plane_mask)
{
    (void)display;
    (void)drawable;
    (void)x;
    (void)y;

    static atomic_ulong calls = ATOMIC_VAR_INIT(0);
    unsigned long n = atomic_fetch_add(&calls, 1) + 1;

    /*
     * 上报最近一次取帧。注意: 选择共享目标的预览阶段也会走到这里，
     * 所以带上尺寸，由主进程判断是否达到屏幕量级再启动捕获。
     */
    qwlss_activity_mark(image->width, image->height);

    int log_this = n <= 20 || n % 120 == 0;

    if (log_this) {
        fprintf(stderr,
                "[hook pid=%ld] call=%lu image=%dx%d\n",
                (long)getpid(), n, image->width, image->height);
    }

    if (!supported_image(image)) {
        if (log_this) {
            fprintf(stderr,
                    "[hook pid=%ld] call=%lu unsupported XImage\n",
                    (long)getpid(), n);
        }
        return False;
    }

    struct qwlss_snapshot snapshot = {0};
    int status = qwlss_shm_read(&snapshot);

    if (status == 0) {
        /*
         * 当前尚无有效帧。
         * supported_image 已经检查布局和长度乘法。
         */
        memset(image->data, 0,
               (size_t)image->bytes_per_line *
               (size_t)image->height);

        if (log_this) {
            fprintf(stderr,
                    "[hook pid=%ld] call=%lu "
                    "waiting for frame; output black, return True\n",
                    (long)getpid(), n);
        }

        return True;
    }

    if (status < 0) {
        int saved = errno;

        if (log_this) {
            fprintf(stderr,
                    "[hook pid=%ld] call=%lu shm read failed: %s\n",
                    (long)getpid(), n, strerror(saved));
        }

        return False;
    }

    struct frame source = {
        .pixels = snapshot.pixels,
        .width = snapshot.width,
        .height = snapshot.height,
        .stride = snapshot.stride
    };

    render_contain(&source, image, plane_mask);

    if (log_this) {
        fprintf(stderr,
                "[hook pid=%ld] call=%lu "
                "rendered seq=%" PRIu64 " %dx%d -> %dx%d\n",
                (long)getpid(),
                n,
                snapshot.sequence,
                snapshot.width,
                snapshot.height,
                image->width,
                image->height);
    }

    free(snapshot.pixels);
    return True;
}
