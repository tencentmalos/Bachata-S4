# SPDX-FileCopyrightText: Copyright 2024 shadPS4 Emulator Project
# SPDX-License-Identifier: GPL-2.0-or-later

project(ImGui_Resources)

# The font-embed tool must run on the BUILD host. When cross-compiling (e.g. the
# Android NDK host library) a target-built executable cannot run, so accept a
# host-built one via IMGUI_FONT_EMBED_EXECUTABLE (matching the reference Android
# build). Only build the tool from source in a native build.
if(IMGUI_FONT_EMBED_EXECUTABLE)
    if(NOT EXISTS "${IMGUI_FONT_EMBED_EXECUTABLE}")
        message(FATAL_ERROR "Missing host font embed tool: ${IMGUI_FONT_EMBED_EXECUTABLE}")
    endif()
    set(FONT_EMBED_COMMAND "${IMGUI_FONT_EMBED_EXECUTABLE}")
    set(FONT_EMBED_DEP "${IMGUI_FONT_EMBED_EXECUTABLE}")
else()
    if(CMAKE_CROSSCOMPILING)
        message(FATAL_ERROR "Cross builds require IMGUI_FONT_EMBED_EXECUTABLE; use scripts/android/build-host-android")
    endif()
    add_executable(Dear_ImGui_FontEmbed ${CMAKE_SOURCE_DIR}/externals/imgui/misc/fonts/binary_to_compressed_c.cpp)
    set(FONT_EMBED_COMMAND "$<TARGET_FILE:Dear_ImGui_FontEmbed>")
    set(FONT_EMBED_DEP Dear_ImGui_FontEmbed)
endif()

set(FONT_LIST
    NotoSansArabic-Regular.ttf
    NotoSansCJK-Regular.ttc
    NotoSans-Regular.ttf
    NotoSansSymbols2-Regular.ttf
    NotoSansThai-Regular.ttf
    ProggyVector-Regular.ttf
)

set(OutputList "")
FOREACH (FONT_FILE ${FONT_LIST})
    string(REGEX REPLACE "-" "_" fontname ${FONT_FILE})
    string(TOLOWER ${fontname} fontname)
    string(REGEX REPLACE "\\.(ttf|otf|ttc)$" "" fontname_cpp ${fontname})
    set(fontname_cpp "imgui_font_${fontname_cpp}")

    MESSAGE(STATUS "Embedding font ${FONT_FILE}")
    set(OUTPUT "generated_fonts/imgui_fonts/${fontname}")
    add_custom_command(
        OUTPUT "${OUTPUT}.g.cpp"
        COMMAND ${CMAKE_COMMAND} -E make_directory "generated_fonts/imgui_fonts"
        COMMAND ${FONT_EMBED_COMMAND} -nostatic "${CMAKE_CURRENT_SOURCE_DIR}/fonts/${FONT_FILE}" ${fontname_cpp} > "${OUTPUT}.g.cpp"
        DEPENDS ${FONT_EMBED_DEP} "fonts/${FONT_FILE}"
        USES_TERMINAL
    )
    list(APPEND OutputList "${OUTPUT}.g.cpp")
ENDFOREACH ()

add_library(ImGui_Resources STATIC ${OutputList})
set(IMGUI_RESOURCES_INCLUDE ${CMAKE_CURRENT_BINARY_DIR}/generated_fonts PARENT_SCOPE)
