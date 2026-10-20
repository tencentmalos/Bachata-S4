package com.shadps4.android.runtime.settings

import kotlinx.serialization.json.JsonPrimitive

/**
 * Streaming submit (gpu.stream_submit): the open command buffer is submitted early once this many
 * draws are recorded and the GPU has run out of work, so the GPU and the command processor
 * overlap. Off by default; turned on per game where it helps.
 */
object StreamSubmit {
    const val ID = "gpu.stream_submit"
    /** Draws per early submit when on (Core::GameOptions::StreamSubmitOnDraws). */
    const val ON_DRAWS = 100

    /** Draws per early submit for the session; 0 = off. */
    fun resolve(global: RuntimeProfile, game: RuntimeProfile): Int {
        val value = game.values[ID] ?: global.values[ID] ?: return 0
        return when (val choice = (value as? JsonPrimitive)?.content) {
            "off" -> 0
            "on" -> ON_DRAWS
            else -> throw IllegalArgumentException("Invalid streaming submit: $choice")
        }
    }
}
