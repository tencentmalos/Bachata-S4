package com.shadps4.android.runtime.settings

import kotlinx.serialization.json.JsonPrimitive
import kotlinx.serialization.json.booleanOrNull

/** Touchpad gestures from the right stick and shoulder buttons, for controllers without one. */
object TouchpadEmulation {
    const val ID = "input.touchpad_emulation"
    fun resolve(global: RuntimeProfile, game: RuntimeProfile): Boolean {
        val value = game.values[ID] ?: global.values[ID] ?: return false
        return requireNotNull((value as? JsonPrimitive)?.booleanOrNull) { "Invalid touchpad emulation: $value" }
    }
}
