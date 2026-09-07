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

    # Verifies the FEX host-page fixes (docs/fex-host-page-size-adaptation.md).
    # Deliberately standalone: it reproduces the layout and syscalls rather than
    # linking FEXCore, so it runs on the build host and on a device without
    # requiring the backend that does not exist yet.
    add_executable(host_page_size_probe
        "${CMAKE_CURRENT_SOURCE_DIR}/../../tests/host_page_size/host_page_size_probe.cpp")
endif()

if(V0_ENABLE_FEX)
    # The two 16 KiB blockers on the mandatory path (PS-02 code buffer guard,
    # PS-03 InterruptFaultPage) are fixed in the owned fork; see
    # docs/fex-host-page-size-adaptation.md. What is still missing is the
    # backend adapter itself, so this path would build FEXCore without
    # anything driving it.
    #
    # Fail rather than quietly produce a library with no working backend: a
    # stub that links would make the acceptance matrix look like it advanced
    # while nothing is actually executing guest code.
    message(FATAL_ERROR
        "V0_ENABLE_FEX is not available yet. The 16 KiB host page blockers are "
        "fixed (see docs/fex-host-page-size-adaptation.md), but the FEX backend "
        "adapter is not implemented, so there is nothing to drive FEXCore. "
        "Build with -DV0_ENABLE_FEX=OFF.")
endif()
