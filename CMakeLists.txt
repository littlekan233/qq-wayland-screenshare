cmake_minimum_required(VERSION 3.16)

project(qwlss LANGUAGES C)

find_package(PkgConfig REQUIRED)

# 查找依赖，统一生成 PkgConfig::QWLSS_DEPS 目标。
pkg_check_modules(QWLSS_DEPS REQUIRED IMPORTED_TARGET
    glib-2.0
    gobject-2.0
    gio-2.0
    xcb
    x11
    xext
    libportal
    libpipewire-0.3
)

add_library(qwlss SHARED
    hook.c
    watcher.c
    capture_adapter.c
    x11_window.c
    portal_capture.c
    capture_controller.c
)

# 这里选择 GNU C11；如果项目有其他标准要求，可以调整。
set_target_properties(qwlss PROPERTIES
    C_STANDARD 11
    C_STANDARD_REQUIRED YES
    C_EXTENSIONS YES
)

target_include_directories(qwlss PRIVATE
    "${CMAKE_CURRENT_SOURCE_DIR}"
)

target_link_libraries(qwlss PRIVATE
    PkgConfig::QWLSS_DEPS
)
