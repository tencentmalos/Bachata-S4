// Root build file. AGP 8.13 does NOT auto-apply the Kotlin Android plugin (AGP 9 did), so every
// Android module must apply `kotlin-android` explicitly — declared here and applied per-module.
// Hilt and KSP are declared here for the modules that use DI / annotation processing.
plugins {
    alias(libs.plugins.android.application) apply false
    alias(libs.plugins.android.library) apply false
    alias(libs.plugins.kotlin.android) apply false
    alias(libs.plugins.compose.compiler) apply false
    alias(libs.plugins.kotlin.serialization) apply false
    alias(libs.plugins.hilt) apply false
    alias(libs.plugins.ksp) apply false
}
