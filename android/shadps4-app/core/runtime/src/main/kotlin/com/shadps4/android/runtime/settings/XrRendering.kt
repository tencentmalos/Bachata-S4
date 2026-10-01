package com.shadps4.android.runtime.settings

import kotlinx.serialization.json.JsonPrimitive
import kotlinx.serialization.json.JsonElement

object XrRendering {
    const val UPSCALER = "gpu.xr_upscaler"
    /** Ordinary 2D screen: FSR1/SGSR1 from the guest image to the window size. */
    const val SCREEN_UPSCALER = "gpu.screen_upscaler"
    const val OUTPUT = "gpu.xr_output_resolution"
    const val STATUS = "gpu.xr_status_layer"
    const val FOVEATION = "gpu.xr_foveation"
    const val LEVEL = "gpu.xr_foveation_level"
    const val SHARPNESS = "gpu.xr_upscale_sharpness"
    data class Options(val upscaler: Int = 1, val foveation: Int = 2, val level: Int = 0, val sharpness: Int = 50, val outputResolution: Int = 0, val statusLayer: Int = 0)
    // Retain selections made with the short-lived checkbox version.
    fun statusChoice(raw: JsonElement?): String = when ((raw as? JsonPrimitive)?.content) {
        null -> { require(raw == null) { "Invalid $STATUS: $raw" }; "horizontal" }
        "horizontal", "true" -> "horizontal"
        "vertical" -> "vertical"
        "none", "false" -> "none"
        else -> throw IllegalArgumentException("Invalid $STATUS: $raw")
    }
    fun resolve(global: RuntimeProfile, game: RuntimeProfile, xr: Boolean): Options {
        fun select(id: String, choices: List<String>, default: Int): Int {
            val raw = game.values[id] ?: global.values[id] ?: return default
            val index = choices.indexOf((raw as? JsonPrimitive)?.content)
            require(index >= 0) { "Invalid $id: $raw" }
            return index
        }
        if (!xr) return Options(
            upscaler = select(SCREEN_UPSCALER, listOf("off", "fsr1", "sgsr1"), 0), foveation = 0,
            sharpness = listOf(0, 25, 50, 75, 100)[select(SHARPNESS, listOf("0", "25", "50", "75", "100"), 2)],
            statusLayer = 2)
        return Options(
            select(UPSCALER, listOf("off", "fsr1", "sgsr1"), 1),
            select(FOVEATION, listOf("off", "fixed", "eye_tracked"), 2),
            select(LEVEL, listOf("low", "balanced", "high"), 0),
            listOf(0, 25, 50, 75, 100)[select(SHARPNESS, listOf("0", "25", "50", "75", "100"), 2)],
            select(OUTPUT, listOf("recommended", "high", "maximum"), 0),
            listOf("horizontal", "vertical", "none").indexOf(statusChoice(game.values[STATUS] ?: global.values[STATUS])),
        )
    }
}
