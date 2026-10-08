# SPDX-FileCopyrightText: Copyright 2026 shadPS4 Emulator Project
# SPDX-License-Identifier: GPL-2.0-or-later

# Find or download Google Test
include(FetchContent)
    FetchContent_Declare(
    googletest
    URL https://github.com/google/googletest/archive/refs/tags/v1.17.0.zip
)
FetchContent_MakeAvailable(googletest)

set(TEST_TARGETS
    shadps4_settings_test
    shadps4_ngs2_test
    shadps4_gcn_test
    shadps4_content_roots_test
    shadps4_audio_transfer_test
    shadps4_guest_patch_format_test
)

set(SETTINGS_TEST_SOURCES
    # Under test
    ${CMAKE_SOURCE_DIR}/src/core/emulator_settings.cpp
    ${CMAKE_SOURCE_DIR}/src/core/emulator_state.cpp

    # Minimal common support
    ${CMAKE_SOURCE_DIR}/src/common/path_util.cpp
    ${CMAKE_SOURCE_DIR}/src/common/assert.cpp
    ${CMAKE_SOURCE_DIR}/src/common/error.cpp
    ${CMAKE_SOURCE_DIR}/src/common/string_util.cpp
    ${CMAKE_SOURCE_DIR}/src/common/logging/classes.cpp
    ${CMAKE_SOURCE_DIR}/src/common/logging/log.cpp
    ${CMAKE_SOURCE_DIR}/src/common/logging/log_stats.cpp
    ${CMAKE_SOURCE_DIR}/src/common/profiler.cpp

    # Stubs that replace dependencies
    stubs/common_stub.cpp
    stubs/core_stub.cpp
    stubs/signals_stub.cpp
    stubs/file_sys_stub.cpp
    stubs/scm_rev_stub.cpp
    stubs/sdl_stub.cpp

    # Tests
    test_emulator_settings.cpp
)

set(NGS2_TEST_SOURCES
    # Under test
    ${CMAKE_SOURCE_DIR}/src/core/libraries/ngs2/ngs2_impl.cpp

    # Minimal common and kernel support
    ${CMAKE_SOURCE_DIR}/src/core/emulator_settings.cpp
    ${CMAKE_SOURCE_DIR}/src/core/emulator_state.cpp
    ${CMAKE_SOURCE_DIR}/src/core/libraries/kernel/sync/mutex.cpp
    ${CMAKE_SOURCE_DIR}/src/common/path_util.cpp
    ${CMAKE_SOURCE_DIR}/src/common/assert.cpp
    ${CMAKE_SOURCE_DIR}/src/common/error.cpp
    ${CMAKE_SOURCE_DIR}/src/common/string_util.cpp
    ${CMAKE_SOURCE_DIR}/src/common/logging/classes.cpp
    ${CMAKE_SOURCE_DIR}/src/common/logging/log.cpp
    ${CMAKE_SOURCE_DIR}/src/common/logging/log_stats.cpp
    ${CMAKE_SOURCE_DIR}/src/common/profiler.cpp

    # Stubs that replace dependencies
    stubs/common_stub.cpp
    stubs/core_stub.cpp
    stubs/signals_stub.cpp
    stubs/file_sys_stub.cpp
    stubs/scm_rev_stub.cpp
    stubs/sdl_stub.cpp

    # Tests
    test_ngs2.cpp
)
set(GCN_TEST_SOURCES
    # Under test
    ${CMAKE_SOURCE_DIR}/src/core/emulator_settings.cpp
    ${CMAKE_SOURCE_DIR}/src/core/emulator_state.cpp
    # Kept in step with SHADER_RECOMPILER in the top-level CMakeLists.txt, except
    # recompiler.cpp: the tests run their own pass list (stubs/resource_tracking_pass_stub.cpp).
    ${CMAKE_SOURCE_DIR}/src/shader_recompiler/ir/passes/resource_patching_pass.cpp
    ${CMAKE_SOURCE_DIR}/src/common/decoder.cpp
    ${CMAKE_SOURCE_DIR}/src/common/signal_context.cpp
    ${CMAKE_SOURCE_DIR}/src/shader_recompiler/srt_gate_diag.cpp
    ${CMAKE_SOURCE_DIR}/src/shader_recompiler/backend/spirv/emit_spirv_discard_frag.cpp
    ${CMAKE_SOURCE_DIR}/src/shader_recompiler/backend/spirv/spirv_int64.cpp
    ${CMAKE_SOURCE_DIR}/src/shader_recompiler/backend/spirv/emit_spirv_interpolation.cpp
    ${CMAKE_SOURCE_DIR}/src/shader_recompiler/backend/spirv/emit_spirv_quad_rect.cpp
    ${CMAKE_SOURCE_DIR}/src/shader_recompiler/frontend/control_flow_graph.cpp
    ${CMAKE_SOURCE_DIR}/src/shader_recompiler/frontend/copy_shader.cpp
    ${CMAKE_SOURCE_DIR}/src/shader_recompiler/frontend/instruction.cpp
    ${CMAKE_SOURCE_DIR}/src/shader_recompiler/frontend/structured_control_flow.cpp
    ${CMAKE_SOURCE_DIR}/src/shader_recompiler/ir/passes/flatten_extended_userdata_pass.cpp
    ${CMAKE_SOURCE_DIR}/src/shader_recompiler/ir/passes/hull_shader_transform.cpp
    ${CMAKE_SOURCE_DIR}/src/shader_recompiler/ir/passes/inject_clip_distance_attributes.cpp
    ${CMAKE_SOURCE_DIR}/src/shader_recompiler/ir/passes/inverse_ballot_elimination_pass.cpp
    ${CMAKE_SOURCE_DIR}/src/shader_recompiler/ir/passes/lower_buffer_format_to_raw.cpp
    ${CMAKE_SOURCE_DIR}/src/shader_recompiler/ir/passes/lower_fp64_to_fp32.cpp
    ${CMAKE_SOURCE_DIR}/src/shader_recompiler/ir/passes/lower_hardware_intrinsics.cpp
    ${CMAKE_SOURCE_DIR}/src/shader_recompiler/ir/passes/lower_user_clip_planes.cpp
    ${CMAKE_SOURCE_DIR}/src/shader_recompiler/ir/passes/lower_wave64_pass.cpp
    ${CMAKE_SOURCE_DIR}/src/shader_recompiler/ir/passes/phi_simplification_pass.cpp
    ${CMAKE_SOURCE_DIR}/src/shader_recompiler/ir/passes/readlane_elimination_pass.cpp
    ${CMAKE_SOURCE_DIR}/src/shader_recompiler/ir/passes/resource_discover_pass.cpp
    ${CMAKE_SOURCE_DIR}/src/shader_recompiler/ir/passes/ring_access_elimination.cpp
    ${CMAKE_SOURCE_DIR}/src/shader_recompiler/ir/passes/shared_memory_barrier_pass.cpp
    ${CMAKE_SOURCE_DIR}/src/shader_recompiler/ir/passes/shared_memory_simplify_pass.cpp
    ${CMAKE_SOURCE_DIR}/src/shader_recompiler/ir/passes/fragment_lds_pass.cpp
    ${CMAKE_SOURCE_DIR}/src/shader_recompiler/ir/passes/shared_memory_to_storage_pass.cpp
    ${CMAKE_SOURCE_DIR}/src/shader_recompiler/ir/passes/lower_phis_to_regs_pass.cpp
    ${CMAKE_SOURCE_DIR}/src/shader_recompiler/ir/passes/ssa_repair_pass.cpp
    ${CMAKE_SOURCE_DIR}/src/shader_recompiler/ir/dominance.cpp
    ${CMAKE_SOURCE_DIR}/src/shader_recompiler/backend/spirv/emit_spirv.cpp
    ${CMAKE_SOURCE_DIR}/src/shader_recompiler/backend/spirv/emit_spirv.h
    ${CMAKE_SOURCE_DIR}/src/shader_recompiler/backend/spirv/emit_spirv_atomic.cpp
    ${CMAKE_SOURCE_DIR}/src/shader_recompiler/backend/spirv/emit_spirv_barriers.cpp
    ${CMAKE_SOURCE_DIR}/src/shader_recompiler/backend/spirv/emit_spirv_bitwise_conversion.cpp
    ${CMAKE_SOURCE_DIR}/src/shader_recompiler/backend/spirv/emit_spirv_composite.cpp
    ${CMAKE_SOURCE_DIR}/src/shader_recompiler/backend/spirv/emit_spirv_context_get_set.cpp
    ${CMAKE_SOURCE_DIR}/src/shader_recompiler/backend/spirv/emit_spirv_convert.cpp
    ${CMAKE_SOURCE_DIR}/src/shader_recompiler/backend/spirv/emit_spirv_floating_point.cpp
    ${CMAKE_SOURCE_DIR}/src/shader_recompiler/backend/spirv/emit_spirv_image.cpp
    ${CMAKE_SOURCE_DIR}/src/shader_recompiler/backend/spirv/emit_spirv_integer.cpp
    ${CMAKE_SOURCE_DIR}/src/shader_recompiler/backend/spirv/emit_spirv_logical.cpp
    ${CMAKE_SOURCE_DIR}/src/shader_recompiler/backend/spirv/emit_spirv_select.cpp
    ${CMAKE_SOURCE_DIR}/src/shader_recompiler/backend/spirv/emit_spirv_shared_memory.cpp
    ${CMAKE_SOURCE_DIR}/src/shader_recompiler/backend/spirv/emit_spirv_special.cpp
    ${CMAKE_SOURCE_DIR}/src/shader_recompiler/backend/spirv/emit_spirv_undefined.cpp
    ${CMAKE_SOURCE_DIR}/src/shader_recompiler/backend/spirv/emit_spirv_warp.cpp
    ${CMAKE_SOURCE_DIR}/src/shader_recompiler/backend/spirv/emit_spirv_instructions.h
    ${CMAKE_SOURCE_DIR}/src/shader_recompiler/backend/spirv/spirv_emit_context.cpp
    ${CMAKE_SOURCE_DIR}/src/shader_recompiler/backend/spirv/spirv_emit_context.h
    ${CMAKE_SOURCE_DIR}/src/shader_recompiler/frontend/format.cpp
    ${CMAKE_SOURCE_DIR}/src/shader_recompiler/frontend/decode.cpp
    ${CMAKE_SOURCE_DIR}/src/shader_recompiler/frontend/decode.h
    ${CMAKE_SOURCE_DIR}/src/shader_recompiler/frontend/fetch_shader.cpp
    ${CMAKE_SOURCE_DIR}/src/shader_recompiler/frontend/fetch_shader.h
    ${CMAKE_SOURCE_DIR}/src/shader_recompiler/frontend/translate/data_share.cpp
    ${CMAKE_SOURCE_DIR}/src/shader_recompiler/frontend/translate/export.cpp
    ${CMAKE_SOURCE_DIR}/src/shader_recompiler/frontend/translate/translate.cpp
    ${CMAKE_SOURCE_DIR}/src/shader_recompiler/frontend/translate/translate.h
    ${CMAKE_SOURCE_DIR}/src/shader_recompiler/frontend/translate/scalar_alu.cpp
    ${CMAKE_SOURCE_DIR}/src/shader_recompiler/frontend/translate/scalar_flow.cpp
    ${CMAKE_SOURCE_DIR}/src/shader_recompiler/frontend/translate/scalar_memory.cpp
    ${CMAKE_SOURCE_DIR}/src/shader_recompiler/frontend/translate/vector_alu.cpp
    ${CMAKE_SOURCE_DIR}/src/shader_recompiler/frontend/translate/vector_interpolation.cpp
    ${CMAKE_SOURCE_DIR}/src/shader_recompiler/frontend/translate/vector_memory.cpp
    ${CMAKE_SOURCE_DIR}/src/shader_recompiler/ir/abstract_syntax_list.cpp
    ${CMAKE_SOURCE_DIR}/src/shader_recompiler/ir/abstract_syntax_list.h
    ${CMAKE_SOURCE_DIR}/src/shader_recompiler/ir/basic_block.cpp
    ${CMAKE_SOURCE_DIR}/src/shader_recompiler/ir/basic_block.h
    ${CMAKE_SOURCE_DIR}/src/shader_recompiler/ir/microinstruction.cpp
    ${CMAKE_SOURCE_DIR}/src/shader_recompiler/ir/attribute.cpp
    ${CMAKE_SOURCE_DIR}/src/shader_recompiler/ir/attribute.h
    ${CMAKE_SOURCE_DIR}/src/shader_recompiler/ir/ir_emitter.cpp
    ${CMAKE_SOURCE_DIR}/src/shader_recompiler/ir/ir_emitter.h
    ${CMAKE_SOURCE_DIR}/src/shader_recompiler/ir/opcodes.cpp
    ${CMAKE_SOURCE_DIR}/src/shader_recompiler/ir/opcodes.h
    ${CMAKE_SOURCE_DIR}/src/shader_recompiler/ir/patch.cpp
    ${CMAKE_SOURCE_DIR}/src/shader_recompiler/ir/patch.h
    ${CMAKE_SOURCE_DIR}/src/shader_recompiler/ir/program.cpp
    ${CMAKE_SOURCE_DIR}/src/shader_recompiler/ir/program.h
    ${CMAKE_SOURCE_DIR}/src/shader_recompiler/ir/passes/ir_passes.h
    ${CMAKE_SOURCE_DIR}/src/shader_recompiler/ir/passes/constant_propagation_pass.cpp
    ${CMAKE_SOURCE_DIR}/src/shader_recompiler/ir/passes/dead_code_elimination_pass.cpp
    ${CMAKE_SOURCE_DIR}/src/shader_recompiler/ir/passes/shader_info_collection_pass.cpp
    ${CMAKE_SOURCE_DIR}/src/shader_recompiler/ir/passes/ssa_rewrite_pass.cpp
    ${CMAKE_SOURCE_DIR}/src/shader_recompiler/ir/post_order.cpp
    ${CMAKE_SOURCE_DIR}/src/shader_recompiler/ir/post_order.h
    ${CMAKE_SOURCE_DIR}/src/shader_recompiler/ir/type.cpp
    ${CMAKE_SOURCE_DIR}/src/shader_recompiler/ir/type.h
    ${CMAKE_SOURCE_DIR}/src/shader_recompiler/ir/value.cpp
    ${CMAKE_SOURCE_DIR}/src/shader_recompiler/ir/value.h
    ${CMAKE_SOURCE_DIR}/src/video_core/amdgpu/pixel_format.cpp
    ${CMAKE_SOURCE_DIR}/src/video_core/amdgpu/pixel_format.h
    ${CMAKE_SOURCE_DIR}/src/video_core/amdgpu/resource.h

    # Minimal common support
    ${CMAKE_SOURCE_DIR}/src/common/path_util.cpp
    ${CMAKE_SOURCE_DIR}/src/common/assert.cpp
    ${CMAKE_SOURCE_DIR}/src/common/error.cpp
    ${CMAKE_SOURCE_DIR}/src/common/io_file.cpp
    ${CMAKE_SOURCE_DIR}/src/common/string_util.cpp
    ${CMAKE_SOURCE_DIR}/src/common/logging/classes.cpp
    ${CMAKE_SOURCE_DIR}/src/common/logging/log.cpp
    ${CMAKE_SOURCE_DIR}/src/common/logging/log_stats.cpp
    ${CMAKE_SOURCE_DIR}/src/common/profiler.cpp
    ${CMAKE_SOURCE_DIR}/src/common/ntapi.cpp

    # Stubs that replace dependencies
    stubs/common_stub.cpp
    stubs/resource_tracking_pass_stub.cpp
    stubs/scm_rev_stub.cpp
    stubs/sdl_stub.cpp
    stubs/core_stub.cpp
    stubs/signals_stub.cpp
    stubs/file_sys_stub.cpp

    stubs/memory_stub.cpp

    gcn/gcn_test_runner.hpp
    gcn/gcn_test_runner.cpp
    gcn/translator.hpp
    gcn/translator.cpp

    # Tests
    gcn/test_gcn_instructions.cpp
    gcn/test_geometry_inputs.cpp
)

set(CONTENT_ROOTS_TEST_SOURCES
    # Under test
    ${CMAKE_SOURCE_DIR}/src/core/file_sys/fs.cpp
    ${CMAKE_SOURCE_DIR}/src/core/file_sys/ifile.cpp
    ${CMAKE_SOURCE_DIR}/src/core/file_sys/backends/host_fs.cpp
    ${CMAKE_SOURCE_DIR}/src/core/file_sys/backends/zarchive_fs.cpp
    ${CMAKE_SOURCE_DIR}/src/core/file_sys/backends/pkg_fs.cpp
    ${CMAKE_SOURCE_DIR}/src/core/file_format/pkg_reader.cpp
    ${CMAKE_SOURCE_DIR}/src/core/file_format/pkg_type.cpp
    ${CMAKE_SOURCE_DIR}/src/core/file_format/psf.cpp
    ${CMAKE_SOURCE_DIR}/src/core/crypto/crypto.cpp
    ${CMAKE_SOURCE_DIR}/src/core/file_sys/devices/base_device.cpp
    ${CMAKE_SOURCE_DIR}/src/core/file_sys/devices/logger.cpp

    # Minimal common support
    ${CMAKE_SOURCE_DIR}/src/core/emulator_settings.cpp
    ${CMAKE_SOURCE_DIR}/src/core/emulator_state.cpp
    ${CMAKE_SOURCE_DIR}/src/common/io_file.cpp
    ${CMAKE_SOURCE_DIR}/src/common/path_util.cpp
    ${CMAKE_SOURCE_DIR}/src/common/assert.cpp
    ${CMAKE_SOURCE_DIR}/src/common/error.cpp
    ${CMAKE_SOURCE_DIR}/src/common/string_util.cpp
    ${CMAKE_SOURCE_DIR}/src/common/logging/classes.cpp
    ${CMAKE_SOURCE_DIR}/src/common/logging/log.cpp
    ${CMAKE_SOURCE_DIR}/src/common/logging/log_stats.cpp
    ${CMAKE_SOURCE_DIR}/src/common/profiler.cpp

    # Stubs that replace dependencies
    stubs/common_stub.cpp
    stubs/core_stub.cpp
    stubs/signals_stub.cpp
    stubs/scm_rev_stub.cpp
    stubs/sdl_stub.cpp
    stubs/thread_stub.cpp

    # Tests
    test_content_roots.cpp
    test_pkg_filesystem.cpp
)

add_executable(shadps4_settings_test ${SETTINGS_TEST_SOURCES})
add_executable(shadps4_ngs2_test ${NGS2_TEST_SOURCES})
add_executable(shadps4_gcn_test ${GCN_TEST_SOURCES})
add_executable(shadps4_content_roots_test ${CONTENT_ROOTS_TEST_SOURCES})
# Header-only under test: the PCM preparation shared by the Oboe and cubeb backends.
add_executable(shadps4_audio_transfer_test test_audio_transfer.cpp)
# Guest function package format (sdk_version 3: signatures, import hooks) and PLT lookup.
add_executable(shadps4_guest_patch_format_test
    ${CMAKE_SOURCE_DIR}/src/core/host_runtime/guest_patch_format.cpp
    ${CMAKE_SOURCE_DIR}/src/core/host_runtime/guest_patch_relocator.cpp
    test_guest_patch_format.cpp)
target_compile_definitions(shadps4_guest_patch_format_test PRIVATE
    SHADPS4_SOURCE_DIR="${CMAKE_SOURCE_DIR}")

foreach(t ${TEST_TARGETS})
    target_include_directories(${t} PRIVATE
        ${CMAKE_SOURCE_DIR}/src
        ${CMAKE_SOURCE_DIR}
    )
    target_compile_features(${t} PRIVATE cxx_std_23)
    target_compile_definitions(${t} PRIVATE BOOST_ASIO_STANDALONE)
endforeach()

target_link_libraries(shadps4_settings_test PRIVATE
    GTest::gtest_main
    fmt::fmt
    nlohmann_json::nlohmann_json
    toml11::toml11
    SDL3::SDL3
    spatial::foundation_log
)
target_link_libraries(shadps4_ngs2_test PRIVATE
    GTest::gtest_main
    fmt::fmt
    nlohmann_json::nlohmann_json
    toml11::toml11
    SDL3::SDL3
    spatial::foundation_log
)
target_link_libraries(shadps4_gcn_test PRIVATE
    GTest::gtest_main
    xbyak::xbyak
    xxHash::xxhash
    Zydis::Zydis
    fmt::fmt
    nlohmann_json::nlohmann_json
    magic_enum::magic_enum
    toml11::toml11
    Boost::headers
    Vulkan::Headers
    sirit
    SDL3::SDL3
    spatial::foundation_log
    half::half
)

target_link_libraries(shadps4_guest_patch_format_test PRIVATE
    GTest::gtest_main
    Zydis::Zydis
    nlohmann_json::nlohmann_json
)
# SHA-256 of package payloads: the bundled LibreSSL's crypto, or a system one.
foreach(crypto_lib LibreSSL::Crypto OpenSSL::Crypto crypto)
    if (TARGET ${crypto_lib})
        target_link_libraries(shadps4_guest_patch_format_test PRIVATE ${crypto_lib})
        break()
    endif()
endforeach()

target_link_libraries(shadps4_audio_transfer_test PRIVATE
    GTest::gtest_main
    fmt::fmt
    magic_enum::magic_enum
    Boost::headers
)

target_link_libraries(shadps4_content_roots_test PRIVATE
    GTest::gtest_main
    fmt::fmt
    magic_enum::magic_enum
    nlohmann_json::nlohmann_json
    toml11::toml11
    tsl::robin_map
    SDL3::SDL3
    spatial::foundation_log
    ZArchive::zarchive
    ${SSL_DEPENDENCIES}
    ZLIB::ZLIB
)

if (CMAKE_CXX_COMPILER_ID STREQUAL "Clang" OR
    CMAKE_CXX_COMPILER_ID STREQUAL "AppleClang")
    include(CheckCXXSymbolExists)
    check_cxx_symbol_exists(_LIBCPP_VERSION version LIBCPP)
    if (LIBCPP)
        foreach(t ${TEST_TARGETS})
            target_compile_options(${t} PRIVATE -fexperimental-library)
        endforeach()
    endif()
endif()

foreach(t ${TEST_TARGETS})
    if (WIN32)
        target_link_libraries(${t} PRIVATE onecore)
        target_compile_definitions(${t} PRIVATE
            NOMINMAX
            WIN32_LEAN_AND_MEAN
            NTDDI_VERSION=0x0A000006
            _WIN32_WINNT=0x0A00
            WINVER=0x0A00
        )
        if (MSVC)
            target_compile_definitions(${t} PRIVATE
                _CRT_SECURE_NO_WARNINGS
                _CRT_NONSTDC_NO_DEPRECATE
                _SCL_SECURE_NO_WARNINGS
                _TIMESPEC_DEFINED
            )
        endif()
        if (MSVC AND CMAKE_CXX_COMPILER_ID STREQUAL "Clang")
            # Foundation's logging core uses 128-bit division (__udivti3), like shadps4 itself.
            target_link_libraries(${t} PRIVATE clang_rt.builtins-x86_64.lib)
        endif()
    endif()
endforeach()

include(GoogleTest)

foreach(t ${TEST_TARGETS})
    gtest_discover_tests(${t}
        WORKING_DIRECTORY ${CMAKE_BINARY_DIR}
        DISCOVERY_TIMEOUT 60
    )
endforeach()

# ===========================================================================
# HTTP tests (libSceHttp - URI helpers and status-line parsing)
# ===========================================================================
# Self-contained block - mirrors the structure of the targets above so the
# new test does not depend on any earlier foreach iterations.

set(HTTP_TEST_SOURCES
    # Under test
    ${CMAKE_SOURCE_DIR}/src/core/libraries/network/http.cpp
    # Required to link RegisterLib's LIB_FUNCTION calls and the logger's
    # access to EmulatorSettings.
    ${CMAKE_SOURCE_DIR}/src/core/emulator_settings.cpp
    ${CMAKE_SOURCE_DIR}/src/core/emulator_state.cpp

    # Minimal common support
    ${CMAKE_SOURCE_DIR}/src/common/path_util.cpp
    ${CMAKE_SOURCE_DIR}/src/common/assert.cpp
    ${CMAKE_SOURCE_DIR}/src/common/error.cpp
    ${CMAKE_SOURCE_DIR}/src/common/string_util.cpp
    ${CMAKE_SOURCE_DIR}/src/common/logging/classes.cpp
    ${CMAKE_SOURCE_DIR}/src/common/logging/log.cpp
    ${CMAKE_SOURCE_DIR}/src/common/logging/log_stats.cpp
    ${CMAKE_SOURCE_DIR}/src/common/profiler.cpp

    # Stubs that replace dependencies
    stubs/common_stub.cpp
    stubs/scm_rev_stub.cpp
    stubs/sdl_stub.cpp
    stubs/loader_stub.cpp
    stubs/core_stub.cpp
    stubs/signals_stub.cpp
    stubs/file_sys_stub.cpp
    stubs/kernel_stub.cpp

    # Tests
    network/test_http_uri.cpp
    network/test_http_status_line.cpp
    network/test_http_parse_response_header.cpp
    network/test_http_lifecycle.cpp
)

add_executable(shadps4_http_test ${HTTP_TEST_SOURCES})

list(APPEND TEST_TARGETS shadps4_http_test)

target_include_directories(shadps4_http_test PRIVATE
    ${CMAKE_SOURCE_DIR}/src
    ${CMAKE_SOURCE_DIR}
)
target_compile_features(shadps4_http_test PRIVATE cxx_std_23)
target_compile_definitions(shadps4_http_test PRIVATE BOOST_ASIO_STANDALONE)

target_link_libraries(shadps4_http_test PRIVATE
    GTest::gtest_main
    fmt::fmt
    magic_enum::magic_enum
    nlohmann_json::nlohmann_json
    toml11::toml11
    SDL3::SDL3
    spatial::foundation_log
    ZLIB::ZLIB
)

if (CMAKE_CXX_COMPILER_ID STREQUAL "Clang" OR
    CMAKE_CXX_COMPILER_ID STREQUAL "AppleClang")
    include(CheckCXXSymbolExists)
    check_cxx_symbol_exists(_LIBCPP_VERSION version LIBCPP)
    if (LIBCPP)
        target_compile_options(shadps4_http_test PRIVATE -fexperimental-library)
    endif()
endif()

if (WIN32)
    target_link_libraries(shadps4_http_test PRIVATE onecore)
    target_compile_definitions(shadps4_http_test PRIVATE
        NOMINMAX
        WIN32_LEAN_AND_MEAN
        NTDDI_VERSION=0x0A000006
        _WIN32_WINNT=0x0A00
        WINVER=0x0A00
    )
    if (MSVC)
        target_compile_definitions(shadps4_http_test PRIVATE
            _CRT_SECURE_NO_WARNINGS
            _CRT_NONSTDC_NO_DEPRECATE
            _SCL_SECURE_NO_WARNINGS
            _TIMESPEC_DEFINED
        )
    endif()
endif()

gtest_discover_tests(shadps4_http_test
    WORKING_DIRECTORY ${CMAKE_BINARY_DIR}
    DISCOVERY_TIMEOUT 60
)

# Standalone-capable WSI contract checks, no GPU or full emulator link required.
add_subdirectory(video_core)

# COMMON libc and ARM64 signal-context regressions.
add_subdirectory(common)

# Standalone package reader / verification tool (does not start a guest).
add_executable(shadps4_pkg_read
    ${CMAKE_SOURCE_DIR}/tools/pkg_read_cli.cpp
    ${CMAKE_SOURCE_DIR}/src/core/file_format/pkg_reader.cpp
    ${CMAKE_SOURCE_DIR}/src/core/file_format/pkg_type.cpp
    ${CMAKE_SOURCE_DIR}/src/core/crypto/crypto.cpp)
target_include_directories(shadps4_pkg_read PRIVATE ${CMAKE_SOURCE_DIR}/src)
target_link_libraries(shadps4_pkg_read PRIVATE nlohmann_json::nlohmann_json ${SSL_DEPENDENCIES} ZLIB::ZLIB)
