pluginManagement {
    repositories { google(); mavenCentral(); gradlePluginPortal() }
    plugins { id("com.android.library") version "8.13.2" }
}
dependencyResolutionManagement { repositories { google(); mavenCentral() } }
rootProject.name = "xreal-pose-probe"
include(":foundation-xreal")
project(":foundation-xreal").projectDir=file("../../foundation/modules/xreal/android")
