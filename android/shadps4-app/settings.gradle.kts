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
