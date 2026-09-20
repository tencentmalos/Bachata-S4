package com.shadps4.android.runtime.settings

import kotlinx.serialization.json.JsonPrimitive

/** Independent asset quality, captured before renderer creation. */
object TextureQuality {
    const val ID = "gpu.texture_quality"
    const val DEFAULT = 0
    private val choices = mapOf("high" to 0, "medium" to 1, "low" to 2)
    fun resolve(global: RuntimeProfile, game: RuntimeProfile): Int {
        val value = game.values[ID] ?: global.values[ID] ?: return DEFAULT
        val choice = (value as? JsonPrimitive)?.content
        return requireNotNull(choices[choice]) { "Invalid texture quality: $choice" }
    }
}
