cmake_minimum_required(VERSION 3.22)
project(shadps4_overlay_probe LANGUAGES CXX)
set(CMAKE_CXX_STANDARD 23)
get_filename_component(ROOT "../.." ABSOLUTE)
find_package(fmt QUIET)
if (NOT TARGET fmt::fmt)
    # Header-only from the pinned submodule when no package is installed (Windows).
    add_library(fmt::fmt INTERFACE IMPORTED)
    target_include_directories(fmt::fmt INTERFACE ${ROOT}/externals/fmt/include)
    target_compile_definitions(fmt::fmt INTERFACE FMT_HEADER_ONLY)
endif()
if (NOT TARGET Vulkan::Headers)
    add_library(Vulkan::Headers INTERFACE IMPORTED)
    target_include_directories(Vulkan::Headers INTERFACE ${ROOT}/externals/vulkan-headers/include)
endif()
add_library(probe_imgui STATIC
    ${ROOT}/foundation/third_party/imgui/imgui.cpp
    ${ROOT}/foundation/third_party/imgui/imgui_draw.cpp
    ${ROOT}/foundation/third_party/imgui/imgui_widgets.cpp
    ${ROOT}/foundation/third_party/imgui/imgui_tables.cpp)
target_include_directories(probe_imgui PUBLIC ${ROOT}/foundation/third_party/imgui ${ROOT}/src)
target_compile_definitions(probe_imgui PUBLIC IMGUI_USER_CONFIG="imgui/imgui_config.h")
set(FOUNDATION_OVERLAY_HOST_IMGUI_TARGET probe_imgui CACHE STRING "" FORCE)
set(SPATIAL_BUILD_OVERLAY_EXAMPLE OFF CACHE BOOL "" FORCE)
add_subdirectory(${ROOT}/foundation/modules/imgui_overlay overlay)
set(FOUNDATION_IMGUI_VULKAN_HOST_IMGUI_TARGET probe_imgui CACHE STRING "" FORCE)
add_subdirectory(${ROOT}/foundation/modules/imgui_vulkan imgui_vulkan)
add_executable(overlay_vulkan_probe overlay_vulkan_probe.cpp)
target_include_directories(overlay_vulkan_probe PRIVATE ${ROOT}/externals/vulkan-headers/include)
target_link_libraries(overlay_vulkan_probe PRIVATE probe_imgui foundation_imgui_overlay
    spatial::foundation_imgui_vulkan_renderer fmt::fmt ${CMAKE_DL_LIBS})
# As on the shadps4 target: vk_common.h expects the beta enumerants in every Vulkan header it meets.
target_compile_definitions(overlay_vulkan_probe PRIVATE VK_ENABLE_BETA_EXTENSIONS)
add_executable(overlay_tooltips_test overlay_tooltips_test.cpp ${ROOT}/src/imgui/runtime_tooltips.cpp)
target_link_libraries(overlay_tooltips_test PRIVATE probe_imgui fmt::fmt)
