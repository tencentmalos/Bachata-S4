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
    (providers.gradleProperty("fexBuildDir").orNull ?: localProps.getProperty("fexBuildDir") ?: System.getenv("FEX_BUILD_DIR") ?: "").trim()

val hostLoaderConfig = (providers.gradleProperty("hostLoaderConfig").orNull ?: localProps.getProperty("hostLoaderConfig")
    ?: System.getenv("SHADPS4_HOST_LOADER_CONFIG")
    ?: rootProject.file("../../build/android-host-api33/native/shadps4-host-loader.cmake").absolutePath).trim()

android {
    namespace = "com.shadps4.android.runtime"
    compileSdk = 36
    // NDK pinned to 29.0.14206865 (native API follows minSdk 33), NOT the reference's 30.x.
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
                    "-DSHADPS4_HOST_LOADER_CONFIG=$hostLoaderConfig",
                    "-DCMAKE_BUILD_TYPE=RelWithDebInfo",
                )
                cppFlags += listOf("-std=c++23")
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
    implementation(project(":foundation-input-android"))
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

// These hook DSOs must be packaged, but MUST NOT be linked as DT_NEEDED:
// loading them in the app namespace interposes fopen/ioctl before hook init.
val nativeHooks = layout.buildDirectory.dir("generated/adrenotoolsJniLibs")
android.sourceSets.getByName("main").jniLibs.srcDir(nativeHooks)
val packageNativeHooks = tasks.register("packageNativeHooks") {
    val config = file(hostLoaderConfig)
    inputs.file(config)
    val marker = "set(SHADPS4_HOST_ADRENOTOOLS_HOOKS [==["
    val hookFiles = if (config.exists()) config.readText().substringAfter(marker, "")
        .substringBefore("]==])").split(';').filter { it.isNotBlank() }.map { file(it) } else emptyList()
    inputs.files(hookFiles)
    outputs.dir(nativeHooks)
    doLast {
        check(hookFiles.size == 4 && hookFiles.all { it.isFile }) {
            "Rebuild native host: expected four adrenotools hooks in host loader config"
        }
        val output = nativeHooks.get().dir("arm64-v8a").asFile.apply { mkdirs() }
        hookFiles.forEach { it.copyTo(output.resolve(it.name), overwrite = true) }
    }
}
tasks.named("preBuild").configure { dependsOn(packageNativeHooks) }
