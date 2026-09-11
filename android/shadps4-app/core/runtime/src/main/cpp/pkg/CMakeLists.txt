add_library(bachata_pkg SHARED
    bachata_pkg_jni.cpp
    pkg_extractor.cpp
    pkg_crypto.cpp
    pkg_type.cpp
    aes.c
)

target_include_directories(bachata_pkg PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})
target_compile_features(bachata_pkg PRIVATE cxx_std_20)
target_compile_options(bachata_pkg PRIVATE -fexceptions -frtti)
# Enable the ARMv8 AES/PMULL crypto instructions for the hardware XTS path in
# pkg_crypto.cpp (guarded by __ARM_FEATURE_AES). arm64-v8a mandates these on
# every conforming CPU. Scoped to bachata_pkg so sibling libs are unaffected.
if(ANDROID_ABI STREQUAL "arm64-v8a")
    target_compile_options(bachata_pkg PRIVATE -march=armv8-a+crypto)
endif()
# Force -O2 on the PKG extract/crypto hot path regardless of CMake build type.
# AGP compiles native code at the clang default (-O0) for the debug variant;
# the XTS-AES / PFSC inflate / parallel extract loop is ~5-10x slower
# unoptimized. The last -O flag wins, so this overrides the toolchain -O0.
target_compile_options(bachata_pkg PRIVATE -O2)
find_library(log-lib log)
find_library(z-lib z)
target_link_libraries(bachata_pkg PRIVATE ${log-lib} ${z-lib})

# NOTE: the reference linked libdeflate here (a ~2x-faster single-shot inflate) via
# externals/libdeflate, which is not vendored into the main repo. pkg_extractor.cpp has a complete
# zlib #else fallback (inflateInit/inflate/inflateEnd), so this replica builds against the NDK's
# stock zlib and does NOT define BACHATA_HAVE_LIBDEFLATE. Re-add libdeflate later purely for speed
# if wanted; correctness does not depend on it.
