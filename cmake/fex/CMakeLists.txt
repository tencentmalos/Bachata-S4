# SPDX-FileCopyrightText: Copyright 2026 shadPS4 Emulator Project
# SPDX-License-Identifier: GPL-2.0-or-later
#
# Standalone entry point for the V0 Android native build.
#
# Kept separate from the desktop root CMakeLists on purpose: the Android and
# FEX targets must be opt-in, and FEX's compile options and allocator settings
# must not leak into the whole repository (spec §3).

cmake_minimum_required(VERSION 3.22)
project(shadps4_v0_android CXX C)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_POSITION_INDEPENDENT_CODE ON)

option(V0_ENABLE_FEX "Link the FEX guest CPU backend" OFF)
option(V0_BUILD_TESTS "Build the guest CPU contract tests" ON)

if(ANDROID)
    message(STATUS "V0: Android ${ANDROID_ABI} platform ${ANDROID_PLATFORM}")
    # 16 KiB ELF alignment. NDK r28 defaults this on, but state it explicitly
    # so the flag is visible in the build log and cannot regress silently
    # (acceptance B02).
    add_link_options("-Wl,-z,max-page-size=16384" "-Wl,-z,common-page-size=16384")
    # Keep a Build ID so a deployed library can be matched to its symbols.
    add_link_options("-Wl,--build-id=sha1")
endif()

add_subdirectory("${CMAKE_CURRENT_SOURCE_DIR}/../../src/core/guest_cpu"
                 "${CMAKE_CURRENT_BINARY_DIR}/guest_cpu")

if(V0_BUILD_TESTS)
    add_executable(guest_cpu_contract_tests
        "${CMAKE_CURRENT_SOURCE_DIR}/../../tests/guest_cpu/api_contract_tests.cpp")
    target_link_libraries(guest_cpu_contract_tests PRIVATE guest_cpu_api)

    add_executable(guest_cpu_hle_abi_tests
        "${CMAKE_CURRENT_SOURCE_DIR}/../../tests/guest_cpu/hle_abi_tests.cpp")
    target_link_libraries(guest_cpu_hle_abi_tests PRIVATE guest_cpu_api)
endif()

if(V0_ENABLE_FEX)
    # Deliberately fails rather than quietly building something that is not a
    # FEX backend. See docs/validation/v0/decisions.md DEC-03: FEXCore cannot
    # start on a 16 KiB host until four in-tree blockers are fixed, and FEX's
    # contribution rules forbid AI-generated changes, so this path is not
    # available in this milestone.
    message(FATAL_ERROR
        "V0_ENABLE_FEX is not available. FEXCore aborts at startup on a 16 KiB "
        "host (GetHostVABits, see docs/validation/v0/page-size-audit.md PS-01) "
        "and the required fixes are blocked by FEX's contribution rules "
        "(DEC-03). Build with -DV0_ENABLE_FEX=OFF.")
endif()
