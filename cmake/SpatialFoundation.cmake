# Host profile: shared basic services and graphics modules, plus Lite Engine for
# the Android XR status scene. See
# docs/foundation-integration.md for the dependency closure of each part.
include_guard(GLOBAL)

function(shadps4_add_foundation)
    if(TARGET shadps4_foundation)
        return()
    endif()

    set(foundation_root "${CMAKE_CURRENT_FUNCTION_LIST_DIR}/../foundation")
    if(NOT EXISTS "${foundation_root}/modules/debugbus/CMakeLists.txt")
        message(FATAL_ERROR "Foundation is missing. Run: git submodule update --init foundation")
    endif()

    set(CMAKE_POSITION_INDEPENDENT_CODE ON)
    # Foundation targets read the host's unity switch under this name.
    set(CITRA_USE_UNITY_BUILD ${ENABLE_UNITY_BUILD})

    # The desktop DebugBus TCP transport (spatial::foundation_debugbus_tcp) runs on
    # Foundation's NetSystemModule. Lite Engine uses the same basic services on
    # Android, where commands remain accessible through dumpsys. These are
    # libraries, not a second module-runtime/JNI initialization; JniHelper's
    # environment target remains shared with the existing host entry point.
    include("${foundation_root}/cmake/FoundationSubdirectory.cmake")
    include("${foundation_root}/cmake/FoundationHostDependencies.cmake")
    foreach(layer third_party modules/property basic)
        add_subdirectory("${foundation_root}/${layer}"
                         "${CMAKE_CURRENT_BINARY_DIR}/foundation/${layer}" EXCLUDE_FROM_ALL)
    endforeach()
    add_subdirectory("${foundation_root}/modules/debugbus"
                     "${CMAKE_CURRENT_BINARY_DIR}/foundation/debugbus" EXCLUDE_FROM_ALL)
    # Logging (common/logging) shares Foundation's LogModule with the engine.
    add_subdirectory("${foundation_root}/modules/log"
                     "${CMAKE_CURRENT_BINARY_DIR}/foundation/log" EXCLUDE_FROM_ALL)

    add_library(shadps4_foundation INTERFACE)
    add_library(shadps4::foundation ALIAS shadps4_foundation)
    target_link_libraries(shadps4_foundation INTERFACE spatial::foundation_debugbus spatial::foundation_log)
    if(NOT ANDROID AND TARGET spatial::foundation_debugbus_tcp)
        target_link_libraries(shadps4_foundation INTERFACE spatial::foundation_debugbus_tcp)
        target_compile_definitions(shadps4_foundation INTERFACE SHADPS4_DEBUGBUS_TCP=1)
    endif()

    add_subdirectory("${foundation_root}/modules/fsr1"
                     "${CMAKE_CURRENT_BINARY_DIR}/foundation/fsr1" EXCLUDE_FROM_ALL)
    target_link_libraries(shadps4_foundation INTERFACE spatial::foundation_fsr1)

    # The FDM module is graphics-only and its upstream CMake target pulls in
    # Foundation's full basic/core graph.  shadPS4 already owns those host
    # services, so keep the integration narrow: compile the two FDM sources
    # against the public math/platform headers; their diagnostics use a
    # Foundation logger kind.
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
        PUBLIC Vulkan::Headers fmt::fmt spatial::foundation_log
    )
    target_compile_features(shadps4_foundation_foveation PUBLIC cxx_std_20)
    target_compile_definitions(shadps4_foundation_foveation PRIVATE VULKAN_HPP_NO_TO_STRING)
    set_target_properties(shadps4_foundation_foveation PROPERTIES UNITY_BUILD OFF)
    target_link_libraries(shadps4_foundation INTERFACE shadps4_foundation_foveation)
    add_library(spatial::foundation_foveation_vulkan ALIAS shadps4_foundation_foveation)
    add_library(foundation_foveation_vulkan ALIAS shadps4_foundation_foveation)
    if(ANDROID)
        set(FOUNDATION_RENDER_VMA_IMPLEMENTATION OFF CACHE BOOL "" FORCE)
        add_subdirectory("${foundation_root}/engine"
                         "${CMAKE_CURRENT_BINARY_DIR}/foundation/engine" EXCLUDE_FROM_ALL)
        add_library(shadps4_xr_scene STATIC
            "${foundation_root}/modules/xr/src/XrSceneVulkanLayer.cpp"
            "${foundation_root}/modules/xr/src/XrCamera.cpp")
        target_include_directories(shadps4_xr_scene PUBLIC
            "${foundation_root}/modules/xr/include"
            "${foundation_root}/third_party/openxr/openxr_header")
        target_link_libraries(shadps4_xr_scene PUBLIC spatial::foundation_engine
            spatial::foundation_async_task Dear_ImGui)
        target_compile_features(shadps4_xr_scene PUBLIC cxx_std_20)
        set_target_properties(shadps4_xr_scene PROPERTIES UNITY_BUILD OFF)
    endif()
    add_subdirectory("${foundation_root}/modules/upscale"
                     "${CMAKE_CURRENT_BINARY_DIR}/foundation/upscale" EXCLUDE_FROM_ALL)
    target_link_libraries(shadps4_foundation INTERFACE spatial::foundation_upscale)
    add_subdirectory("${foundation_root}/modules/texture_codec"
                     "${CMAKE_CURRENT_BINARY_DIR}/foundation/texture_codec" EXCLUDE_FROM_ALL)
    target_link_libraries(shadps4_foundation INTERFACE spatial::foundation_texture_codec)
    # Use the renderer's ImGui ABI/context. The shell-only profile consumes copied
    # Property models and does not pull in Foundation's platform/allocator graph.
    set(FOUNDATION_OVERLAY_HOST_IMGUI_TARGET Dear_ImGui CACHE STRING "" FORCE)
    set(SPATIAL_BUILD_OVERLAY_EXAMPLE OFF CACHE BOOL "" FORCE)
    add_subdirectory("${foundation_root}/modules/imgui_overlay"
                     "${CMAKE_CURRENT_BINARY_DIR}/foundation/imgui_overlay" EXCLUDE_FROM_ALL)
    target_compile_definitions(foundation_imgui_overlay PRIVATE
        IMGUI_USER_CONFIG="${CMAKE_SOURCE_DIR}/src/imgui/imgui_config.h")
    target_link_libraries(shadps4_foundation INTERFACE spatial::foundation_imgui_overlay)
    # One ImGui Vulkan renderer for the presenter and the XR layers, built against the same ImGui
    # ABI (ImDrawIdx, ImTextureID) as the rest of shadPS4.
    set(FOUNDATION_IMGUI_VULKAN_HOST_IMGUI_TARGET Dear_ImGui CACHE STRING "" FORCE)
    add_subdirectory("${foundation_root}/modules/imgui_vulkan"
                     "${CMAKE_CURRENT_BINARY_DIR}/foundation/imgui_vulkan" EXCLUDE_FROM_ALL)
    target_compile_definitions(foundation_imgui_vulkan_renderer PRIVATE
        IMGUI_USER_CONFIG="${CMAKE_SOURCE_DIR}/src/imgui/imgui_config.h")
    target_link_libraries(shadps4_foundation INTERFACE spatial::foundation_imgui_vulkan_renderer)
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
    endif()
    # Litep ring capture (DebugBus profiler_ring / profiler_capture), on every platform: the SDK
    # ring and LiteTrace only, without the Foundation module runtime.
    add_subdirectory("${foundation_root}/modules/profiler_ring"
                     "${CMAKE_CURRENT_BINARY_DIR}/foundation/profiler_ring" EXCLUDE_FROM_ALL)
    target_link_libraries(shadps4_foundation INTERFACE spatial::foundation_profiler_ring)
    target_compile_definitions(shadps4_foundation INTERFACE SHADPS4_PROFILER_RING=1)
endfunction()
