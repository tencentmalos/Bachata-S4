# Native session library for the shadps4-app replica.
#
# Unlike the reference runtime CMake (which built winlator/vortek/adrenotools + an external
# glibc shadPS4 loader), this builds ONLY the in-process FEX session library: it links the same
# guest_cpu_fex the CLI/tests/validation-app use (via cmake/fex from the main repo root) and drives
# the public guest_cpu API from a normal app process. No external process, no X server, no Vortek.
#
# The PS4 .pkg extractor (cpp/pkg, needs externals/libdeflate) is NOT built here yet — libdeflate is
# not vendored into the main repo. The Kotlin import UI compiles without it; wire the pkg native lib
# in once libdeflate is available.
cmake_minimum_required(VERSION 3.22)
project(shadps4_fex_session_native LANGUAGES CXX C)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(V0_ENABLE_FEX ON CACHE BOOL "" FORCE)

# Main repo root: android/shadps4-app/core/runtime/src/main/cpp -> up 7 -> repo root.
get_filename_component(REPO_ROOT "${CMAKE_CURRENT_LIST_DIR}/../../../../../../.." ABSOLUTE)

add_subdirectory("${REPO_ROOT}/cmake/fex" fex)

add_library(shadps4_fex_session SHARED fex_session_jni.cpp)
target_link_libraries(shadps4_fex_session PRIVATE guest_cpu_fex)
target_include_directories(shadps4_fex_session PRIVATE "${REPO_ROOT}/src")
target_compile_options(shadps4_fex_session PRIVATE -fexceptions)
target_link_options(shadps4_fex_session PRIVATE -Wl,--build-id=sha1)
