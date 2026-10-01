cmake_minimum_required(VERSION 3.16)

project(qwlss LANGUAGES C)

find_package(PkgConfig REQUIRED)

# 查找依赖，统一生成 PkgConfig::QWLSS_DEPS 目标。
pkg_check_modules(QWLSS_DEPS REQUIRED IMPORTED_TARGET
    glib-2.0
    gobject-2.0
    gio-2.0
    xcb
    xcb-randr
    x11
    xext
    libportal
    libpipewire-0.3
)

set(QWLSS_SOURCES
    hook.c
    watcher.c
    capture_adapter.c
    x11_window.c
    portal_capture.c
    capture_controller.c
    qwlss_shm.c
    mesa_workaround.c
)

# 由 wlsanitizer.js 生成内嵌脚本头文件，测试目录也会复用。
file(READ "${CMAKE_CURRENT_SOURCE_DIR}/wlsanitizer.js" QWLSS_SANITIZER_JS)
file(WRITE "${CMAKE_CURRENT_BINARY_DIR}/wlsanitizer_js.h"
"/* Generated from wlsanitizer.js; do not edit. */
#ifndef QWLSS_WLSANITIZER_JS_H
#define QWLSS_WLSANITIZER_JS_H

static const unsigned char wlsanitizer_js[] = {
")
string(REGEX REPLACE "(.)" "\\1," QWLSS_SANITIZER_BYTES "${QWLSS_SANITIZER_JS}")
file(APPEND "${CMAKE_CURRENT_BINARY_DIR}/wlsanitizer_js.h"
"    ${QWLSS_SANITIZER_BYTES}0
};

#endif
")

# 稳定版：原有 X11/Portal 采集链路。
add_library(qwlss SHARED
    ${QWLSS_SOURCES}
    runtime_stub.c
)
set_target_properties(qwlss PROPERTIES OUTPUT_NAME qwlss)

# 实验版：追加基于 LD_PRELOAD 的 Wayland 入口与共享边框处理。
add_library(qwlss_exp_wlsanitizer SHARED
    ${QWLSS_SOURCES}
    activity.c
    runtime.c
    wlsanitizer.c
)
set_target_properties(qwlss_exp_wlsanitizer PROPERTIES
    OUTPUT_NAME qwlss-exp-wlsanitizer
)

find_package(Threads REQUIRED)

foreach(target IN ITEMS qwlss qwlss_exp_wlsanitizer)
    target_link_libraries(${target} PRIVATE
        Threads::Threads
        rt
        ${CMAKE_DL_LIBS}
        PkgConfig::QWLSS_DEPS
    )

    target_link_options(${target} PRIVATE
        "-Wl,-z,defs"
    )

    # 这里选择 GNU C11；如果项目有其他标准要求，可以调整。
    set_target_properties(${target} PROPERTIES
        C_STANDARD 11
        C_STANDARD_REQUIRED YES
        C_EXTENSIONS YES
    )

    target_include_directories(${target} PRIVATE
        "${CMAKE_CURRENT_SOURCE_DIR}"
        "${CMAKE_CURRENT_BINARY_DIR}"
    )
endforeach()

# 测试默认独立构建（tests/CMakeLists.txt）；如需随主工程一起构建：
#   cmake -DQWLSS_BUILD_TESTS=ON ..
option(QWLSS_BUILD_TESTS "Build deterministic test executables" OFF)

if(QWLSS_BUILD_TESTS)
    enable_testing()
    add_subdirectory(tests)
endif()
