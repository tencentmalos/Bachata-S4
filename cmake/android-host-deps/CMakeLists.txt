# SPDX-FileCopyrightText: Copyright 2026 shadPS4 Emulator Project
# SPDX-License-Identifier: GPL-2.0-or-later
cmake_minimum_required(VERSION 3.22.1)
project(shadps4_android_host_deps LANGUAGES C CXX ASM)

if(NOT ANDROID OR NOT ANDROID_ABI STREQUAL "arm64-v8a")
    message(FATAL_ERROR "Use the Android NDK toolchain with arm64-v8a")
endif()
if(NOT ANDROID_STL STREQUAL "c++_shared")
    message(FATAL_ERROR "Use c++_shared, matching the production APK")
endif()
set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_POSITION_INDEPENDENT_CODE ON)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)
set(BUILD_SHARED_LIBS OFF)
set(BUILD_TESTING OFF)
include("${CMAKE_CURRENT_LIST_DIR}/../HostPortableDependencies.cmake")
foreach(dep date glslang zydis miniz libressl)
    shadps4_add_host_portable_dependency(${dep})
endforeach()

# This DSO pulls real library functions; an empty archive link is insufficient.
# It is a dependency probe only, never packaged as the game host.
add_library(host_deps_probe SHARED probe.cpp)
target_link_libraries(host_deps_probe PRIVATE date::date-tz glslang::glslang
    glslang::glslang-default-resource-limits Zydis::Zydis miniz::miniz ${SSL_DEPENDENCIES})
target_link_options(host_deps_probe PRIVATE -Wl,--no-undefined -Wl,--build-id=sha1
    "-Wl,-Map,${CMAKE_CURRENT_BINARY_DIR}/host_deps_probe.map"
    -Wl,-z,max-page-size=16384)
add_executable(host_deps_smoke smoke.cpp)
target_link_libraries(host_deps_smoke PRIVATE host_deps_probe)
target_link_options(host_deps_smoke PRIVATE -Wl,--no-undefined -Wl,--build-id=sha1)
