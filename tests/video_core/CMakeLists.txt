# SPDX-FileCopyrightText: Copyright 2026 shadPS4 Emulator Project
# SPDX-License-Identifier: GPL-2.0-or-later
cmake_minimum_required(VERSION 3.22)
if(CMAKE_SOURCE_DIR STREQUAL CMAKE_CURRENT_SOURCE_DIR)
    project(shadps4_video_core_tests LANGUAGES CXX)
    enable_testing()
endif()
add_executable(swapchain_acquire_tests swapchain_acquire_tests.cpp)
target_compile_features(swapchain_acquire_tests PRIVATE cxx_std_20)
target_include_directories(swapchain_acquire_tests PRIVATE
    ../../src ../../externals/vulkan-headers/include)
add_test(NAME swapchain_acquire_tests COMMAND swapchain_acquire_tests)
