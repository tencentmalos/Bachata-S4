package com.shadps4.android.runtime.driver

import java.nio.file.Path

enum class RuntimeVulkanDriver {
    SYSTEM,
    SYSTEM_VORTEK,
    CUSTOM,
    TURNIP_25_0_0,
    TURNIP_25_3_0_R11,
    TURNIP_26_1_0,
}

/**
 * Profile / backend driver ids used by [com.shadps4.android.runtime.settings.RuntimeProfile.driverId].
 * Synthetic ids (not present under vulkan-drivers/installed).
 */
object RuntimeVulkanDriverIds {
    const val SYSTEM = "system"
    const val SYSTEM_VORTEK = "system-vortek"

    fun isSynthetic(id: String): Boolean =
        id == SYSTEM || id == SYSTEM_VORTEK

    fun isVortek(id: String?): Boolean = id == SYSTEM_VORTEK
}

object RuntimeVulkanDriverPreference {
    const val FILE_NAME = "emulator_settings"
    const val KEY = "vulkan_driver"
    /** Safe default remains legacy SYSTEM (APK native bridge), not experimental Vortek. */
    val DEFAULT = RuntimeVulkanDriver.SYSTEM

    /**
     * Decode a stored preference value.
     *
     * Migration policy:
     * - `SYSTEM` stays [RuntimeVulkanDriver.SYSTEM] (existing production behavior).
     * - `SYSTEM_VORTEK` is opt-in experimental only.
     * - Unknown / null / blank → [DEFAULT] (never auto-select Vortek).
     * - Enum names are matched case-sensitively.
     */
    fun decode(value: String?): RuntimeVulkanDriver {
        if (value.isNullOrBlank()) return DEFAULT
        return RuntimeVulkanDriver.entries.firstOrNull { it.name == value } ?: DEFAULT
    }
}

/**
 * Session-scoped inputs for driver resolution.
 * Socket path is owned by the session lifecycle — never generated here.
 */
data class VulkanDriverResolveContext(
    val runtimeRoot: Path,
    val customDriverRoot: Path? = null,
    val vortekSocketPath: Path? = null,
)

/**
 * Backend-neutral Vulkan driver configuration for the in-process native FEX build.
 *
 * The external-process / glibc host-loader / Vortek model was removed with the runtime
 * process launcher, so the old `Box64Mode` discriminator no longer exists. The only real
 * launch paths are the APK-native system loader ([RuntimeVulkanDriver.SYSTEM]) and
 * Android bionic adrenotools drivers ([DriverAbi.ANDROID_BIONIC]); every glibc / Vortek
 * path fails fast rather than silently returning a non-functional environment.
 */
data class VulkanDriverConfiguration(
    val environment: Map<String, String>,
    /** Stable log / diagnostics id (e.g. turnip-...). */
    val driverProfileId: String = "unknown",
) {
    companion object {
        private const val UNSUPPORTED =
            "glibc/vortek driver path is not supported in the native FEX build"

        fun resolve(driver: InstalledDriver, runtimeRoot: Path): VulkanDriverConfiguration =
            when (driver.metadata.abi) {
                DriverAbi.ANDROID_BIONIC -> VulkanDriverConfiguration(
                    environment = mapOf(
                        "SDL_VULKAN_LIBRARY" to "libvulkan.so.1",
                        "BACHATA_VULKAN_DRIVER_DIR" to driver.root.toString() + "/",
                        "BACHATA_VULKAN_DRIVER_NAME" to driver.library.fileName.toString(),
                        "BACHATA_VULKAN_TMPDIR" to runtimeRoot.resolve("tmp").toString(),
                    ),
                    driverProfileId = driver.metadata.id,
                )
                DriverAbi.LINUX_GLIBC ->
                    throw UnsupportedOperationException(UNSUPPORTED)
            }

        fun resolve(
            driver: RuntimeVulkanDriver,
            runtimeRoot: Path,
            customDriverRoot: Path? = null,
            vortekSocketPath: Path? = null,
        ): VulkanDriverConfiguration =
            resolve(
                driver,
                VulkanDriverResolveContext(
                    runtimeRoot = runtimeRoot,
                    customDriverRoot = customDriverRoot,
                    vortekSocketPath = vortekSocketPath,
                ),
            )

        fun resolve(
            driver: RuntimeVulkanDriver,
            context: VulkanDriverResolveContext,
        ): VulkanDriverConfiguration {
            val runtimeRoot = context.runtimeRoot
            return when (driver) {
                RuntimeVulkanDriver.SYSTEM -> VulkanDriverConfiguration(
                    environment = mapOf(
                        "SDL_VULKAN_LIBRARY" to "libvulkan.so.1",
                    ),
                    driverProfileId = RuntimeVulkanDriverIds.SYSTEM,
                )
                RuntimeVulkanDriver.TURNIP_25_3_0_R11 -> VulkanDriverConfiguration(
                    environment = mapOf(
                        "SDL_VULKAN_LIBRARY" to "libvulkan.so.1",
                        "BACHATA_VULKAN_DRIVER_DIR" to
                            runtimeRoot.resolve("drivers/turnip-25.3.0-r11").toString() + "/",
                        "BACHATA_VULKAN_DRIVER_NAME" to "vulkan.ad07xx.so",
                        "BACHATA_VULKAN_TMPDIR" to runtimeRoot.resolve("tmp").toString(),
                    ),
                    driverProfileId = "turnip-25.3.0-r11",
                )
                // glibc host-loader and Vortek client/server paths were removed with the
                // external-process runtime model; they have no equivalent in the native FEX build.
                RuntimeVulkanDriver.SYSTEM_VORTEK,
                RuntimeVulkanDriver.CUSTOM,
                RuntimeVulkanDriver.TURNIP_25_0_0,
                RuntimeVulkanDriver.TURNIP_26_1_0 ->
                    throw UnsupportedOperationException(UNSUPPORTED)
            }
        }

        fun resolveByDriverId(
            driverId: String,
            context: VulkanDriverResolveContext,
            installedResolver: (String) -> InstalledDriver? = { null },
        ): VulkanDriverConfiguration =
            when (driverId) {
                RuntimeVulkanDriverIds.SYSTEM -> resolve(RuntimeVulkanDriver.SYSTEM, context)
                RuntimeVulkanDriverIds.SYSTEM_VORTEK -> resolve(RuntimeVulkanDriver.SYSTEM_VORTEK, context)
                else -> {
                    val installed = installedResolver(driverId)
                        ?: throw IllegalStateException(
                            "Selected Vulkan driver '$driverId' is not installed; open Turnip drivers and select another driver",
                        )
                    resolve(installed, context.runtimeRoot)
                }
            }
    }
}
