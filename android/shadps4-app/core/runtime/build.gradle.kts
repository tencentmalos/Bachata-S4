import java.util.Properties

plugins {
    alias(libs.plugins.android.library)
    alias(libs.plugins.kotlin.android)
    alias(libs.plugins.kotlin.serialization)
}

// The prebuilt FEX static libraries are produced by scripts/android/build-fexcore-android via the
// pinned cmake/fex tree (main repo root). Their directory is supplied as fexBuildDir in
// local.properties or the FEX_BUILD_DIR env var; the native CMakeLists add_subdirectory()s cmake/fex
// and links guest_cpu_fex — the same backend the CLI and fex-validation app use.
// NB: import Properties at the top — the bare `java` identifier is the Java plugin accessor in the
// Gradle Kotlin DSL, not the java package (A0 hit this).
val localProps = Properties().apply {
    val f = rootProject.file("local.properties")
    if (f.exists()) f.inputStream().use { load(it) }
}
val fexBuildDir: String =
    (localProps.getProperty("fexBuildDir") ?: System.getenv("FEX_BUILD_DIR") ?: "").trim()

android {
    namespace = "com.shadps4.android.runtime"
    compileSdk = 36
    // NDK pinned to the A0-validated 29.0.14206865 (native API 35), NOT the reference's 30.x.
    ndkVersion = "29.0.14206865"
    defaultConfig {
        minSdk = 33
        ndk { abiFilters += "arm64-v8a" }
        testInstrumentationRunner = "androidx.test.runner.AndroidJUnitRunner"
        externalNativeBuild {
            cmake {
                arguments += listOf(
                    "-DANDROID_STL=c++_shared",
                    "-DV0_ENABLE_FEX=ON",
                    "-DV0_BUILD_TESTS=OFF",
                    "-DFEX_BUILD_DIR=$fexBuildDir",
                    "-DCMAKE_BUILD_TYPE=RelWithDebInfo",
                )
                cppFlags += listOf("-std=c++20")
            }
        }
    }
    // Builds libshadps4_fex_session.so from src/main/cpp (FEX in-process session; no winlator/vortek).
    externalNativeBuild {
        cmake {
            path = file("src/main/cpp/CMakeLists.txt")
            version = "3.22.1"
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

dependencies {
    api(project(":core:model"))
    api(libs.kotlinx.coroutines.core)
    implementation(libs.kotlinx.coroutines.android)
    implementation(libs.kotlinx.serialization.json)
    implementation("androidx.collection:collection:1.5.0")
    implementation("androidx.annotation:annotation:1.9.1")
    testImplementation(libs.junit)
    testImplementation(libs.kotlinx.coroutines.test)
    testImplementation(libs.turbine)
    testImplementation(kotlin("test"))
    androidTestImplementation(libs.junit)
    androidTestImplementation("androidx.test.ext:junit:1.2.1")
    androidTestImplementation("androidx.test:runner:1.6.2")
    androidTestImplementation(libs.kotlinx.coroutines.test)
    androidTestImplementation(kotlin("test"))
}
