# SPDX-FileCopyrightText: Copyright 2026 shadPS4 Emulator Project
# SPDX-License-Identifier: GPL-2.0-or-later

# Build-speed controls shared by the desktop and Android host builds. Include this
# after project() and before add_subdirectory(externals) so the compiler launcher
# and LTO setting also reach third-party targets.

option(ENABLE_UNITY_BUILD "Compile shadPS4's own sources in unity batches" ON)
set(SHADPS4_UNITY_BATCH_SIZE 16 CACHE STRING "Source files per unity batch for shadPS4's own targets")
option(ENABLE_LTO "Enable link-time optimization (slow links; keep off for development builds)" OFF)
option(ENABLE_CCACHE "Use ccache as the compiler launcher when it is installed" ON)

set(CMAKE_INTERPROCEDURAL_OPTIMIZATION ${ENABLE_LTO})

if(ENABLE_CCACHE AND NOT CMAKE_CXX_COMPILER_LAUNCHER)
    find_program(CCACHE_PROGRAM ccache)
    if(CCACHE_PROGRAM)
        set(CMAKE_C_COMPILER_LAUNCHER "${CCACHE_PROGRAM}")
        set(CMAKE_CXX_COMPILER_LAUNCHER "${CCACHE_PROGRAM}")
        if(MSVC)
            # ccache cannot cache /Zi (shared PDB); embed CodeView in each object instead.
            foreach(lang C CXX)
                foreach(config DEBUG RELWITHDEBINFO)
                    string(REPLACE "/Zi" "/Z7" CMAKE_${lang}_FLAGS_${config} "${CMAKE_${lang}_FLAGS_${config}}")
                endforeach()
            endforeach()
        endif()
        message(STATUS "ccache: ${CCACHE_PROGRAM}")
    else()
        message(STATUS "ccache: not found, compiling without a launcher")
    endif()
endif()

# Sources that must be compiled on their own. Besides these, every source is
# scanned at configure time and kept out of unity batches when it
#  - has a using-directive at file/namespace scope (it would leak into the
#    following files of the batch, e.g. `using namespace ImGui;` makes `Core`
#    ambiguous),
#  - includes Foundation headers directly (they define or #undef generic macros
#    such as far/near/FAR that break later Windows SDK headers), or
#  - on Windows, reaches the Windows SDK through its includes: windows.h leaks
#    macros such as DELETE, WAIT_TIMEOUT and ERROR into the following files.
#    Shared headers must therefore keep <windows.h> out and leave it to .cpp files.
set(SHADPS4_UNITY_EXCLUDE
    # File-local constants/helpers with the same name as a sibling source.
    src/core/libraries/save_data/save_instance.cpp    # sce_sys (save_backup.cpp)
    src/core/libraries/save_data/save_memory.cpp      # sce_sys (save_backup.cpp)
    src/core/libraries/network/unix_sockets.cpp       # ConvertLevels/ConvertReturnErrorCode (posix_sockets.cpp)
    src/core/libraries/avplayer/avplayer_handle_streamer.cpp  # AVPLAYER_AVIO_BUFFER_SIZE (avplayer_file_streamer.cpp)
    src/core/libraries/np/np_matching2/np_matching2_mm.cpp         # OnlineIdToString, IpStringToAddr
    src/core/libraries/np/np_matching2/np_matching2_signaling.cpp  # OnlineIdToString (np_signaling.cpp)
    src/core/libraries/np/np_signaling/np_signaling_state.cpp      # OnlineIdToString (np_signaling.cpp)
    src/core/libraries/hmd/hmd_distortion.cpp         # g_library_initialized (hmd.cpp)
    src/core/libraries/move/move.cpp                  # g_library_initialized (hmd.cpp)
    src/core/libraries/vr_tracker/vr_tracker.cpp      # g_library_initialized (hmd.cpp)
    src/core/libraries/audio/openal_audio_out.cpp     # VOLUME_* (sdl_audio_out.cpp)
    src/shader_recompiler/backend/spirv/emit_spirv_quad_rect.cpp  # SPIRV_VERSION_1_5 (emit_spirv_discard_frag.cpp)
    src/core/host_runtime/guest_commerce_dialog.cpp   # Code/ReadValue/Zero (guest_msg_dialog.cpp)
    src/shader_recompiler/ir/passes/inverse_ballot_elimination_pass.cpp  # FoldCompositeConstruct/FoldInverseFunc (constant_propagation_pass.cpp)
    # Order-dependent sources.
    src/video_core/renderer_vulkan/vk_platform.cpp    # VK_USE_PLATFORM_* must precede the first Vulkan include
    src/shader_recompiler/ir/ir_emitter.cpp           # explicit specializations must precede first instantiation
)

# Includes that bring in the Windows SDK: its own headers plus third-party headers
# that include winsock/windows.h themselves.
set(_shadps4_windows_include_regex
    "#[ \t]*include[ \t]*[<\"]([Ww]indows|[Ww]in[Ss]ock2?|[Ww][Ss]2tcpip|[Ii]phlpapi|[Ss]hl[Oo]bj|[Ss]hellapi|[Dd]bg[Hh]elp|[Pp]sapi|winternl|afunix|[Mm][Ss][Ww][Ss]ock|[Ww]indef|[Ww]innt)\\.h[>\"]|#[ \t]*include[ \t]*[<\"](boost/asio|httplib\\.h|miniupnpc/|libusb)")

# Sets `out` to the subset of the given files that reach the Windows SDK through their
# includes. Project headers are followed whether spelled with "" or <>, when they resolve
# next to the including file, under src/ or under the repository root. The include graph
# is built first and the flag then propagated from SDK-including files to their includers,
# so cycles cannot hide a path.
function(_shadps4_windows_reaching out)
    set(queue ${ARGN})
    set(direct)
    while(queue)
        list(POP_FRONT queue file)
        get_property(visited GLOBAL PROPERTY "_shadps4_seen:${file}" SET)
        if(visited)
            continue()
        endif()
        set_property(GLOBAL PROPERTY "_shadps4_seen:${file}" TRUE)
        file(STRINGS "${file}" includes REGEX "^[ \t]*#[ \t]*include")
        get_filename_component(dir "${file}" DIRECTORY)
        foreach(line IN LISTS includes)
            if(line MATCHES "${_shadps4_windows_include_regex}")
                list(APPEND direct "${file}")
                continue()
            endif()
            if(NOT line MATCHES "#[ \t]*include[ \t]*[<\"]([^>\"]+)[>\"]")
                continue()
            endif()
            set(name "${CMAKE_MATCH_1}")
            foreach(base "${dir}" "${PROJECT_SOURCE_DIR}/src" "${PROJECT_SOURCE_DIR}")
                if(EXISTS "${base}/${name}" AND NOT IS_DIRECTORY "${base}/${name}")
                    cmake_path(ABSOLUTE_PATH name BASE_DIRECTORY "${base}" NORMALIZE OUTPUT_VARIABLE header)
                    set_property(GLOBAL APPEND PROPERTY "_shadps4_parents:${header}" "${file}")
                    list(APPEND queue "${header}")
                    break()
                endif()
            endforeach()
        endforeach()
    endwhile()

    set(queue ${direct})
    while(queue)
        list(POP_FRONT queue file)
        get_property(marked GLOBAL PROPERTY "_shadps4_win:${file}" SET)
        if(marked)
            continue()
        endif()
        set_property(GLOBAL PROPERTY "_shadps4_win:${file}" TRUE)
        get_property(parents GLOBAL PROPERTY "_shadps4_parents:${file}")
        list(APPEND queue ${parents})
    endwhile()

    set(result)
    foreach(file IN LISTS ARGN)
        get_property(marked GLOBAL PROPERTY "_shadps4_win:${file}" SET)
        if(marked)
            list(APPEND result "${file}")
        endif()
    endforeach()
    set(${out} ${result} PARENT_SCOPE)
endfunction()

function(shadps4_enable_unity_build target)
    if(NOT ENABLE_UNITY_BUILD)
        return()
    endif()
    set_target_properties(${target} PROPERTIES
        UNITY_BUILD ON
        UNITY_BUILD_BATCH_SIZE ${SHADPS4_UNITY_BATCH_SIZE}
        # Foundation's utils/debug.h (pulled in through vk_instance.h) defines an
        # assert_invariant() macro that collides with nlohmann::json's member of the
        # same name when json.hpp is first included by a later file of the batch.
        UNITY_BUILD_CODE_AFTER_INCLUDE "#undef assert_invariant")

    get_target_property(sources ${target} SOURCES)
    get_target_property(source_dir ${target} SOURCE_DIR)
    set(skip)
    foreach(source IN LISTS SHADPS4_UNITY_EXCLUDE)
        cmake_path(ABSOLUTE_PATH source BASE_DIRECTORY "${source_dir}" NORMALIZE OUTPUT_VARIABLE path)
        list(APPEND skip "${path}")
    endforeach()
    set(candidates)
    foreach(source IN LISTS sources)
        if(source MATCHES "^\\$<" OR NOT source MATCHES "\\.(cpp|cc|cxx)$")
            continue()
        endif()
        cmake_path(ABSOLUTE_PATH source BASE_DIRECTORY "${source_dir}" NORMALIZE OUTPUT_VARIABLE path)
        if(NOT EXISTS "${path}")
            continue()
        endif()
        file(STRINGS "${path}" leaking
            REGEX "^(using namespace |#include [<\"](spatial|utils|foundation|debugbus|profiler_ring)/)")
        if(leaking)
            list(APPEND skip "${path}")
        else()
            list(APPEND candidates "${path}")
        endif()
    endforeach()
    set(windows_sources)
    if(WIN32)
        _shadps4_windows_reaching(windows_sources ${candidates})
        list(APPEND skip ${windows_sources})
    endif()
    list(LENGTH windows_sources windows_sources)
    list(REMOVE_DUPLICATES skip)
    list(LENGTH skip skipped)
    message(STATUS "Unity build: ${target} batches of ${SHADPS4_UNITY_BATCH_SIZE}, "
                   "${skipped} sources compiled separately (${windows_sources} reach the Windows SDK)")
    set_source_files_properties(${skip}
        TARGET_DIRECTORY ${target}
        PROPERTIES SKIP_UNITY_BUILD_INCLUSION ON)
endfunction()
