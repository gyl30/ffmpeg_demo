
add_executable(aac_pcm main.cpp )

target_link_libraries(aac_pcm PkgConfig::LIBAV)
