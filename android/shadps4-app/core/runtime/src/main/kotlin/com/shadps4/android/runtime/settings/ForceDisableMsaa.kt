package com.shadps4.android.runtime.settings

import kotlinx.serialization.json.JsonPrimitive
import kotlinx.serialization.json.booleanOrNull

/** Captured before Vulkan creation; changing a live game's sample layout is unsupported. */
object ForceDisableMsaa {
    const val ID = "gpu.force_disable_msaa"
    fun resolve(global: RuntimeProfile, game: RuntimeProfile): Boolean {
        val value = game.values[ID] ?: global.values[ID] ?: return false
        return requireNotNull((value as? JsonPrimitive)?.booleanOrNull) { "Invalid MSAA override: $value" }
    }
}
