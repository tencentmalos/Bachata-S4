package com.shadps4.android.runtime.settings

import kotlinx.serialization.json.JsonPrimitive

/** Physical host image scale; applied before native renderer creation. */
object InternalScale {
    const val ID = "gpu.internal_scale"
    const val DEFAULT_PERCENT = 50f
    private val choices = mapOf("0.25" to 25f, "0.375" to 37.5f, "0.5" to 50f, "0.75" to 75f, "1.0" to 100f)
    fun resolve(global: RuntimeProfile, game: RuntimeProfile): Float {
        val value = game.values[ID] ?: global.values[ID] ?: return DEFAULT_PERCENT
        val choice = (value as? JsonPrimitive)?.content
        return requireNotNull(choices[choice]) { "Invalid internal scale: $choice" }
    }
}
