#include "qwlss_shm.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

static const unsigned char colors[8][3] = {
    {255, 0, 0},     {0, 255, 0},
    {0, 0, 255},     {255, 255, 255},
    {255, 255, 0},   {0, 255, 255},
    {255, 0, 255},   {0, 0, 0}
};

int main(int argc, char **argv)
{
    if (argc != 2) {
        fprintf(stderr,
                "Usage: %s publish|small|read|empty|clear|unlink\n",
                argv[0]);
        return 2;
    }

    const char *action = argv[1];

    if (!strcmp(action, "publish") || !strcmp(action, "small")) {
        int small = !strcmp(action, "small");
        int width = small ? 320 : 640;
        int height = small ? 240 : 360;

        /* 特意加入行尾 padding，验证输入 stride。 */
        size_t stride = (size_t)width * 4 + 16;
        size_t bytes = stride * height;

        unsigned char *pixels = malloc(bytes);
        if (!pixels) {
            perror("malloc");
            return 1;
        }

        memset(pixels, 0xee, bytes);

        for (int y = 0; y < height; ++y) {
            for (int x = 0; x < width; ++x) {
                int bar = x * 8 / width;
                unsigned char *p =
                    pixels + (size_t)y * stride + (size_t)x * 4;

                p[0] = colors[bar][2];
                p[1] = colors[bar][1];
                p[2] = colors[bar][0];
                p[3] = 0;
            }
        }

        int result = qwlss_shm_publish(
            pixels, bytes, width, height, stride);

        free(pixels);

        if (result < 0) {
            perror("publish");
            return 1;
        }

        printf("publisher pid=%ld: %dx%d\n",
               (long)getpid(), width, height);
        return 0;
    }

    if (!strcmp(action, "read") || !strcmp(action, "empty")) {
        struct qwlss_snapshot frame = {0};
        int result = qwlss_shm_read(&frame);

        if (result < 0) {
            perror("read");
            return 1;
        }

        if (!strcmp(action, "empty")) {
            free(frame.pixels);
            puts(result == 0 ? "PASS: empty" : "FAIL: not empty");
            return result == 0 ? 0 : 1;
        }

        if (result != 1) {
            fprintf(stderr, "FAIL: no frame\n");
            return 1;
        }

        int ok =
            ((frame.width == 640 && frame.height == 360) ||
             (frame.width == 320 && frame.height == 240)) &&
            frame.stride == (size_t)frame.width * 4;

        for (int y = 0; ok && y < frame.height; ++y) {
            for (int x = 0; x < frame.width; ++x) {
                int bar = x * 8 / frame.width;
                const unsigned char *p =
                    frame.pixels + (size_t)y * frame.stride +
                    (size_t)x * 4;

                if (p[0] != colors[bar][2] ||
                    p[1] != colors[bar][1] ||
                    p[2] != colors[bar][0] || p[3] != 0) {
                    ok = 0;
                    break;
                }
            }
        }

        printf("%s: reader pid=%ld, %dx%d\n",
               ok ? "PASS" : "FAIL",
               (long)getpid(), frame.width, frame.height);

        free(frame.pixels);
        return ok ? 0 : 1;
    }

    int result;

    if (!strcmp(action, "clear"))
        result = qwlss_shm_clear();
    else if (!strcmp(action, "unlink"))
        result = qwlss_shm_remove();
    else
        return 2;

    if (result < 0)
        perror(action);

    return result < 0 ? 1 : 0;
}
