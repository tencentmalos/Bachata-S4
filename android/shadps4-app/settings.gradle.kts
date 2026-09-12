pluginManagement {
    repositories {
        google()
        mavenCentral()
        gradlePluginPortal()
    }
}
dependencyResolutionManagement {
    repositoriesMode.set(RepositoriesMode.FAIL_ON_PROJECT_REPOS)
    repositories {
        google()
        mavenCentral()
    }
}
rootProject.name = "shadps4-app"
include(":app", ":core:model", ":core:data", ":core:database", ":core:runtime")
include(":core:testing", ":core:designsystem")
include(":feature:setup", ":feature:library", ":feature:session", ":feature:settings", ":feature:drivers")

// Foundation Android input library — referenced once from the Foundation subrepo
// path (spec android-foundation-input-first.md §Layer 2). It is a plain Android
// library with its own namespace; the app's version catalog supplies the plugin
// versions. It declares no native library and no app-specific JNI.
include(":foundation-input-android")
project(":foundation-input-android").projectDir =
    file("../../foundation/modules/input/android")

