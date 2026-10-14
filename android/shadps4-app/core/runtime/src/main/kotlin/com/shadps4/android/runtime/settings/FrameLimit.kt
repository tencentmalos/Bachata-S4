package com.shadps4.android.runtime.settings

import kotlinx.serialization.json.JsonPrimitive

/** Most guest frames presented a second (0 = the game's own rate); see gpu.frame_limit. */
object FrameLimit {
    const val ID = "gpu.frame_limit"
    private val choices = mapOf("off" to 0, "50" to 50, "45" to 45, "40" to 40, "30" to 30, "20" to 20)

    fun resolve(global: RuntimeProfile, game: RuntimeProfile): Int {
        val value = game.values[ID] ?: global.values[ID] ?: return 0
        val choice = (value as? JsonPrimitive)?.content
        return requireNotNull(choices[choice]) { "Invalid frame limit: $choice" }
    }
}
