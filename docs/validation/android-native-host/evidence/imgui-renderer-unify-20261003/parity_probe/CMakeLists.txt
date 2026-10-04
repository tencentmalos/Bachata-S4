cmake_minimum_required(VERSION 3.22)
project(imgui_renderer_parity LANGUAGES CXX)
set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(ROOT "D:/workspace/shadps4")
set(FOUNDATION "${ROOT}/foundation")

add_library(probe_imgui STATIC
    ${FOUNDATION}/third_party/imgui/imgui.cpp
    ${FOUNDATION}/third_party/imgui/imgui_draw.cpp
    ${FOUNDATION}/third_party/imgui/imgui_widgets.cpp
    ${FOUNDATION}/third_party/imgui/imgui_tables.cpp)
# The reference backend predates the new ImTextureID: keep the previous config for this probe.
target_include_directories(probe_imgui PUBLIC ${FOUNDATION}/third_party/imgui ${CMAKE_CURRENT_SOURCE_DIR}/probe_config ${ROOT}/src)
target_compile_definitions(probe_imgui PUBLIC IMGUI_USER_CONFIG="imgui/imgui_config.h")

add_library(Vulkan::Headers INTERFACE IMPORTED)
target_include_directories(Vulkan::Headers INTERFACE ${ROOT}/externals/vulkan-headers/include)

set(FOUNDATION_IMGUI_VULKAN_HOST_IMGUI_TARGET probe_imgui CACHE STRING "" FORCE)
add_subdirectory(${FOUNDATION}/modules/imgui_vulkan imgui_vulkan)

add_executable(parity_probe parity_probe.cpp old_backend/imgui_impl_vulkan.cpp
    ${FOUNDATION}/third_party/imgui/backends/imgui_impl_vulkan.cpp)
target_include_directories(parity_probe PRIVATE old_backend ${CMAKE_CURRENT_SOURCE_DIR}/probe_config ${ROOT}/src ${ROOT}/externals/fmt/include)
target_compile_definitions(parity_probe PRIVATE FMT_HEADER_ONLY NOMINMAX IMGUI_IMPL_VULKAN_NO_PROTOTYPES)
file(MAKE_DIRECTORY ${CMAKE_BINARY_DIR}/upstream_include/upstream)
configure_file(${FOUNDATION}/third_party/imgui/backends/imgui_impl_vulkan.h ${CMAKE_BINARY_DIR}/upstream_include/upstream/imgui_impl_vulkan.h COPYONLY)
target_include_directories(parity_probe PRIVATE ${CMAKE_BINARY_DIR}/upstream_include)
target_link_libraries(parity_probe PRIVATE probe_imgui spatial::foundation_imgui_vulkan_renderer
    Vulkan::Headers)
if (MSVC)
    target_compile_options(parity_probe PRIVATE /utf-8)
endif()
