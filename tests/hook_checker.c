#include <X11/Xlib.h>

#include <dlfcn.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>

typedef Bool (*get_image_fn)(
    Display *,
    Drawable,
    XImage *,
    int,
    int,
    unsigned long
);

static uint32_t read_pixel(const XImage *image, int x, int y)
{
    const uint8_t *p =
        (const uint8_t *)image->data +
        (size_t)y * image->bytes_per_line +
        (size_t)x * 4;

    return (uint32_t)p[0] |
           ((uint32_t)p[1] << 8) |
           ((uint32_t)p[2] << 16);
}

int main(int argc, char **argv)
{
    if (argc != 2) {
        fprintf(stderr, "Usage: %s /path/to/libqwlss-test.so\n",
                argv[0]);
        return 2;
    }

    void *library = dlopen(argv[1], RTLD_NOW | RTLD_LOCAL);

    if (!library) {
        fprintf(stderr, "dlopen: %s\n", dlerror());
        return 1;
    }

    get_image_fn get_image =
        (get_image_fn)dlsym(library, "XShmGetImage");

    if (!get_image) {
        fprintf(stderr, "dlsym: %s\n", dlerror());
        return 1;
    }

    /*
     * 640×360 应缩放为 320×180，
     * 放在 320×240 画布中，上下各 30 行黑边。
     *
     * 特意加 16 字节行尾空间，检查行跨度处理。
     */
    enum {
        WIDTH = 320,
        HEIGHT = 240,
        STRIDE = WIDTH * 4 + 16
    };

    XImage image = {0};

    image.width = WIDTH;
    image.height = HEIGHT;
    image.format = ZPixmap;
    image.depth = 24;
    image.bits_per_pixel = 32;
    image.bytes_per_line = STRIDE;
    image.byte_order = LSBFirst;
    image.red_mask = 0x00ff0000;
    image.green_mask = 0x0000ff00;
    image.blue_mask = 0x000000ff;

    image.data = calloc(HEIGHT, STRIDE);

    if (!image.data) {
        perror("calloc");
        return 1;
    }

    /*
     * 当前 hook 不访问 display/drawable，
     * 所以这个检查不需要连接 X Server。
     */
    int ok = get_image(NULL, 0, &image, 0, 0, ~0UL);

    if (ok) {
        ok =
            read_pixel(&image, 10, 0)   == 0x000000 &&
            read_pixel(&image, 10, 29)  == 0x000000 &&
            read_pixel(&image, 10, 30)  == 0xff0000 &&
            read_pixel(&image, 50, 100) == 0x00ff00 &&
            read_pixel(&image, 90, 100) == 0x0000ff &&
            read_pixel(&image, 130, 100) == 0xffffff &&
            read_pixel(&image, 10, 209) == 0xff0000 &&
            read_pixel(&image, 10, 210) == 0x000000;
    }

    if (ok) {
        /* 只保留红色位平面。 */
        ok = get_image(NULL, 0, &image, 0, 0, 0x00ff0000UL);

        if (ok) {
            ok =
                read_pixel(&image, 10, 100) == 0xff0000 &&
                read_pixel(&image, 50, 100) == 0x000000 &&
                read_pixel(&image, 90, 100) == 0x000000;
        }
    }

    puts(ok ? "PASS: hook rendering" : "FAIL: hook rendering");

    free(image.data);

    /*
     * 测试进程马上退出，不在这里卸载库。
     * 当前测试图缓存随进程退出回收。
     */
    return ok ? 0 : 1;
}
