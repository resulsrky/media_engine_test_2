cmake_minimum_required(VERSION 3.20)

project(Hydra VERSION 0.1.0 LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

option(HYDRA_ENABLE_WARNINGS "Enable extra compiler warnings" ON)

if(HYDRA_ENABLE_WARNINGS)
  if(MSVC)
    add_compile_options(/W4 /permissive-)
  else()
    add_compile_options(-Wall -Wextra -Wpedantic -Wconversion -Wsign-conversion)
  endif()
endif()

include(FetchContent)

# Fetch standalone Asio (header-only)
FetchContent_Declare(
  asio
  GIT_REPOSITORY https://github.com/chriskohlhoff/asio.git
  GIT_TAG asio-1-30-2
)
FetchContent_MakeAvailable(asio)

# Create an interface target exposing Asio headers and ASIO_STANDALONE define
add_library(asio_headers INTERFACE)
target_include_directories(asio_headers INTERFACE ${asio_SOURCE_DIR}/asio/include)
target_compile_definitions(asio_headers INTERFACE ASIO_STANDALONE)

find_package(Threads REQUIRED)

add_library(hydra_network STATIC
  src/network/NetworkSender.cpp
  src/network/NetworkReceiver.cpp
  src/network/Packetizer.cpp
)
target_include_directories(hydra_network PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/include)
target_link_libraries(hydra_network PUBLIC asio_headers Threads::Threads)
target_compile_features(hydra_network PUBLIC cxx_std_20)

add_executable(sender_app apps/sender/main.cpp)
target_link_libraries(sender_app PRIVATE hydra_network)
target_compile_features(sender_app PRIVATE cxx_std_20)

add_executable(receiver_app apps/receiver/main.cpp)
target_link_libraries(receiver_app PRIVATE hydra_network)
target_compile_features(receiver_app PRIVATE cxx_std_20)

add_executable(sender_video_app apps/sender_video/main.cpp)
target_link_libraries(sender_video_app PRIVATE hydra_network)
target_compile_features(sender_video_app PRIVATE cxx_std_20)

add_executable(receiver_video_app apps/receiver_video/main.cpp)
target_link_libraries(receiver_video_app PRIVATE hydra_network)
target_compile_features(receiver_video_app PRIVATE cxx_std_20)

# Optional: FFmpeg CPU pipeline
find_package(PkgConfig)
if(PkgConfig_FOUND)
  pkg_check_modules(FFMPEG libavcodec libavutil libavformat libswscale libavdevice)
  pkg_check_modules(SDL2 sdl2)
endif()

if(FFMPEG_FOUND)
  add_library(hydra_media_ffmpeg STATIC
    src/media/FFmpegCpuPipeline.cpp
    src/media/FFmpegDecoder.cpp
    src/media/SdlRenderer.cpp
    src/media/V4L2Enumerator.cpp
    src/media/FFmpegCameraEncodingPipeline.cpp
  )
  target_include_directories(hydra_media_ffmpeg PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/include)
  target_link_libraries(hydra_media_ffmpeg PUBLIC ${FFMPEG_LIBRARIES})
  target_include_directories(hydra_media_ffmpeg SYSTEM PUBLIC ${FFMPEG_INCLUDE_DIRS})
  target_compile_definitions(hydra_media_ffmpeg PUBLIC HYDRA_HAVE_FFMPEG=1)

  add_executable(sender_ffmpeg_app apps/sender_ffmpeg/main.cpp)
  target_link_libraries(sender_ffmpeg_app PRIVATE hydra_network hydra_media_ffmpeg)

  add_executable(receiver_ffmpeg_app apps/receiver_ffmpeg/main.cpp)
  target_link_libraries(receiver_ffmpeg_app PRIVATE hydra_network hydra_media_ffmpeg)

  if(SDL2_FOUND)
    target_include_directories(hydra_media_ffmpeg SYSTEM PUBLIC ${SDL2_INCLUDE_DIRS})
    target_link_libraries(hydra_media_ffmpeg PUBLIC ${SDL2_LIBRARIES})
    add_executable(duplex_ffmpeg_app apps/duplex_ffmpeg/main.cpp)
    target_link_libraries(duplex_ffmpeg_app PRIVATE hydra_network hydra_media_ffmpeg)
    add_executable(video_engine apps/video_engine/main.cpp)
    target_link_libraries(video_engine PRIVATE hydra_network hydra_media_ffmpeg)
    
    # Ultra-low latency video engine
    add_executable(realtime_video apps/realtime_video/main.cpp)
    target_link_libraries(realtime_video PRIVATE hydra_network)
    target_link_libraries(realtime_video PRIVATE ${SDL2_LIBRARIES})
    target_include_directories(realtime_video SYSTEM PRIVATE ${SDL2_INCLUDE_DIRS})
    
    # FFmpeg libraries via pkg-config
    target_link_libraries(realtime_video PRIVATE avcodec avformat avutil swscale)
    
    # Zero-latency video engine (extreme optimization)
    add_executable(zero_latency apps/zero_latency/main.cpp)
    target_link_libraries(zero_latency PRIVATE hydra_network)
    target_link_libraries(zero_latency PRIVATE ${SDL2_LIBRARIES})
    target_include_directories(zero_latency SYSTEM PRIVATE ${SDL2_INCLUDE_DIRS})
    target_link_libraries(zero_latency PRIVATE avcodec avformat avutil swscale)
  endif()
endif()



