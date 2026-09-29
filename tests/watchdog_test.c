#include "qwlss_shm.h"

#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <sys/wait.h>
#include <unistd.h>

#define CHECK(expr)                                              \
    do {                                                         \
        if (!(expr)) {                                           \
            fprintf(stderr, "FAIL line %d: %s\n", __LINE__, #expr);\
            exit(1);                                             \
        }                                                        \
    } while (0)

int main(void)
{
    uint64_t first = 0;
    uint64_t second = 0;
    uint64_t ticks = 0;

    /* 必须使用专门用于测试的 QWLSS_SHM_NAME。 */
    CHECK(qwlss_wd_remove() == 0);

    CHECK(qwlss_wd_begin(&first) == 1);

    /* 选择期间不接受心跳。 */
    CHECK(qwlss_wd_beat() == 0);
    CHECK(qwlss_wd_arm(first) == 1);

    pid_t child = fork();
    CHECK(child >= 0);

    if (child == 0) {
        int ok =
            qwlss_wd_beat() == 1 &&
            qwlss_wd_beat() == 1;

        _exit(ok ? 0 : 1);
    }

    int status = 0;
    CHECK(waitpid(child, &status, 0) == child);
    CHECK(WIFEXITED(status) && WEXITSTATUS(status) == 0);

    /* 父进程必须看到子进程的心跳。 */
    CHECK(qwlss_wd_poll(first, &ticks) == 1);
    CHECK(ticks == 2);

    /* 用过期序号关闭，必须失败。 */
    CHECK(qwlss_wd_expire(first, 1) == 0);
    CHECK(qwlss_wd_expire(first, 2) == 1);
    CHECK(qwlss_wd_beat() == 0);

    CHECK(qwlss_wd_begin(&second) == 1);
    CHECK(second != first);
    CHECK(qwlss_wd_arm(second) == 1);

    /* 上一次会话不能结束新会话。 */
    CHECK(qwlss_wd_end(first) == 0);
    CHECK(qwlss_wd_poll(second, &ticks) == 1);
    CHECK(ticks == 0);

    CHECK(qwlss_wd_end(second) == 1);
    CHECK(qwlss_wd_remove() == 0);

    puts("PASS: cross-process watchdog state");
    return 0;
}
