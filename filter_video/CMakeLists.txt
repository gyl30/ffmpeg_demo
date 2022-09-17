
add_executable(filter_video main.cpp )

target_link_libraries(filter_video PkgConfig::LIBAV)
