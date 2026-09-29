#ifndef WATCHER_H
#define WATCHER_H

/* 异步请求开始捕获，重复调用不会重复弹出 Portal。 */
void do_screencast(void);

/* 异步请求停止，包括仍在等待用户选择的情况。 */
void stop_screencast(void);

#endif
