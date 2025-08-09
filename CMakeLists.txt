cmake_minimum_required(VERSION 3.16)

project(media_engine_test LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

find_package(PkgConfig REQUIRED)
pkg_check_modules(GST REQUIRED
        gstreamer-1.0
        gstreamer-video-1.0
        gstreamer-rtp-1.0
        gstreamer-pbutils-1.0
)

add_executable(nova_engine
        src/nova_engine.cpp
        src/gpu_detect.cpp
)

target_include_directories(nova_engine PRIVATE ${GST_INCLUDE_DIRS})
target_link_directories(nova_engine PRIVATE ${GST_LIBRARY_DIRS})
target_compile_options(nova_engine PRIVATE ${GST_CFLAGS_OTHER})
target_link_libraries(nova_engine PRIVATE ${GST_LIBRARIES} pthread)
