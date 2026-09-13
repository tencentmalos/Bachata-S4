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
option(V0_BUILD_EBOOT_HARNESS "Build the auxiliary eboot probe using a native host build" OFF)
set(V0_HOST_LOADER_CONFIG "" CACHE FILEPATH "Generated shadps4-host-loader.cmake from BUILD_HOST_CORE")
if(V0_BUILD_EBOOT_HARNESS AND (NOT V0_BUILD_TESTS OR NOT ANDROID))
    message(FATAL_ERROR "The eboot load audit requires Android and V0_BUILD_TESTS")
endif()

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
    target_compile_definitions(guest_cpu_api PRIVATE GUEST_CPU_TEST_HOOKS=1)
    # M13g and M13j race two host threads, so this needs a threading library on hosts where it is
    # not implicit.
    find_package(Threads REQUIRED)

    add_executable(guest_cpu_contract_tests
        "${CMAKE_CURRENT_SOURCE_DIR}/../../tests/guest_cpu/api_contract_tests.cpp")
    target_link_libraries(guest_cpu_contract_tests PRIVATE guest_cpu_api Threads::Threads)

    add_executable(guest_cpu_hle_abi_tests
        "${CMAKE_CURRENT_SOURCE_DIR}/../../tests/guest_cpu/hle_abi_tests.cpp")
    target_link_libraries(guest_cpu_hle_abi_tests PRIVATE guest_cpu_api Threads::Threads)

    # Verifies the FEX host-page fixes (docs/fex-host-page-size-adaptation.md).
    # Deliberately standalone: it reproduces the layout and syscalls rather than
    # linking FEXCore, so it runs on the build host and on a device without
    # requiring the backend that does not exist yet.
    add_executable(host_page_size_probe
        "${CMAKE_CURRENT_SOURCE_DIR}/../../tests/host_page_size/host_page_size_probe.cpp")

    # Host-deterministic test of the test-only Run-entry gate state machine. Standard-library only,
    # so it runs on the build host (no FEX link) and exercises the same protocol header the FEX
    # backend includes under GUEST_CPU_TEST_HOOKS.
    add_executable(test_run_gate_protocol
        "${CMAKE_CURRENT_SOURCE_DIR}/../../tests/guest_cpu/test_run_gate_protocol.cpp")
    target_include_directories(test_run_gate_protocol PRIVATE
        "${CMAKE_CURRENT_SOURCE_DIR}/../../src")

    # HN0.1 host session lifecycle. SessionCore + a deterministic FakeBackend (no FEX, no JIT).
    # SESSION_TEST_HOOKS arms the handshake gate and the UAF tripwire; production .so never defines
    # it. Links guest_cpu_api only, so it builds and runs on the CI host.
    add_executable(session_lifecycle_tests
        "${CMAKE_CURRENT_SOURCE_DIR}/../../src/core/host_runtime/session_core.cpp"
        "${CMAKE_CURRENT_SOURCE_DIR}/../../src/core/host_runtime/session_backend.cpp"
        "${CMAKE_CURRENT_SOURCE_DIR}/../../tests/host_runtime/session_lifecycle_tests.cpp")
    target_link_libraries(session_lifecycle_tests PRIVATE guest_cpu_api Threads::Threads)
    target_compile_definitions(session_lifecycle_tests PRIVATE SESSION_TEST_HOOKS=1)

    # Native pad adapter contract. OrbisPadAdapter is the native consumer of Android
    # controller input; it reuses the real Libraries::Pad::OrbisPadData layout and
    # depends only on that header + the C++ stdlib (no FEX, no SDL, no guest_cpu), so
    # it builds and runs on the CI host and cross-compiles under the NDK.
    add_executable(orbis_pad_adapter_tests
        "${CMAKE_CURRENT_SOURCE_DIR}/../../src/core/host_runtime/orbis_pad_adapter.cpp"
        "${CMAKE_CURRENT_SOURCE_DIR}/../../tests/host_runtime/orbis_pad_adapter_tests.cpp")
    target_include_directories(orbis_pad_adapter_tests PRIVATE
        "${CMAKE_CURRENT_SOURCE_DIR}/../../src")
    if(NOT TARGET foundation_input)
        add_subdirectory("${CMAKE_CURRENT_SOURCE_DIR}/../../foundation/modules/input" foundation/input EXCLUDE_FROM_ALL)
    endif()
    target_link_libraries(orbis_pad_adapter_tests PRIVATE Threads::Threads foundation_input)

    # HLE veneer allocator contract. Backend-free: links guest_cpu_api only (the
    # allocator lives in that library), so it runs on the CI host and cross-compiles
    # under the NDK. The guest-call-through-a-veneer round trip is in
    # guest_execution_tests, which needs FEX.
    add_executable(veneer_allocator_tests
        "${CMAKE_CURRENT_SOURCE_DIR}/../../tests/guest_cpu/veneer_allocator_tests.cpp")
    target_link_libraries(veneer_allocator_tests PRIVATE guest_cpu_api Threads::Threads)
endif()

if(V0_ENABLE_FEX)
    # The FEX guest CPU backend.
    #
    # This used to be a FATAL_ERROR saying the adapter did not exist. It does now: it executes real
    # x86-64 and is what tests/guest_cpu/guest_execution_tests drives on a device. The build was
    # still being done by hand in scripts/android/build-fexcore-android, so the target list existed
    # twice and could drift; this is the single definition.
    #
    # FEXCore must already be configured and built, because its own CMake brings compile options
    # and allocator settings that must not leak into this project (spec §3). Point FEX_BUILD_DIR at
    # the directory scripts/android/build-fexcore-android produced.
    if(NOT DEFINED FEX_BUILD_DIR)
        message(FATAL_ERROR
            "V0_ENABLE_FEX requires -DFEX_BUILD_DIR=<dir>, the FEXCore build produced by "
            "scripts/android/build-fexcore-android. FEXCore is not configured from here so its "
            "compile options and allocator settings do not leak into this project.")
    endif()
    if(NOT EXISTS "${FEX_BUILD_DIR}/FEXCore/Source/libFEXCore.a")
        message(FATAL_ERROR
            "no libFEXCore.a under ${FEX_BUILD_DIR}. Run scripts/android/build-fexcore-android "
            "first; a stub that links would make the acceptance matrix look like it advanced while "
            "nothing executes guest code.")
    endif()

    get_filename_component(FEX_SOURCE_DIR "${CMAKE_CURRENT_SOURCE_DIR}/../../references/FEX" ABSOLUTE)

    add_library(guest_cpu_fex STATIC
        "${CMAKE_CURRENT_SOURCE_DIR}/../../src/core/guest_cpu/fex/fex_context.cpp")

    # PRIVATE throughout: a consumer of the public API must not need any FEX include path, and
    # acceptance B05 checks that no FEX header is reachable from it.
    target_include_directories(guest_cpu_fex PRIVATE
        "${FEX_BUILD_DIR}/FEXCore/Source"
        "${FEX_BUILD_DIR}/Source"
        "${FEX_BUILD_DIR}/generated"
        "${FEX_BUILD_DIR}/include"
        "${FEX_SOURCE_DIR}/FEXCore/Source"
        "${FEX_SOURCE_DIR}/FEXCore/Source/IncludePrivate"
        "${FEX_SOURCE_DIR}/FEXCore/include"
        "${FEX_SOURCE_DIR}/Source"
        "${FEX_SOURCE_DIR}/FEXHeaderUtils"
        "${FEX_SOURCE_DIR}/CodeEmitter"
        "${FEX_SOURCE_DIR}/External/fmt/include"
        "${FEX_SOURCE_DIR}/External/xxhash"
        "${FEX_SOURCE_DIR}/External/unordered_dense/include")
    target_compile_definitions(guest_cpu_fex PRIVATE ARCHITECTURE_arm64=1 FEX_DISABLE_TELEMETRY=1)
    # Test-only Run entry gate (see fex_context.cpp FexTestEntryGate). Enabled only in test builds;
    # release products do not compile the wait mechanism at all.
    if(V0_BUILD_TESTS)
        target_compile_definitions(guest_cpu_fex PRIVATE GUEST_CPU_TEST_HOOKS=1)
    endif()

    # FEXCore, FEXCore_Base and the allocator libraries reference each other cyclically, so they
    # need a link group rather than a topological order.
    target_link_libraries(guest_cpu_fex PUBLIC
        guest_cpu_api
        -Wl,--start-group
        "${FEX_BUILD_DIR}/FEXCore/Source/libFEXCore.a"
        "${FEX_BUILD_DIR}/Source/Common/libCommon.a"
        "${FEX_BUILD_DIR}/FEXCore/Source/libFEXCore_Base.a"
        "${FEX_BUILD_DIR}/FEXCore/Source/libJemallocLibs.a"
        "${FEX_BUILD_DIR}/External/rpmalloc/librpmalloc.a"
        "${FEX_BUILD_DIR}/External/SoftFloat-3e/libsoftfloat_3e.a"
        "${FEX_BUILD_DIR}/External/cephes/libcephes_128bit.a"
        "${FEX_BUILD_DIR}/External/fmt/libfmt.a"
        "${FEX_BUILD_DIR}/External/xxhash/cmake_unofficial/libxxhash.a"
        "${FEX_BUILD_DIR}/External/tiny-json/libtiny-json.a"
        -Wl,--end-group)
    if(ANDROID)
        target_link_libraries(guest_cpu_fex PUBLIC log)
    endif()

    if(V0_BUILD_TESTS)
        # Link-and-init only; executes no guest code. Feeds P04/P05.
        add_executable(fexcore_bionic_smoke
            "${CMAKE_CURRENT_SOURCE_DIR}/../../tests/fex_bionic/fexcore_bionic_smoke.cpp")
        target_link_libraries(fexcore_bionic_smoke PRIVATE guest_cpu_fex)
        target_include_directories(fexcore_bionic_smoke PRIVATE
            "${FEX_BUILD_DIR}/FEXCore/Source"
            "${FEX_BUILD_DIR}/Source"
            "${FEX_BUILD_DIR}/generated"
            "${FEX_BUILD_DIR}/include"
            "${FEX_SOURCE_DIR}/FEXCore/Source"
            "${FEX_SOURCE_DIR}/FEXCore/Source/IncludePrivate"
            "${FEX_SOURCE_DIR}/FEXCore/include"
            "${FEX_SOURCE_DIR}/Source"
            "${FEX_SOURCE_DIR}/FEXHeaderUtils"
            "${FEX_SOURCE_DIR}/CodeEmitter"
            "${FEX_SOURCE_DIR}/External/fmt/include"
            "${FEX_SOURCE_DIR}/External/xxhash"
            "${FEX_SOURCE_DIR}/External/unordered_dense/include")
        target_compile_definitions(fexcore_bionic_smoke PRIVATE
            ARCHITECTURE_arm64=1 FEX_DISABLE_TELEMETRY=1)

        # Real guest execution. Needs the generated fixtures, which are assembled from source and
        # disassembled back so a wrong instruction is a build failure rather than a wrong result.
        if(NOT DEFINED V0_FIXTURE_DIR)
            set(V0_FIXTURE_DIR "${FEX_BUILD_DIR}/generated-fixtures")
        endif()
        find_package(Python3 REQUIRED COMPONENTS Interpreter)
        set(FIXTURE_GENERATOR "${CMAKE_CURRENT_SOURCE_DIR}/../../scripts/android/generate-guest-fixtures")
        set(FIXTURE_SOURCE "${CMAKE_CURRENT_SOURCE_DIR}/../../tests/guest_cpu/fixtures/guest_fixtures.S")
        add_custom_command(OUTPUT "${V0_FIXTURE_DIR}/guest_fixtures.h"
            COMMAND "${Python3_EXECUTABLE}" "${FIXTURE_GENERATOR}"
                    --source "${FIXTURE_SOURCE}" --output "${V0_FIXTURE_DIR}/guest_fixtures.h"
            DEPENDS "${FIXTURE_GENERATOR}" "${FIXTURE_SOURCE}"
            COMMENT "Assemble and verify self-contained x86-64 guest fixtures"
            VERBATIM)
        add_executable(guest_execution_tests
            "${CMAKE_CURRENT_SOURCE_DIR}/../../tests/guest_cpu/guest_execution_tests.cpp"
            "${V0_FIXTURE_DIR}/guest_fixtures.h")
        target_include_directories(guest_execution_tests PRIVATE "${V0_FIXTURE_DIR}")
        target_link_libraries(guest_execution_tests PRIVATE guest_cpu_fex)
        # See the matching definition on guest_cpu_fex: exposes the test-only FexTestRunGate
        # declaration the test drives. Release consumers do not define this.
        target_compile_definitions(guest_execution_tests PRIVATE GUEST_CPU_TEST_HOOKS=1)

    endif()
endif()

# Load audit does not execute guest code; it needs no FEX runtime archive.
if(V0_BUILD_EBOOT_HARNESS)
    if(NOT EXISTS "${V0_HOST_LOADER_CONFIG}")
        message(FATAL_ERROR "Build the native host first and set V0_HOST_LOADER_CONFIG")
    endif()
    include("${V0_HOST_LOADER_CONFIG}")
    if(NOT EXISTS "${SHADPS4_HOST_LOADER_LIBRARY}" OR
       NOT ANDROID_ABI STREQUAL SHADPS4_HOST_LOADER_ABI OR
       NOT ANDROID_STL STREQUAL SHADPS4_HOST_LOADER_STL OR
       ANDROID_PLATFORM_LEVEL LESS SHADPS4_HOST_LOADER_API)
        message(FATAL_ERROR "Native host loader is missing or ABI/API/STL is incompatible")
    endif()
    add_library(eboot_host_loader SHARED IMPORTED)
    set_target_properties(eboot_host_loader PROPERTIES
        IMPORTED_LOCATION "${SHADPS4_HOST_LOADER_LIBRARY}")
    add_executable(eboot_prologue_harness
        "${CMAKE_CURRENT_SOURCE_DIR}/../../tools/eboot_prologue_harness.cpp")
    target_compile_features(eboot_prologue_harness PRIVATE cxx_std_23)
    target_include_directories(eboot_prologue_harness PRIVATE ${SHADPS4_HOST_LOADER_INCLUDES})
    target_compile_definitions(eboot_prologue_harness PRIVATE ${SHADPS4_HOST_LOADER_DEFINITIONS})
    target_link_libraries(eboot_prologue_harness PRIVATE guest_cpu_api eboot_host_loader)
    target_link_options(eboot_prologue_harness PRIVATE -Wl,--no-undefined
        "-Wl,-Map,${CMAKE_CURRENT_BINARY_DIR}/eboot_prologue_harness.map")
endif()
