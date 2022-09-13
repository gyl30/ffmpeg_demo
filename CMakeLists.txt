cmake_minimum_required(VERSION 3.21)

project(ffmpeg_demo)

set(CMAKE_CXX_STANDARD 20)

set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

find_package(PkgConfig REQUIRED)

pkg_check_modules(LIBAV REQUIRED IMPORTED_TARGET
    libavfilter
    libavformat
    libavcodec
    libavutil
    )

add_subdirectory(h264_to_yuv)


