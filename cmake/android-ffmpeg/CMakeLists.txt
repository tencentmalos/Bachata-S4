# SPDX-License-Identifier: GPL-2.0-or-later
cmake_minimum_required(VERSION 3.22.1)
project(shadps4_android_ffmpeg_probe LANGUAGES C CXX)
set(CMAKE_CXX_STANDARD 20)
set(CMAKE_POSITION_INDEPENDENT_CODE ON)
get_filename_component(repo "${CMAKE_CURRENT_LIST_DIR}/../.." ABSOLUTE)
add_subdirectory("${repo}/externals/ffmpeg/cmake/android" ffmpeg)
add_library(ffmpeg_media_probe SHARED probe.cpp)
target_link_libraries(ffmpeg_media_probe PRIVATE FFmpeg::ffmpeg)
target_link_options(ffmpeg_media_probe PRIVATE -Wl,--no-undefined -Wl,--build-id=sha1
    -Wl,-z,max-page-size=16384 "-Wl,-Map,${CMAKE_CURRENT_BINARY_DIR}/ffmpeg_media_probe.map")
add_executable(ffmpeg_media_smoke smoke.cpp)
target_link_libraries(ffmpeg_media_smoke PRIVATE ffmpeg_media_probe)
target_link_options(ffmpeg_media_smoke PRIVATE -Wl,--no-undefined -Wl,--build-id=sha1)
