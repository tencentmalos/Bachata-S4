package com.shadps4.android.runtime.settings

import kotlinx.serialization.json.JsonPrimitive

/** Same global/game precedence as the settings UI, consumed before native session startup. */
object GuestShadingQuality {
    const val ID = "gpu.shading_quality"
    private val choices = listOf("Low (1/4)", "Medium (1/2)", "High (1/1)")

    fun resolve(global: RuntimeProfile, game: RuntimeProfile): Int {
        val value = game.values[ID] ?: global.values[ID] ?: return 2
        val choice = (value as? JsonPrimitive)?.content
        return choices.indexOf(choice).also { require(it >= 0) { "Invalid shading quality: $choice" } }
    }
}
