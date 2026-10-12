plugins { id("com.android.application") version "8.13.2" }

android {
    namespace = "org.shadps4.xrealprobe"
    compileSdk = 36
    ndkVersion = "29.0.14206865"
    defaultConfig {
        applicationId = "org.shadps4.xrealprobe"
        minSdk = 29
        targetSdk = 32 // Matches the experimental SDK bridge, including its legacy receivers.
        versionCode = 1
        versionName = "0.1-sdk3.1.0"
        ndk { abiFilters += "arm64-v8a" }
    }
    sourceSets["main"].jniLibs.srcDir("build/sdk/jniLibs")
    sourceSets["main"].assets.srcDir("build/sdk/assets")
    packaging { jniLibs { useLegacyPackaging = true } }
}
dependencies {
    implementation(project(":foundation-xreal"))
    implementation(fileTree("build/sdk/aars") { include("*.aar") })
}
tasks.named("preBuild") {
    doFirst {
        check(file("build/sdk/assets/sdk-manifest.json").exists()) {
            "Run Foundation modules/xreal/tools/prepare-sdk.py with the SDK package and this module's build/sdk output."
        }
    }
}
