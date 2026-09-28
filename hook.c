#include <X11/Xlib.h>
#include <X11/extensions/XShm.h>

#include <pthread.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <stdatomic.h>
#include <stdio.h>
#include <unistd.h>

#include "watcher.h"

struct frame {
    uint8_t *pixels;
    int width;
    int height;
    size_t stride;
};

static pthread_mutex_t frame_lock = PTHREAD_MUTEX_INITIALIZER;
static struct frame current_frame;

/*
 * 发布一帧 BGRx。
 *
 * available：从 src 开始连续可读的字节数。
 * stride：源图每行字节数，必须为正。
 *
 * 函数内部复制数据，不持有源 buffer。
 * 成功返回 0，失败返回 -1。
 */
int hook_publish_bgrx(const void *src,
                     size_t available,
                     int width,
                     int height,
                     size_t stride)
{
    if (!src || width <= 0 || height <= 0)
        return -1;

    if ((size_t)width > SIZE_MAX / 4)
        return -1;

    size_t row_bytes = (size_t)width * 4;

    if (stride < row_bytes)
        return -1;

    /* 检查最后一行末尾是否处于有效数据范围内。 */
    if ((size_t)(height - 1) >
        (SIZE_MAX - row_bytes) / stride)
        return -1;

    size_t required =
        (size_t)(height - 1) * stride + row_bytes;

    if (available < required)
        return -1;

    if ((size_t)height > SIZE_MAX / row_bytes)
        return -1;

    uint8_t *copy = malloc((size_t)height * row_bytes);
    if (!copy)
        return -1;

    for (int y = 0; y < height; ++y) {
        memcpy(copy + (size_t)y * row_bytes,
               (const uint8_t *)src + (size_t)y * stride,
               row_bytes);
    }

    pthread_mutex_lock(&frame_lock);

    uint8_t *old = current_frame.pixels;

    current_frame = (struct frame) {
        .pixels = copy,
        .width = width,
        .height = height,
        .stride = row_bytes,
    };

    /* 放在持有 frame_lock 的区域内。 */
    static unsigned long published = 0;
    ++published;

    if (published <= 5 || published % 120 == 0) {
        fprintf(stderr,
            "[cache pid=%ld] published #%lu "
            "cache=%p size=%dx%d\n",
            (long)getpid(),
            published,
            (void *)&current_frame,
            current_frame.width,
            current_frame.height);
    }

    pthread_mutex_unlock(&frame_lock);

    free(old);
    return 0;
}

/*
 * 停止捕获、流断开或协商格式变化时调用。
 * 与发布操作应由捕获线程按顺序执行。
 */
void hook_clear_frame(void)
{
    pthread_mutex_lock(&frame_lock);

    uint8_t *old = current_frame.pixels;
    current_frame = (struct frame) {0};

    pthread_mutex_unlock(&frame_lock);

    free(old);
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

    /* 先填黑，也清零行尾 padding。 */
    memset(dst->data, 0,
           (size_t)dst->bytes_per_line * dh);

    uint32_t mask =
        (uint32_t)plane_mask & 0x00ffffffu;

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
            int sx = (int)(
                (int64_t)x * src->width / out_w
            );

            const uint8_t *p = src_row + (size_t)sx * 4;
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
}

Bool XShmGetImage(Display *display,
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

    unsigned long n =
        atomic_fetch_add_explicit(
            &calls, 1, memory_order_relaxed) + 1;

    /* 前五次，以及之后每 120 次打印一次。 */
    int log_this = n <= 5 || n % 120 == 0;

    if (log_this) {
        fprintf(stderr,
                "[hook pid=%ld] entered #%lu cache=%p\n",
                (long)getpid(),
                n,
                (void *)&current_frame);

        if (image) {
            fprintf(stderr,
                    "[hook] dst=%dx%d depth=%d bpp=%d "
                    "stride=%d order=%d format=%d "
                    "masks=%lx/%lx/%lx data=%p\n",
                    image->width,
                    image->height,
                    image->depth,
                    image->bits_per_pixel,
                    image->bytes_per_line,
                    image->byte_order,
                    image->format,
                    image->red_mask,
                    image->green_mask,
                    image->blue_mask,
                    (void *)image->data);
        }
    }

    if (!supported_image(image)) {
        if (log_this)
            fprintf(stderr, "[hook] rejected XImage layout\n");

        return False;
    }

    pthread_mutex_lock(&frame_lock);

    if (!current_frame.pixels) {
        if (log_this)
            fprintf(stderr, "[hook] no cached frame\n");

        pthread_mutex_unlock(&frame_lock);
        return False;
    }

    watchdog_timer_reset();
    render_contain(&current_frame, image, plane_mask);

    if (log_this) {
        fprintf(stderr,
                "[hook] rendered %dx%d -> %dx%d; return True\n",
                current_frame.width,
                current_frame.height,
                image->width,
                image->height);
    }

    pthread_mutex_unlock(&frame_lock);

    return True;
}
