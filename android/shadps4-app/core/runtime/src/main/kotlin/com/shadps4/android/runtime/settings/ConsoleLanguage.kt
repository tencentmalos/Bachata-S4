package com.shadps4.android.runtime.settings

import kotlinx.serialization.json.JsonElement
import kotlinx.serialization.json.JsonPrimitive

/** Orbis language IDs; independent of Android UI locale and applied before GuestPlatform exists. */
object ConsoleLanguage {
    const val ID = "general.console_language"
    const val DEFAULT_LANGUAGE = 1
    private val spec by lazy { RuntimeSettingCatalog.loadAndroidSettings().single { it.id == ID } }

    private fun language(value: JsonElement?): Int {
        if (value == null) return DEFAULT_LANGUAGE
        val primitive = value as? JsonPrimitive
        val choice = if (primitive?.isString == true) spec.choices.indexOf(primitive.content) else -1
        require(choice >= 0) { "Invalid console language: $value" }
        return spec.nativeEnumValues[choice]
    }

    fun resolve(global: RuntimeProfile, game: RuntimeProfile): Int =
        language(game.values[ID] ?: global.values[ID])

    fun displayValue(value: JsonElement?): JsonPrimitive = runCatching {
        JsonPrimitive(spec.choices[spec.nativeEnumValues.indexOf(language(value))])
    }.getOrElse { spec.defaultValue as JsonPrimitive }
}
