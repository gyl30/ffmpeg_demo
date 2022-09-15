
add_executable(decode_video main.cpp )

target_link_libraries(decode_video PkgConfig::LIBAV)
