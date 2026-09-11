// Top-level build file. The activity/bridge are written in Kotlin, so the Kotlin Android plugin is
// required in addition to the Android application plugin — without it the .kt sources are silently
// left out of the dex and the app crashes at launch with ClassNotFoundException for MainActivity.
// Versions are pinned here so a fresh checkout resolves deterministically. Kotlin 2.0.20 pairs with
// AGP 8.13.2 and ships a gradle85-tagged plugin compatible with the pinned Gradle 8.13 wrapper.
plugins {
    id("com.android.application") version "8.13.2" apply false
    id("org.jetbrains.kotlin.android") version "2.0.20" apply false
}
