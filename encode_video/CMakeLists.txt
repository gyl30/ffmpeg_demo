
add_executable(encode_video main.cpp )

target_link_libraries(encode_video PkgConfig::LIBAV)
