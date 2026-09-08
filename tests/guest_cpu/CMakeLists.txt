# SPDX-FileCopyrightText: Copyright 2026 shadPS4 Emulator Project
# SPDX-License-Identifier: GPL-2.0-or-later

# V0 guest CPU suites, buildable without the desktop emulator.
#
# These were previously built by hand: the 2026-09-08 publication review's host run used a
# CMakeLists written into a temp directory that no longer exists, so the recipe could not be
# reproduced from the repository and scripts/android/run-v0-tests found no binaries to run. The
# targets live here so a run is reproducible from a clean checkout.
#
# Deliberately standalone. Adding these to the desktop tests/ tree would make every V0 run depend
# on configuring the whole emulator, which is minutes of work for suites that need only the public
# API.

cmake_minimum_required(VERSION 3.20)
project(shadps4_guest_cpu_v0 CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

# Repository root: this file is tests/guest_cpu/.
get_filename_component(SHADPS4_ROOT "${CMAKE_CURRENT_SOURCE_DIR}/../.." ABSOLUTE)

# The backend-free public API. Same target the desktop build uses.
add_subdirectory("${SHADPS4_ROOT}/src/core/guest_cpu" guest_cpu)

find_package(Threads REQUIRED)

# Host suites. These link guest_cpu_api only: no FEX include path, no FEX definitions (B05).
add_executable(guest_cpu_contract_tests
    "${CMAKE_CURRENT_SOURCE_DIR}/api_contract_tests.cpp")
target_link_libraries(guest_cpu_contract_tests PRIVATE guest_cpu_api Threads::Threads)

add_executable(guest_cpu_hle_abi_tests
    "${CMAKE_CURRENT_SOURCE_DIR}/hle_abi_tests.cpp")
target_link_libraries(guest_cpu_hle_abi_tests PRIVATE guest_cpu_api Threads::Threads)

add_executable(host_page_size_probe
    "${SHADPS4_ROOT}/tests/host_page_size/host_page_size_probe.cpp")
target_link_libraries(host_page_size_probe PRIVATE Threads::Threads)
target_include_directories(host_page_size_probe PRIVATE "${SHADPS4_ROOT}/src")

enable_testing()
add_test(NAME guest_cpu_contract COMMAND guest_cpu_contract_tests)
add_test(NAME guest_cpu_hle_abi COMMAND guest_cpu_hle_abi_tests)
