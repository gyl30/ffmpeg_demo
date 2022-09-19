cmake_minimum_required(VERSION 3.21)

project(ffmpeg_demo)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

#set(CMAKE_VERBOSE_MAKEFILE ON)

set(CMAKE_BUILD_TYPE Debug)
set(CMAKE_EXPORT_COMPILE_COMMANDS "ON")

set(CMAKE_C_FLAGS "${CMAKE_C_FLAGS} -fno-omit-frame-pointer -g -O0 -ggdb")
set(CMAKE_CXX_FLAGS "${CMAKE_CXX_FLAGS} -fno-omit-frame-pointer -g -O0 -ggdb")

find_package(PkgConfig REQUIRED)

pkg_check_modules(LIBAV REQUIRED IMPORTED_TARGET
    libavfilter
    libavformat
    libavcodec
    libavutil
    )

add_subdirectory(h264_to_yuv)

add_subdirectory(h264_filter)

add_subdirectory(aac_pcm)

add_subdirectory(decode_video)

add_subdirectory(rtp_ps_h264)

add_subdirectory(encode_video)

add_subdirectory(filter_video)

add_subdirectory(rtp_filter)
