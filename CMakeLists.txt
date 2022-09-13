cmake_minimum_required(VERSION 3.21)

project(h264_filter)

set(CMAKE_CXX_STANDARD 17)

set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

set (Boost_FIND_REQUIRED TRUE)
set (Boost_FIND_QUIETLY TRUE)
set (Boost_DEBUG FALSE)
set (Boost_USE_MULTITHREADED TRUE)

find_package (Boost)

find_package(PkgConfig REQUIRED)
pkg_check_modules(LIBAV REQUIRED IMPORTED_TARGET
    libavfilter
    libavformat
    libavcodec
    libavutil
    )

include_directories (${Boost_INCLUDE_DIRS})
link_directories (${Boost_LIBRARY_DIRS})

add_executable(${PROJECT_NAME} main.cpp ${Boost_LIBRARIES})

target_link_libraries(${PROJECT_NAME} ${Boost_LIBRARIES}      PkgConfig::LIBAV)
