# Native validation library for the ordinary shadPS4 FEX validation app.
#
# Unlike the throwaway review probes this is NOT a renamed copy of the CLI test main: it exposes a
# small JNI surface over the *public* guest_cpu API and links the same guest_cpu_fex the CLI uses
# (added via cmake/fex from the repo root), so the in-app run exercises the real backend in a normal
# app process. It performs a self-contained x86-64 arithmetic loop (input -> guest buffer -> real
# FEX execution -> output) and reports device/library identity; it does not depend on test hooks.
cmake_minimum_required(VERSION 3.22)
project(shadps4_fex_validation_native LANGUAGES CXX C)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(V0_ENABLE_FEX ON CACHE BOOL "" FORCE)

# cmake/fex lives in the repo root (this file is android/fex-validation/app/src/main/cpp).
get_filename_component(REPO_ROOT "${CMAKE_CURRENT_LIST_DIR}/../../../../../.." ABSOLUTE)

add_subdirectory("${REPO_ROOT}/cmake/fex" fex)

add_library(shadps4_fex_validation SHARED validation_jni.cpp)
target_link_libraries(shadps4_fex_validation PRIVATE guest_cpu_fex)
target_include_directories(shadps4_fex_validation PRIVATE "${REPO_ROOT}/src")
target_compile_options(shadps4_fex_validation PRIVATE -fexceptions)
target_link_options(shadps4_fex_validation PRIVATE -Wl,--build-id=sha1)
