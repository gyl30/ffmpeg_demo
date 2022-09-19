include_directories(${CMAKE_CURRENT_SOURCE_DIR}/ps_parse/include)

link_directories(${CMAKE_CURRENT_SOURCE_DIR}/ps_parse/lib)

add_executable(rtp_filter main.cpp ps.cpp rtp.cpp)

target_link_libraries(rtp_filter PkgConfig::LIBAV ps_parse)
