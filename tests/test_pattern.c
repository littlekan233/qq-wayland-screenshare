#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>

extern int hook_publish_bgrx(
    const void *src,
    size_t available,
    int width,
    int height,
    size_t stride
);

/*
 * 加载测试库时，在当前进程发布测试图。
 *
 * 这里没有启动线程，图像只生成一次。
 * 后续 XShmGetImage 使用原来的缓存和缩放实现。
 */
__attribute__((constructor))
static void publish_test_pattern(void)
{
    enum {
        WIDTH = 640,
        HEIGHT = 360,
        STRIDE = WIDTH * 4
    };

    /*
     * RGB 色条：
     * 红、绿、蓝、白、黄、青、品红、黑。
     */
    static const uint8_t colors[8][3] = {
        {255,   0,   0},
        {  0, 255,   0},
        {  0,   0, 255},
        {255, 255, 255},
        {255, 255,   0},
        {  0, 255, 255},
        {255,   0, 255},
        {  0,   0,   0},
    };

    size_t bytes = (size_t)STRIDE * HEIGHT;
    uint8_t *pixels = malloc(bytes);

    if (!pixels) {
        fprintf(stderr,
                "[qwlss-test pid=%ld] allocation failed\n",
                (long)getpid());
        return;
    }

    for (int y = 0; y < HEIGHT; ++y) {
        for (int x = 0; x < WIDTH; ++x) {
            unsigned bar = (unsigned)x * 8 / WIDTH;

            uint8_t *p =
                pixels + (size_t)y * STRIDE + (size_t)x * 4;

            /* RGB → BGRx。 */
            p[0] = colors[bar][2];
            p[1] = colors[bar][1];
            p[2] = colors[bar][0];
            p[3] = 0;
        }
    }

    int result = hook_publish_bgrx(
        pixels,
        bytes,
        WIDTH,
        HEIGHT,
        STRIDE
    );

    /* publish 内部已经复制，源缓冲区可以释放。 */
    free(pixels);

    fprintf(stderr,
            "[qwlss-test pid=%ld] loaded; "
            "test pattern publish=%d, size=%dx%d\n",
            (long)getpid(),
            result,
            WIDTH,
            HEIGHT);
}
