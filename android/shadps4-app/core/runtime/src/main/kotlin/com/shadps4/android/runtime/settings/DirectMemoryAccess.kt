package com.shadps4.android.runtime.settings

import kotlinx.serialization.json.JsonPrimitive
import kotlinx.serialization.json.booleanOrNull

/** Applied before renderer creation, including an explicit false on subsequent launches. */
object DirectMemoryAccess {
    const val ID = "gpu.direct_memory_access_enabled"
    fun resolve(global: RuntimeProfile, game: RuntimeProfile): Boolean {
        val value = game.values[ID] ?: global.values[ID] ?: return false
        return requireNotNull((value as? JsonPrimitive)?.booleanOrNull) {
            "Invalid direct memory access setting: $value"
        }
    }
}
