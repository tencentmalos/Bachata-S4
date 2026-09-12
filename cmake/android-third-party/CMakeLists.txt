# SPDX-License-Identifier: GPL-2.0-or-later
cmake_minimum_required(VERSION 3.24)
project(shadps4_android_third_party LANGUAGES C CXX ASM)
if(NOT ANDROID OR NOT ANDROID_ABI STREQUAL "arm64-v8a" OR NOT ANDROID_STL STREQUAL "c++_shared")
    message(FATAL_ERROR "Use the NDK toolchain, arm64-v8a and c++_shared")
endif()
set(CMAKE_CXX_STANDARD 23)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_POSITION_INDEPENDENT_CODE ON)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)
set(ARCHITECTURE arm64)
set(ENABLE_DISCORD_RPC OFF)
set(TRACY_ENABLE OFF CACHE BOOL "" FORCE)
get_filename_component(repo "${CMAKE_CURRENT_LIST_DIR}/../.." ABSOLUTE)
# Exercise the production recipes directly; FFmpeg has its own decode probe.
add_subdirectory("${repo}/externals" externals)
add_library(third_party_probe SHARED probe.cpp)
target_link_libraries(third_party_probe PRIVATE fmt::fmt spdlog::spdlog ZLIB::ZLIB
    PNG::PNG Freetype::Freetype SDL3::SDL3 sirit OpenAL::OpenAL libusb::usb
    fdk-aac LibAtrac9 ZArchive::zarchive protobuf::libprotobuf
    lfreist-hwinfo::hwinfo Cpp_Httplib)
target_include_directories(third_party_probe PRIVATE "${repo}/externals/zstd/lib")
target_link_options(third_party_probe PRIVATE -Wl,--no-undefined -Wl,--build-id=sha1
    -Wl,-z,max-page-size=16384 "-Wl,-Map,${CMAKE_CURRENT_BINARY_DIR}/third_party_probe.map")
add_executable(third_party_smoke smoke.cpp)
target_link_libraries(third_party_smoke PRIVATE third_party_probe)
target_link_options(third_party_smoke PRIVATE -Wl,--no-undefined -Wl,--build-id=sha1)
