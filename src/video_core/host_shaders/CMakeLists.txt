# SPDX-FileCopyrightText: Copyright 2023 Citra Emulator Project
# SPDX-License-Identifier: GPL-2.0-or-later

# Host shaders are compiled to SPIR-V at build time. Cross builds (Android) cannot run the
# glslang they build, so they take a host-built one (scripts/android/build-host-android).
set(HOST_SHADER_COMPILER "" CACHE FILEPATH "glslang executable that runs on the build machine")
if (HOST_SHADER_COMPILER)
    if (NOT EXISTS "${HOST_SHADER_COMPILER}")
        message(FATAL_ERROR "Missing host shader compiler: ${HOST_SHADER_COMPILER}")
    endif()
    set(GLSLANG_COMPILER "${HOST_SHADER_COMPILER}")
elseif (CMAKE_CROSSCOMPILING)
    message(FATAL_ERROR "Cross builds require HOST_SHADER_COMPILER; use scripts/android/build-host-android")
elseif (TARGET glslang-standalone)
    set(GLSLANG_COMPILER glslang-standalone)
elseif (TARGET glslang::glslang-standalone)
    set(GLSLANG_COMPILER glslang::glslang-standalone)
else()
    message(FATAL_ERROR "No glslang executable to build host shaders with")
endif()

set(SHADER_INCLUDE ${CMAKE_CURRENT_BINARY_DIR}/include)
set(SHADER_DIR ${SHADER_INCLUDE}/video_core/host_shaders)
set(HOST_SHADERS_INCLUDE ${SHADER_INCLUDE} PARENT_SCOPE)

set(SHADER_FILES "")

function(add_host_shader FILENAME SHADER_NAME)
    set(SOURCE_HEADER_FILE ${SHADER_DIR}/${SHADER_NAME}.h)
    set(DEPFILE ${SOURCE_HEADER_FILE}.d)
    string(TOUPPER ${SHADER_NAME} SYMBOL)
    set(SHADER_DEFINES ${ARGN})
    list(TRANSFORM SHADER_DEFINES PREPEND "-D")
    add_custom_command(
        OUTPUT ${SOURCE_HEADER_FILE}
        COMMAND ${CMAKE_COMMAND} -E make_directory ${SHADER_DIR}
        COMMAND ${GLSLANG_COMPILER} --target-env vulkan1.3
                -o ${SOURCE_HEADER_FILE} --vn ${SYMBOL} --depfile ${DEPFILE}
                ${SHADER_DEFINES} ${CMAKE_CURRENT_SOURCE_DIR}/${FILENAME}
        COMMAND ${CMAKE_COMMAND} "-DHEADER_FILE=${SOURCE_HEADER_FILE}"
                -P ${CMAKE_CURRENT_SOURCE_DIR}/AddShaderHeaderInclude.cmake
        DEPENDS ${GLSLANG_COMPILER} ${CMAKE_CURRENT_SOURCE_DIR}/${FILENAME}
                ${CMAKE_CURRENT_SOURCE_DIR}/AddShaderHeaderInclude.cmake
        DEPFILE ${DEPFILE}
        BYPRODUCTS ${DEPFILE}
        COMMENT "Building host shader ${FILENAME} -> ${SOURCE_HEADER_FILE}"
        VERBATIM
    )
    set(SHADER_HEADERS ${SHADER_HEADERS} ${SOURCE_HEADER_FILE} PARENT_SCOPE)
    set(SHADER_FILES ${SHADER_FILES} ${FILENAME} PARENT_SCOPE)
endfunction()

add_host_shader(color_to_ms_depth.frag color_to_ms_depth_frag)
add_host_shader(ms_image_blit.frag ms_image_blit_frag)
add_host_shader(ms_image_blit.frag ms_image_blit_msaa_frag SRC_MSAA=1)
add_host_shader(fault_buffer_process.comp fault_buffer_process_comp)
add_host_shader(fs_tri.vert fs_tri_vert)
add_host_shader(fsr.comp fsr_easu_comp SAMPLE_EASU=1)
add_host_shader(fsr.comp fsr_rcas_comp SAMPLE_RCAS=1)
add_host_shader(post_process.frag post_process_frag)

foreach(BPP IN ITEMS 8 16 32 64 96 128)
    add_host_shader(tiling.comp tiling_micro_${BPP}_comp BITS_PER_PIXEL=${BPP} IS_MACRO_TILED=0)
    add_host_shader(tiling.comp tiling_macro_${BPP}_comp BITS_PER_PIXEL=${BPP} IS_MACRO_TILED=1)
endforeach()
# Fused readback tilers (sampled image in, packing as a specialization constant).
foreach(BPP IN ITEMS 8 16 32 64 128)
    add_host_shader(tiling.comp tiling_image_micro_${BPP}_comp BITS_PER_PIXEL=${BPP}
                    IS_MACRO_TILED=0 FROM_IMAGE=1)
    add_host_shader(tiling.comp tiling_image_macro_${BPP}_comp BITS_PER_PIXEL=${BPP}
                    IS_MACRO_TILED=1 FROM_IMAGE=1)
endforeach()

list(REMOVE_DUPLICATES SHADER_FILES)
add_custom_target(host_shaders DEPENDS ${SHADER_HEADERS} SOURCES ${SHADER_FILES})
