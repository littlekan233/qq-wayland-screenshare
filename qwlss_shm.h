#ifndef QWLSS_SHM_H
#define QWLSS_SHM_H

#include <stddef.h>
#include <stdint.h>

/* 这是读端拥有的本地快照，不是共享内存里的结构。 */
struct qwlss_snapshot {
    uint8_t *pixels;
    int width;
    int height;
    size_t stride;
    uint64_t sequence;
};

/* 成功返回 0，失败返回 -1 并设置 errno。 */
int qwlss_shm_publish(
    const void *src,
    size_t available,
    int width,
    int height,
    size_t stride
);

/*
 * 1：取得完整帧，调用方负责 free(out->pixels)。
 * 0：尚无共享对象，或当前没有有效帧。
 * -1：错误，检查 errno。
 *
 * out 必须是空的输出对象，不能传入尚未释放的旧快照。
 */
int qwlss_shm_read(struct qwlss_snapshot *out);

/* 标记当前帧无效，不删除共享对象。 */
int qwlss_shm_clear(void);

/* 仅在所有使用者结束后调用。 */
int qwlss_shm_remove(void);

/*
 * 返回本进程初始化时确定的共享名称。
 * 返回值由库持有，不要修改或释放。
 * 初始化失败时返回 NULL，并设置 errno。
 */
const char *qwlss_shm_name(void);
#endif
