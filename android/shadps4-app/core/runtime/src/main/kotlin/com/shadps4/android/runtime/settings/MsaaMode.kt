package com.shadps4.android.runtime.settings

import kotlinx.serialization.json.JsonPrimitive
import kotlinx.serialization.json.booleanOrNull

/**
 * Game multisampling: off, at most 2x, or the game's own count. The host caps the game's
 * count at the GPU's framebuffer limit and at 4x on Android in every case (8x is far too slow
 * on mobile GPUs), so no choice can fall back to 8x. Captured before Vulkan creation.
 */
object MsaaMode {
    const val ID = "gpu.msaa"
    /** The earlier boolean "Force Disable MSAA"; true still means off when [ID] is unset. */
    const val LEGACY_ID = "gpu.force_disable_msaa"
    const val OFF = "off"
    const val X2 = "2x"
    const val GAME = "game"
    val CHOICES = listOf(OFF, X2, GAME)

    data class Options(val disabled: Boolean, val maxSamples: Int)

    /** The stored choice: [ID] of the game, then of global, each falling back to the legacy flag. */
    fun choice(global: RuntimeProfile, game: RuntimeProfile): String {
        fun of(profile: RuntimeProfile): String? {
            profile.values[ID]?.let { raw ->
                val value = (raw as? JsonPrimitive)?.content
                require(value in CHOICES) { "Invalid MSAA mode: $raw" }
                return value
            }
            val legacy = profile.values[LEGACY_ID] ?: return null
            val disabled = requireNotNull((legacy as? JsonPrimitive)?.booleanOrNull) {
                "Invalid MSAA override: $legacy"
            }
            return if (disabled) OFF else GAME
        }
        return of(game) ?: of(global) ?: GAME
    }

    /** maxSamples 0: the host's own limit (device framebuffer limit, at most 4x on Android). */
    fun resolve(global: RuntimeProfile, game: RuntimeProfile): Options = when (choice(global, game)) {
        OFF -> Options(disabled = true, maxSamples = 0)
        X2 -> Options(disabled = false, maxSamples = 2)
        else -> Options(disabled = false, maxSamples = 0)
    }
}
