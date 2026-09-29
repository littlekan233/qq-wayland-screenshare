cmake_minimum_required(VERSION 3.16)

project(qwlss_tests LANGUAGES C)

set(CMAKE_C_STANDARD 11)
set(CMAKE_C_STANDARD_REQUIRED ON)
set(CMAKE_C_EXTENSIONS ON)

# 默认认为测试目录位于项目根目录的下一层。
# 如果实际位置不同，可以通过 -DQWLSS_SOURCE_DIR=... 指定。
set(
    QWLSS_SOURCE_DIR
    "${CMAKE_CURRENT_SOURCE_DIR}/.."
    CACHE PATH
    "Directory containing hook.c and qwlss_shm.c"
)

get_filename_component(
    QWLSS_SOURCE_DIR
    "${QWLSS_SOURCE_DIR}"
    ABSOLUTE
)

foreach(required_file IN ITEMS
    hook.c
    watcher.h
    qwlss_shm.c
    qwlss_shm.h
)
    if(NOT EXISTS "${QWLSS_SOURCE_DIR}/${required_file}")
        message(FATAL_ERROR
            "Missing ${QWLSS_SOURCE_DIR}/${required_file}\n"
            "Set QWLSS_SOURCE_DIR to the project source directory."
        )
    endif()
endforeach()

find_package(PkgConfig REQUIRED)
find_package(Threads REQUIRED)

pkg_check_modules(
    X11_DEPS REQUIRED IMPORTED_TARGET
    x11
    xext
)

# 所有可执行程序放在 bin/，测试共享库放在 lib/。
set(CMAKE_RUNTIME_OUTPUT_DIRECTORY "${CMAKE_CURRENT_BINARY_DIR}/bin")
set(CMAKE_LIBRARY_OUTPUT_DIRECTORY "${CMAKE_CURRENT_BINARY_DIR}/lib")
set(CMAKE_ARCHIVE_OUTPUT_DIRECTORY "${CMAKE_CURRENT_BINARY_DIR}/lib")

# ------------------------------------------------------------
# 共享内存实现
# 编译为 PIC 静态库，供测试程序和测试 .so 复用。
# ------------------------------------------------------------
add_library(qwlss_test_shm STATIC
    "${QWLSS_SOURCE_DIR}/qwlss_shm.c"
)

set_target_properties(qwlss_test_shm PROPERTIES
    POSITION_INDEPENDENT_CODE ON
)

target_include_directories(qwlss_test_shm PUBLIC
    "${QWLSS_SOURCE_DIR}"
)

target_link_libraries(qwlss_test_shm PUBLIC
    rt
)

# ------------------------------------------------------------
# 跨进程共享内存测试工具
# ------------------------------------------------------------
add_executable(shm_test
    "${CMAKE_CURRENT_SOURCE_DIR}/shm_test.c"
)

target_link_libraries(shm_test PRIVATE
    qwlss_test_shm
)

# ------------------------------------------------------------
# 本地 hook 检查程序：dlopen 指定的测试 .so
# ------------------------------------------------------------
add_executable(hook_checker
    "${CMAKE_CURRENT_SOURCE_DIR}/hook_checker.c"
)

target_link_libraries(hook_checker PRIVATE
    PkgConfig::X11_DEPS
    ${CMAKE_DL_LIBS}
)

# ------------------------------------------------------------
# 色条测试库
# test_pattern.c 自带构造函数和 watchdog 空实现。
# 输出：libqwlss-test.so
# ------------------------------------------------------------
add_library(qwlss_pattern_test SHARED
    "${QWLSS_SOURCE_DIR}/hook.c"
    "${CMAKE_CURRENT_SOURCE_DIR}/test_pattern.c"
)

set_target_properties(qwlss_pattern_test PROPERTIES
    OUTPUT_NAME "qwlss-test"
)

target_link_libraries(qwlss_pattern_test PRIVATE
    qwlss_test_shm
    PkgConfig::X11_DEPS
    Threads::Threads
)

target_link_options(qwlss_pattern_test PRIVATE
    "LINKER:-z,defs"
)

# ------------------------------------------------------------
# 纯共享内存读取测试库
# 不生成色条，不启动 Portal/PipeWire。
# 自动在构建目录生成 watchdog 空实现。
# 输出：libqwlss-shm-test.so
# ------------------------------------------------------------
set(
    WATCHDOG_STUB
    "${CMAKE_CURRENT_BINARY_DIR}/generated/watchdog_stub.c"
)

file(MAKE_DIRECTORY
    "${CMAKE_CURRENT_BINARY_DIR}/generated"
)

file(WRITE "${WATCHDOG_STUB}"
    "void watchdog_timer_reset(void)\n"
    "{\n"
    "}\n"
)

add_library(qwlss_shm_test SHARED
    "${QWLSS_SOURCE_DIR}/hook.c"
    "${WATCHDOG_STUB}"
)

set_target_properties(qwlss_shm_test PROPERTIES
    OUTPUT_NAME "qwlss-shm-test"
)

target_link_libraries(qwlss_shm_test PRIVATE
    qwlss_test_shm
    PkgConfig::X11_DEPS
    Threads::Threads
)

target_link_options(qwlss_shm_test PRIVATE
    "LINKER:-z,defs"
)

# 构建 hook_checker 时，一并构建它常用的色条测试库。
add_dependencies(hook_checker qwlss_pattern_test)

if(CMAKE_C_COMPILER_ID MATCHES "GNU|Clang")
    foreach(test_target IN ITEMS
        qwlss_test_shm
        shm_test
        hook_checker
        qwlss_pattern_test
        qwlss_shm_test
    )
        target_compile_options(${test_target} PRIVATE
            -Wall
            -Wextra
            -Werror=implicit-function-declaration
        )
    endforeach()
endif()

# 构建 watchdog_test
add_executable(watchdog_test
    "${CMAKE_CURRENT_SOURCE_DIR}/watchdog_test.c"
)

target_link_libraries(watchdog_test PRIVATE
    qwlss_test_shm
)
