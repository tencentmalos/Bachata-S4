package com.shadps4.android.runtime.settings

import kotlinx.serialization.json.JsonPrimitive
import kotlinx.serialization.json.booleanOrNull
import kotlinx.serialization.json.intOrNull

/** Capture both settings before renderer creation, resetting them on each game launch. */
object GpuReadbacks {
    const val MODE_ID = "gpu.readbacks_mode"
    const val LINEAR_ID = "gpu.readback_linear_images_enabled"
    val CHOICES = listOf("Disabled", "Relaxed", "Precise")
    data class Options(val mode: Int = 0, val linearImages: Boolean = false)

    fun resolve(global: RuntimeProfile, game: RuntimeProfile): Options {
        val rawMode = game.values[MODE_ID] ?: global.values[MODE_ID]
        val mode = if (rawMode == null) 0 else {
            val value = requireNotNull(rawMode as? JsonPrimitive)
            // Older desktop profiles stored the enum's integer ordinal.
            val ordinal = value.intOrNull ?: CHOICES.indexOf(value.content)
            require(ordinal in CHOICES.indices) { "Invalid GPU readback mode: $rawMode" }
            ordinal
        }
        val rawLinear = game.values[LINEAR_ID] ?: global.values[LINEAR_ID]
        val linear = if (rawLinear == null) false else
            requireNotNull((rawLinear as? JsonPrimitive)?.booleanOrNull) {
                "Invalid linear image readback setting: $rawLinear"
            }
        return Options(mode, linear)
    }
}
