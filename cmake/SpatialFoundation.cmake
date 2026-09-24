# A deliberately small host profile. Reflection/packing and NetSystem have a
# wider dependency closure; see docs/foundation-integration.md before enabling them.
include_guard(GLOBAL)

function(shadps4_add_foundation)
    if(TARGET shadps4_foundation)
        return()
    endif()

    set(foundation_root "${CMAKE_CURRENT_FUNCTION_LIST_DIR}/../foundation")
    if(NOT EXISTS "${foundation_root}/modules/debugbus/CMakeLists.txt")
        message(FATAL_ERROR "Foundation is missing. Run: git submodule update --init foundation")
    endif()

    # Function scope keeps this profile from changing another consumer's options.
    # Foundation exposes these as cache options. Force the parent integration
    # to keep the optional transports disabled unless they are wired explicitly.
    set(FOUNDATION_DEBUGBUS_BUILD_TCP OFF CACHE BOOL "" FORCE)
    set(FOUNDATION_DEBUGBUS_BUILD_PROFILER OFF CACHE BOOL "" FORCE)
    set(CMAKE_POSITION_INDEPENDENT_CODE ON)
    # The parent uses only the registry/dumpsys part of debugbus.  The current
    # Foundation debugbus CMake still declares optional TCP/profiler targets
    # unconditionally; provide empty dependency targets so those unused
    # libraries remain out of this host profile without pulling their complete
    # module graphs into shadPS4.
    if(NOT TARGET spatial::foundation_module_network)
        add_library(foundation_module_network INTERFACE)
        add_library(spatial::foundation_module_network ALIAS foundation_module_network)
    endif()
    if(NOT TARGET spatial::foundation_profiler)
        add_library(foundation_profiler INTERFACE)
        add_library(spatial::foundation_profiler ALIAS foundation_profiler)
    endif()
    add_subdirectory("${foundation_root}/modules/debugbus"
                     "${CMAKE_CURRENT_BINARY_DIR}/foundation/debugbus" EXCLUDE_FROM_ALL)

    add_library(shadps4_foundation INTERFACE)
    add_library(shadps4::foundation ALIAS shadps4_foundation)
    target_link_libraries(shadps4_foundation INTERFACE spatial::foundation_debugbus)

    # The FDM module is graphics-only and its upstream CMake target pulls in
    # Foundation's full basic/core graph.  shadPS4 already owns those host
    # services, so keep the integration narrow: compile the two FDM sources
    # against the public math/platform headers and route Foundation diagnostics
    # through the emulator logger (see foundation_foveation_log.cpp).
    add_library(shadps4_foundation_foveation STATIC
        "${foundation_root}/modules/foveation/src/Foveation.cpp"
        "${foundation_root}/modules/foveation/src/vulkan/FragmentDensityMap.cpp"
    )
    target_include_directories(shadps4_foundation_foveation
        PUBLIC
            "${foundation_root}/modules/foveation/include"
            "${foundation_root}/basic/underlying/core/public"
            "${foundation_root}/basic/underlying/math/public"
            "${foundation_root}/basic/platform/public"
            "${foundation_root}/modules/utils/include"
    )
    target_link_libraries(shadps4_foundation_foveation
        PUBLIC Vulkan::Headers fmt::fmt
    )
    target_compile_features(shadps4_foundation_foveation PUBLIC cxx_std_20)
    target_compile_definitions(shadps4_foundation_foveation PRIVATE VULKAN_HPP_NO_TO_STRING)
    set_target_properties(shadps4_foundation_foveation PROPERTIES UNITY_BUILD OFF)
    target_link_libraries(shadps4_foundation INTERFACE shadps4_foundation_foveation)
    add_subdirectory("${foundation_root}/modules/texture_codec"
                     "${CMAKE_CURRENT_BINARY_DIR}/foundation/texture_codec" EXCLUDE_FROM_ALL)
    target_link_libraries(shadps4_foundation INTERFACE spatial::foundation_texture_codec)
    if(ANDROID)
        add_subdirectory("${foundation_root}/modules/capture"
                         "${CMAKE_CURRENT_BINARY_DIR}/foundation/capture" EXCLUDE_FROM_ALL)
        # Oboe stays an independently pinned dependency. Foundation consumes
        # its target, without vendoring another copy.
        if(NOT TARGET oboe)
            set(CMAKE_POLICY_VERSION_MINIMUM 3.5)
            add_subdirectory("${foundation_root}/../externals/oboe"
                             "${CMAKE_CURRENT_BINARY_DIR}/externals/oboe" EXCLUDE_FROM_ALL)
        endif()
        add_subdirectory("${foundation_root}/modules/audio"
                         "${CMAKE_CURRENT_BINARY_DIR}/foundation/audio" EXCLUDE_FROM_ALL)
        target_link_libraries(shadps4_foundation INTERFACE spatial::foundation_audio)
        target_link_libraries(shadps4_foundation INTERFACE spatial::foundation_debugbus_dumpsys)
        add_subdirectory("${foundation_root}/modules/profiler_ring"
                         "${CMAKE_CURRENT_BINARY_DIR}/foundation/profiler_ring" EXCLUDE_FROM_ALL)
        target_link_libraries(shadps4_foundation INTERFACE spatial::foundation_profiler_ring)
        target_compile_definitions(shadps4_foundation INTERFACE SHADPS4_PROFILER_RING=1)
    endif()
endfunction()
