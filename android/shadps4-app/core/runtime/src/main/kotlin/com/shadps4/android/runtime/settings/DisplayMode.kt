package com.shadps4.android.runtime.settings

import kotlinx.serialization.json.JsonPrimitive

/** Resolve before Activity/Vulkan creation; preferences never change a running session. */
object DisplayMode {
    const val ID = "android.display_mode"
    enum class Mode { TWO_D, XR }
    data class Decision(val preferred: Mode, val effective: Mode, val forcedByPsvr: Boolean)

    fun resolve(global: RuntimeProfile, game: RuntimeProfile, psvr: Boolean): Decision {
        val value = game.values[ID] ?: global.values[ID]
        val preferred = when ((value as? JsonPrimitive)?.content) {
            null, "2d" -> Mode.TWO_D
            "xr" -> Mode.XR
            else -> error("Invalid display mode: $value")
        }
        return Decision(preferred, if (psvr) Mode.XR else preferred, psvr)
    }
}
