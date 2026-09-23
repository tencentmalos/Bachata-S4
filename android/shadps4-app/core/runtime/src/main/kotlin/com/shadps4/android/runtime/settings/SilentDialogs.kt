package com.shadps4.android.runtime.settings

import kotlinx.serialization.json.JsonPrimitive
import kotlinx.serialization.json.booleanOrNull

/** Captured per session; choices and text input always remain interactive. */
object SilentDialogs {
    const val ID = "general.silent_dialogs"
    fun resolve(global: RuntimeProfile, game: RuntimeProfile): Boolean {
        val value = game.values[ID] ?: global.values[ID] ?: return true
        return requireNotNull((value as? JsonPrimitive)?.booleanOrNull) { "Invalid silent dialog setting: $value" }
    }
}
