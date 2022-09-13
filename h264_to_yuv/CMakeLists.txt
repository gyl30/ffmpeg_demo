
add_executable(h264_yuv main.cpp )

target_link_libraries(h264_yuv PkgConfig::LIBAV)
