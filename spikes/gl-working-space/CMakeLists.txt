cmake_minimum_required(VERSION 3.21)
project(gl_working_space LANGUAGES C)

set(CMAKE_C_STANDARD 11)
set(CMAKE_C_STANDARD_REQUIRED ON)
set(CMAKE_C_EXTENSIONS ON)

find_package(PkgConfig REQUIRED)
pkg_check_modules(GST REQUIRED IMPORTED_TARGET
    gstreamer-1.0
    gstreamer-app-1.0
    gstreamer-base-1.0
    gstreamer-video-1.0
    gstreamer-gl-1.0
    gstreamer-gl-egl-1.0)

add_executable(gl-working-space
    main.c
    glscopes.c)
target_link_libraries(gl-working-space PRIVATE PkgConfig::GST)
