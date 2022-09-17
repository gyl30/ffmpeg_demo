
include_directories(${CMAKE_CURRENT_SOURCE_DIR}/ps_parse/include)

link_directories(${CMAKE_CURRENT_SOURCE_DIR}/ps_parse/lib)

add_executable(rtp_ps_h264 main.cpp ps.cpp rtp.cpp)

target_link_libraries(rtp_ps_h264 PkgConfig::LIBAV ps_parse)
