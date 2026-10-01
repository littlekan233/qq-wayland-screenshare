#ifndef QWLSS_RUNTIME_H
#define QWLSS_RUNTIME_H

int qwlss_is_main_process(void);
int qwlss_uses_wayland(void);
const char *qwlss_executable(void);
void qwlss_set_capture_flag(const char *path);
void qwlss_capture_ready(int ready);

#endif
