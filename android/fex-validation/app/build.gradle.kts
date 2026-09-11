import java.util.Properties

plugins {
    id("com.android.application")
    id("org.jetbrains.kotlin.android")
}

// The prebuilt FEX static libraries are produced by scripts/android/build-fexcore-android via the
// pinned cmake/fex tree. Their directory is supplied as fexBuildDir in local.properties (read here)
// or the FEX_BUILD_DIR environment variable; the app's CMake add_subdirectory()s cmake/fex and links
// guest_cpu_fex. This keeps the app on the same backend the CLI/tests use rather than shipping a
// copied or separately-built .so.
// NB: `java.util.Properties()` cannot be written inline here — in the Gradle Kotlin DSL the bare
// `java` identifier resolves to the Java plugin extension accessor, not the java package, so the
// type is imported above and used unqualified instead.
val localProps = Properties().apply {
    val f = rootProject.file("local.properties")
    if (f.exists()) f.inputStream().use { load(it) }
}
val fexBuildDir: String =
    (localProps.getProperty("fexBuildDir") ?: System.getenv("FEX_BUILD_DIR") ?: "").trim()
require(fexBuildDir.isNotEmpty()) {
    "Set fexBuildDir=<dir with built FEX static libs> in local.properties or FEX_BUILD_DIR"
}

android {
    namespace = "com.shadps4.fexvalidation"
    compileSdk = 36

    // Pin the NDK explicitly so the build fails loudly on a different install rather than silently
    // taking AGP's default. This is the Round2-sanctioned NDK; its sysroot metadata advertises
    // max API 35 (native API35 is explicitly allowed by Round2 and is distinct from app SDK36).
    ndkVersion = "29.0.14206865"

    defaultConfig {
        applicationId = "com.shadps4.fexvalidation"
        // minSdk 33 lets the AYN Thor API33 validation device install/run; this is the API33 helper
        // profile from the spec, not the Swan API36 final acceptance.
        minSdk = 33
        targetSdk = 35
        versionCode = 1
        versionName = "round2-validation"

        ndk {
            abiFilters += listOf("arm64-v8a")
        }

        externalNativeBuild {
            cmake {
                arguments += listOf(
                    "-DANDROID_STL=c++_shared",
                    "-DV0_ENABLE_FEX=ON",
                    "-DV0_BUILD_TESTS=ON",
                    "-DFEX_BUILD_DIR=$fexBuildDir",
                    "-DCMAKE_BUILD_TYPE=RelWithDebInfo"
                )
                cFlags += listOf("-std=c17")
                cppFlags += listOf("-std=c++20")
            }
        }
    }

    externalNativeBuild {
        cmake {
            path = file("src/main/cpp/CMakeLists.txt")
            // The SDK ships CMake 3.22.1 with a bundled Ninja; every CMakeLists in this tree
            // (app, cmake/fex, FEX) declares cmake_minimum_required 3.22 or lower, so this is the
            // official-toolchain match. Avoid a homebrew CMake 4.x here: it drops compatibility with
            // cmake_minimum_required < 3.5 and would break FEX's external subtrees.
            version = "3.22.1"
        }
    }

    buildTypes {
        named("release") {
            isMinifyEnabled = false
        }
        named("debug") {
            isMinifyEnabled = false
        }
    }

    compileOptions {
        sourceCompatibility = JavaVersion.VERSION_17
        targetCompatibility = JavaVersion.VERSION_17
    }

    kotlinOptions {
        jvmTarget = "17"
    }
}
