package com.shadps4.android.runtime.settings

import kotlinx.serialization.json.JsonPrimitive
import kotlinx.serialization.json.booleanOrNull

/** Move slots have no intrinsic handedness; games can assign them differently. */
object XrMoveHands {
    const val ID = "input.xr_swap_move_hands"
    fun resolve(global: RuntimeProfile, game: RuntimeProfile): Boolean {
        val value = game.values[ID] ?: global.values[ID] ?: return false
        return requireNotNull((value as? JsonPrimitive)?.booleanOrNull) { "Invalid XR Move hand order: $value" }
    }
}
