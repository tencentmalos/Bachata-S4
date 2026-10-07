package com.shadps4.android.runtime.settings

import kotlinx.serialization.json.JsonPrimitive

/** Resolve before Activity/Vulkan creation; preferences never change a running session. */
object DisplayMode {
    const val ID = "android.display_mode"
    /** How a PSVR title is shown: immersive XR, or side by side in a 2D window (a device without
     *  an OpenXR runtime), where the device's motion sensor stands in for the headset. */
    const val PSVR_ID = "android.psvr_display"
    enum class Mode { TWO_D, XR }
    /** [psvr]: the title is a PSVR title. [forcedByPsvr]: it is one, and runs in XR for that. */
    data class Decision(val preferred: Mode, val effective: Mode, val forcedByPsvr: Boolean,
                        val psvr: Boolean = forcedByPsvr)

    fun resolve(global: RuntimeProfile, game: RuntimeProfile, psvr: Boolean): Decision {
        val value = game.values[ID] ?: global.values[ID]
        val preferred = when ((value as? JsonPrimitive)?.content) {
            null, "2d" -> Mode.TWO_D
            "xr" -> Mode.XR
            else -> error("Invalid display mode: $value")
        }
        if (!psvr) return Decision(preferred, preferred, forcedByPsvr = false, psvr = false)
        val psvrValue = game.values[PSVR_ID] ?: global.values[PSVR_ID]
        val effective = when ((psvrValue as? JsonPrimitive)?.content) {
            null, "xr" -> Mode.XR
            "sbs" -> Mode.TWO_D
            else -> error("Invalid PSVR display: $psvrValue")
        }
        return Decision(preferred, effective, forcedByPsvr = effective == Mode.XR, psvr = true)
    }
}
