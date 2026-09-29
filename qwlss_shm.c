#define _GNU_SOURCE
#include "qwlss_shm.h"

#include <errno.h>
#include <fcntl.h>
#include <inttypes.h>
#include <limits.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/file.h>
#include <sys/mman.h>
#include <sys/random.h>
#include <sys/stat.h>
#include <unistd.h>

#define FRAME_MAGIC UINT32_C(0x51574631)
#define FRAME_VERSION 1u

/* 单帧上限，不是预分配大小。 */
#define MAX_FRAME_BYTES ((size_t)256 * 1024 * 1024)

struct disk_header {
    uint32_t magic;
    uint32_t version;
    uint32_t valid;
    uint32_t width;
    uint32_t height;
    uint32_t stride;

    uint64_t bytes;
    uint64_t sequence;
};

#define QWLSS_NAME_ENV "QWLSS_SHM_NAME"

static char ipc_name[201];
static int ipc_init_error = EAGAIN;

static int valid_ipc_name(const char *name)
{
    if (!name || name[0] != '/' || name[1] == '\0')
        return 0;

    size_t length = strnlen(name, sizeof(ipc_name));

    if (length >= sizeof(ipc_name))
        return 0;

    for (size_t i = 1; i < length; ++i) {
        unsigned char c = (unsigned char)name[i];

        if (!((c >= 'a' && c <= 'z') ||
              (c >= 'A' && c <= 'Z') ||
              (c >= '0' && c <= '9') ||
              c == '-' || c == '_' || c == '.'))
            return 0;
    }

    return 1;
}

static int random_bytes(void *buffer, size_t size)
{
    unsigned char *p = buffer;

    while (size != 0) {
        ssize_t n = getrandom(p, size, 0);

        if (n < 0) {
            if (errno == EINTR)
                continue;
            return -1;
        }

        if (n == 0) {
            errno = EIO;
            return -1;
        }

        p += n;
        size -= (size_t)n;
    }

    return 0;
}

static int generate_ipc_name(void)
{
    unsigned char random_value[16];

    if (random_bytes(random_value, sizeof(random_value)) < 0)
        return -1;

    int n = snprintf(
        ipc_name,
        sizeof(ipc_name),
        "/qwlss-%lu-",
        (unsigned long)geteuid()
    );

    if (n < 0 || (size_t)n >= sizeof(ipc_name)) {
        errno = ENAMETOOLONG;
        return -1;
    }

    size_t used = (size_t)n;

    /*
     * 每次追加一个字节的两位十六进制编码。
     * 每次 snprintf 都会写入结尾的 NUL。
     */
    for (size_t i = 0; i < sizeof(random_value); ++i) {
        n = snprintf(
            ipc_name + used,
            sizeof(ipc_name) - used,
            "%02x",
            (unsigned)random_value[i]
        );

        if (n != 2 || (size_t)n >= sizeof(ipc_name) - used) {
            errno = ENAMETOOLONG;
            return -1;
        }

        used += (size_t)n;
    }

    return 0;
}

/*
 * 面向启动时 LD_PRELOAD 的用法。
 *
 * 优先于同一库中默认优先级的构造函数执行，
 * 在窗口监听、捕获线程启动前准备好名称。
 */
__attribute__((constructor(101)))
static void qwlss_initialize_ipc_name(void)
{
    const char *existing = getenv(QWLSS_NAME_ENV);
    const char *mode = "inherited/configured";

    if (existing && existing[0] != '\0') {
        /*
         * 子进程继承的名称，或开发者显式指定的名称。
         * 不重新生成。
         */
        if (!valid_ipc_name(existing)) {
            ipc_init_error = EINVAL;
            goto failed;
        }

        memcpy(ipc_name, existing, strlen(existing) + 1);
    } else {
        unsigned char random_value[8];
        char hex[33];

        static const char digits[] = "0123456789abcdef";

	if (generate_ipc_name() < 0) {
            ipc_init_error = errno;
            goto failed;
        }

        if (setenv(QWLSS_NAME_ENV, ipc_name, 1) < 0) {
            ipc_init_error = errno;
            goto failed;
        }

        mode = "generated";
    }

    ipc_init_error = 0;

    fprintf(stderr,
            "[qwlss-ipc pid=%ld] %s name=%s\n",
            (long)getpid(), mode, ipc_name);
    return;

failed:
    ipc_name[0] = '\0';

    fprintf(stderr,
            "[qwlss-ipc pid=%ld] initialization failed: %s\n",
            (long)getpid(), strerror(ipc_init_error));
}

const char *qwlss_shm_name(void)
{
    if (ipc_init_error != 0) {
        errno = ipc_init_error;
        return NULL;
    }

    return ipc_name;
}

/* 保持原有内部调用不变。 */
static const char *shared_name(void)
{
    return qwlss_shm_name();
}

/*
 * 每次操作使用新的 open file description。
 * 不缓存 FD，避免 fork 后意外共用同一把 flock。
 */
static int open_locked(int writer, int create)
{
    const char *name = shared_name();
    if (!name)
        return -1;

    int flags = writer ? O_RDWR : O_RDONLY;
    if (create)
        flags |= O_CREAT;

    int fd = shm_open(name, flags, 0600);
    if (fd < 0)
        return -1;

    int op = writer ? LOCK_EX : LOCK_SH;

    while (flock(fd, op) < 0) {
        if (errno == EINTR)
            continue;

        int saved = errno;
        close(fd);
        errno = saved;
        return -1;
    }

    return fd;
}

static int finish(int fd, int result)
{
    int saved = errno;

    (void)flock(fd, LOCK_UN);
    (void)close(fd);

    errno = saved;
    return result;
}

static int read_at(int fd, void *buffer, size_t count, off_t offset)
{
    uint8_t *p = buffer;

    while (count) {
        ssize_t n = pread(fd, p, count, offset);

        if (n < 0 && errno == EINTR)
            continue;

        if (n <= 0) {
            if (n == 0)
                errno = EPROTO;
            return -1;
        }

        p += n;
        count -= (size_t)n;
        offset += n;
    }

    return 0;
}

static int write_at(
    int fd, const void *buffer, size_t count, off_t offset)
{
    const uint8_t *p = buffer;

    while (count) {
        ssize_t n = pwrite(fd, p, count, offset);

        if (n < 0 && errno == EINTR)
            continue;

        if (n <= 0) {
            if (n == 0)
                errno = EIO;
            return -1;
        }

        p += n;
        count -= (size_t)n;
        offset += n;
    }

    return 0;
}

static int set_valid(int fd, uint32_t valid)
{
    return write_at(
        fd, &valid, sizeof(valid),
        (off_t)offsetof(struct disk_header, valid));
}

/*
 * 调用者已持有文件锁。
 * 1：有效帧；0：空帧；-1：损坏或不兼容。
 */
static int load_header(int fd, struct disk_header *h)
{
    struct stat st;

    if (fstat(fd, &st) < 0)
        return -1;

    if (st.st_size < (off_t)sizeof(*h))
        return 0;

    if (read_at(fd, h, sizeof(*h), 0) < 0)
        return -1;

    /* 写入中退出的生产者会留下 valid == 0。 */
    if (h->valid == 0)
        return 0;

    if (h->valid != 1 ||
        h->magic != FRAME_MAGIC ||
        h->version != FRAME_VERSION ||
        h->width == 0 || h->height == 0 ||
        h->width > INT_MAX || h->height > INT_MAX ||
        h->bytes > MAX_FRAME_BYTES ||
        (uint64_t)h->stride != (uint64_t)h->width * 4 ||
        h->bytes != (uint64_t)h->stride * h->height ||
        (uint64_t)st.st_size < sizeof(*h) + h->bytes) {
        errno = EPROTO;
        return -1;
    }

    return 1;
}

int qwlss_shm_publish(
    const void *src,
    size_t available,
    int width,
    int height,
    size_t stride)
{
    if (!src || width <= 0 || height <= 0) {
        errno = EINVAL;
        return -1;
    }

    if ((size_t)width > MAX_FRAME_BYTES / 4) {
        errno = EFBIG;
        return -1;
    }

    size_t row = (size_t)width * 4;

    if ((size_t)height > MAX_FRAME_BYTES / row) {
        errno = EFBIG;
        return -1;
    }

    if (stride < row ||
        (size_t)(height - 1) > (SIZE_MAX - row) / stride ||
        available < (size_t)(height - 1) * stride + row) {
        errno = EINVAL;
        return -1;
    }

    size_t bytes = row * (size_t)height;
    size_t total = sizeof(struct disk_header) + bytes;

    int fd = open_locked(1, 1);
    if (fd < 0)
        return -1;

    struct disk_header previous = {0};
    int old_status = load_header(fd, &previous);

    if (old_status < 0)
        return finish(fd, -1);

    /*
     * 必须先单独清除 valid，再更新其余字段。
     * 即使进程在后续步骤退出，也不会暴露半帧。
     */
    if (set_valid(fd, 0) < 0)
        return finish(fd, -1);

    struct disk_header h = {
        .magic = FRAME_MAGIC,
        .version = FRAME_VERSION,
        .valid = 0,
        .width = (uint32_t)width,
        .height = (uint32_t)height,
        .stride = (uint32_t)row,
        .bytes = bytes,
        .sequence = old_status == 1
            ? previous.sequence + 1 : 1
    };

    if (write_at(fd, &h, sizeof(h), 0) < 0)
        return finish(fd, -1);

    struct stat st;
    if (fstat(fd, &st) < 0)
        return finish(fd, -1);

    /*
     * 只增长，不因分辨率变小而缩小对象。
     * 实际有效数据长度由 header 决定。
     */
    if (st.st_size < (off_t)total) {
        int err;
        do {
            err = posix_fallocate(fd, 0, (off_t)total);
        } while (err == EINTR);

        if (err != 0) {
            errno = err;
            return finish(fd, -1);
        }
    }

    uint8_t *mapping = mmap(
        NULL, total, PROT_READ | PROT_WRITE,
        MAP_SHARED, fd, 0);

    if (mapping == MAP_FAILED)
        return finish(fd, -1);

    uint8_t *dst = mapping + sizeof(h);

    for (int y = 0; y < height; ++y) {
        memcpy(dst + (size_t)y * row,
               (const uint8_t *)src + (size_t)y * stride,
               row);
    }

    if (munmap(mapping, total) < 0)
        return finish(fd, -1);

    /* 像素复制完整后提交。 */
    if (set_valid(fd, 1) < 0)
        return finish(fd, -1);

    return finish(fd, 0);
}

int qwlss_shm_read(struct qwlss_snapshot *out)
{
    if (!out) {
        errno = EINVAL;
        return -1;
    }

    *out = (struct qwlss_snapshot){0};

    int fd = open_locked(0, 0);

    if (fd < 0)
        return errno == ENOENT ? 0 : -1;

    struct disk_header h = {0};
    int status = load_header(fd, &h);

    if (status != 1)
        return finish(fd, status);

    size_t bytes = (size_t)h.bytes;
    size_t total = sizeof(h) + bytes;

    uint8_t *copy = malloc(bytes);
    if (!copy)
        return finish(fd, -1);

    const uint8_t *mapping = mmap(
        NULL, total, PROT_READ, MAP_SHARED, fd, 0);

    if (mapping == MAP_FAILED) {
        int saved = errno;
        free(copy);
        errno = saved;
        return finish(fd, -1);
    }

    memcpy(copy, mapping + sizeof(h), bytes);
    (void)munmap((void *)mapping, total);

    *out = (struct qwlss_snapshot) {
        .pixels = copy,
        .width = (int)h.width,
        .height = (int)h.height,
        .stride = h.stride,
        .sequence = h.sequence
    };

    return finish(fd, 1);
}

int qwlss_shm_clear(void)
{
    int fd = open_locked(1, 0);

    if (fd < 0)
        return errno == ENOENT ? 0 : -1;

    return finish(fd, set_valid(fd, 0));
}

int qwlss_shm_remove(void)
{
    const char *name = shared_name();
    if (!name)
        return -1;

    if (shm_unlink(name) < 0 && errno != ENOENT)
        return -1;

    return 0;
}

