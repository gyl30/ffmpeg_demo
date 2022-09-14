
add_executable(h264_filter main.cpp )

target_link_libraries(h264_filter PkgConfig::LIBAV)
