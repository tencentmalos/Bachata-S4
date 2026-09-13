# Native session library for the shadps4-app replica.
#
# Unlike the reference runtime CMake (which built winlator/vortek/adrenotools + an external
# glibc shadPS4 loader), this builds ONLY the in-process FEX session library: it links the same
# guest_cpu_fex the CLI/tests/validation-app use (via cmake/fex from the main repo root) and drives
# the public guest_cpu API from a normal app process. No external process, no X server, no Vortek.
#
# The PS4 .pkg extractor (cpp/pkg) is built against the NDK's stock zlib — the reference's optional
# libdeflate speedup is not vendored, and pkg_extractor.cpp has a complete zlib fallback. It ships
# libbachata_pkg.so (PkgExtractor/PkgRsa JNI) for the library import path.
cmake_minimum_required(VERSION 3.22)
project(shadps4_fex_session_native LANGUAGES CXX C)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(V0_ENABLE_FEX ON CACHE BOOL "" FORCE)

# Main repo root: android/shadps4-app/core/runtime/src/main/cpp -> up 7 -> repo root.
get_filename_component(REPO_ROOT "${CMAKE_CURRENT_LIST_DIR}/../../../../../../.." ABSOLUTE)

# One host DSO owns input and production scePad*. Never compile a second adapter
# into JNI. Build with scripts/android/build-host-android before Gradle.
if(NOT EXISTS "${SHADPS4_HOST_LOADER_CONFIG}")
    message(FATAL_ERROR "Set hostLoaderConfig or SHADPS4_HOST_LOADER_CONFIG to the generated host config")
endif()
include("${SHADPS4_HOST_LOADER_CONFIG}")
if(NOT EXISTS "${SHADPS4_HOST_LOADER_LIBRARY}" OR NOT ANDROID_ABI STREQUAL SHADPS4_HOST_LOADER_ABI
   OR NOT ANDROID_STL STREQUAL SHADPS4_HOST_LOADER_STL OR ANDROID_PLATFORM_LEVEL LESS SHADPS4_HOST_LOADER_API)
    message(FATAL_ERROR "Host DSO ABI/API/STL mismatch")
endif()
add_library(android_host SHARED IMPORTED)
set_target_properties(android_host PROPERTIES IMPORTED_LOCATION "${SHADPS4_HOST_LOADER_LIBRARY}")
if(NOT SHADPS4_HOST_PROVIDES_GUEST_CPU_API)
    message(FATAL_ERROR "Rebuild host: the production guest CPU API provider is required")
endif()
add_library(guest_cpu_api INTERFACE)
target_include_directories(guest_cpu_api INTERFACE "${REPO_ROOT}/src")
target_compile_features(guest_cpu_api INTERFACE cxx_std_20)
target_link_libraries(guest_cpu_api INTERFACE android_host)

add_subdirectory("${REPO_ROOT}/cmake/fex" fex)

# The embeddable host session: backend-free lifecycle core (SessionCore) plus the
# FEX-backed adapter. Production build never defines SESSION_TEST_HOOKS, so the
# handshake gate and UAF tripwire are compiled out.
add_library(shadps4_fex_session SHARED
    fex_session_jni.cpp
    orbis_pad_jni.cpp
    "${REPO_ROOT}/src/core/host_runtime/session_core.cpp"
    "${REPO_ROOT}/src/core/host_runtime/session_backend.cpp"
    "${REPO_ROOT}/src/core/host_runtime/session_backend_fex.cpp")
target_include_directories(shadps4_fex_session PRIVATE ${SHADPS4_HOST_LOADER_INCLUDES})
target_compile_definitions(shadps4_fex_session PRIVATE ${SHADPS4_HOST_LOADER_DEFINITIONS})
target_compile_features(shadps4_fex_session PRIVATE cxx_std_23)
target_link_libraries(shadps4_fex_session PRIVATE guest_cpu_fex android_host)
target_link_options(shadps4_fex_session PRIVATE -Wl,--no-undefined)
target_include_directories(shadps4_fex_session PRIVATE "${REPO_ROOT}/src")
target_compile_options(shadps4_fex_session PRIVATE -fexceptions)
target_link_options(shadps4_fex_session PRIVATE -Wl,--build-id=sha1)

# PS4 .pkg extractor / crypto (PkgExtractor/PkgRsa JNI). Self-contained under pkg/, zlib-only.
add_subdirectory(pkg)

target_link_libraries(shadps4_fex_session PRIVATE android)
