cmake_minimum_required(VERSION 3.23)

project(wemeet-wayland-screencast LANGUAGES C CXX)
set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

# Find PkgConfig
find_package(PkgConfig REQUIRED)

# Find the gio-2.0 package
pkg_check_modules(GIO REQUIRED gio-2.0)
# libportal
pkg_check_modules(LIBPORTAL REQUIRED libportal)
# pipewire
pkg_check_modules(PIPEWIRE REQUIRED libpipewire-0.3)


find_package(X11 REQUIRED)

find_package(OpenCV QUIET)
if(NOT OpenCV_FOUND)
  set(OpenCV_NOTFOUND_ERROR_MESSAGE "CMake did not find a package provided by \"OpenCV\". Please install develepment package for OpenCV. Split packages may suffice if you REALLY know what that means. Otherwise meta package is recommended.")
  
  # We only need libopencv_core.so, libopencv_imgproc.so and relevant headers
  find_library(LIB_OPENCV_CORE NAMES opencv_core)
  if(NOT LIB_OPENCV_CORE)
    message(FATAL_ERROR "${OpenCV_NOTFOUND_ERROR_MESSAGE}")
  endif(NOT LIB_OPENCV_CORE)
  find_library(LIB_OPENCV_IMGPROC NAMES opencv_imgproc)
  if(NOT LIB_OPENCV_IMGPROC)
    message(FATAL_ERROR "${OpenCV_NOTFOUND_ERROR_MESSAGE}")
  endif(NOT LIB_OPENCV_IMGPROC)
  find_path(OpenCV_INCLUDE_DIRS NAMES opencv2/core/core_c.h PATH_SUFFIXES opencv4)
  if(NOT OpenCV_INCLUDE_DIRS)
    message(FATAL_ERROR "${OpenCV_NOTFOUND_ERROR_MESSAGE}")
  endif(NOT OpenCV_INCLUDE_DIRS)
  
  message(STATUS "CMake did not find a package provided by \"OpenCV\"")
  message(STATUS "Detect OpenCV_INCLUDE_DIRS = ${OpenCV_INCLUDE_DIRS}")
endif(NOT OpenCV_FOUND)


function(add_compile_options_for TARGET_NAME)


  target_link_libraries(${TARGET_NAME} PRIVATE ${GIO_LIBRARIES})
  target_include_directories(${TARGET_NAME} PRIVATE ${GIO_INCLUDE_DIRS})

  target_link_libraries(${TARGET_NAME} PRIVATE ${LIBPORTAL_LIBRARIES})
  target_include_directories(${TARGET_NAME} PRIVATE ${LIBPORTAL_INCLUDE_DIRS})

  target_link_libraries(${TARGET_NAME} PRIVATE ${PIPEWIRE_LIBRARIES})
  target_include_directories(${TARGET_NAME} PRIVATE ${PIPEWIRE_INCLUDE_DIRS})
  target_compile_options(${TARGET_NAME} PRIVATE ${PIPEWIRE_CFLAGS_OTHER})

  # include current directory
  target_include_directories(${TARGET_NAME} PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})

  # opencv
  target_include_directories(${TARGET_NAME} PRIVATE ${OpenCV_INCLUDE_DIRS})

  target_link_libraries(${TARGET_NAME} PRIVATE X11::X11)
  target_link_libraries(${TARGET_NAME} PRIVATE X11::Xrandr)

endfunction()

# wayland screencast hook

add_library(hook SHARED hook.cpp payload.cpp)
set_property(TARGET hook PROPERTY POSITION_INDEPENDENT_CODE ON)
add_compile_options_for(hook)
install(TARGETS hook DESTINATION /opt/QQ)
