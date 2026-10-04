package com.shadps4.android.runtime.settings

import java.io.File
import kotlinx.serialization.json.Json
import kotlinx.serialization.json.JsonArray
import kotlinx.serialization.json.JsonElement
import kotlinx.serialization.json.JsonPrimitive
import kotlinx.serialization.json.jsonArray
import kotlinx.serialization.json.jsonObject
import kotlinx.serialization.json.jsonPrimitive

/**
 * Guest function packages (C/C++ game patches) selected per game, in install order: file stems
 * of host/guest_patches/<TITLE_ID>/<name>.json. The native runtime skips a selected package that
 * does not match the game, or that changes code an earlier selected package changes.
 */
object GuestPatches {
    const val ID = "general.guest_patches"

    fun selection(raw: JsonElement?): List<String> {
        if (raw == null) return emptyList()
        val array = requireNotNull(raw as? JsonArray) { "Invalid $ID: $raw" }
        return array.map { item ->
            val name = (item as? JsonPrimitive)?.takeIf { it.isString }?.content
            requireNotNull(name) { "Invalid $ID entry: $item" }
        }.distinct()
    }

    fun resolve(global: RuntimeProfile, game: RuntimeProfile): List<String> =
        selection(game.values[ID] ?: global.values[ID])

    fun encode(names: List<String>): JsonElement = JsonArray(names.map(::JsonPrimitive))

    fun directory(filesDir: File, titleId: String): File = File(filesDir, "host/guest_patches/$titleId")
}

/** A package file of one game, as the native package loader lists it. */
data class GuestPatchPackage(
    /** File stem, the value stored in the selection. */
    val name: String,
    val label: String,
    val description: String = "",
    val module: String = "",
    /** Other packages of the game that change the same code: only one of them can be on. */
    val conflicts: List<String> = emptyList(),
    /** Selected, but no valid package file has this name. */
    val missing: Boolean = false,
)

object GuestPatchCatalog {
    private val json = Json { ignoreUnknownKeys = true }

    /** Parses NativeFexSession.nativeListGuestPatches output. */
    fun parse(text: String): List<GuestPatchPackage> {
        val packages = json.parseToJsonElement(text).jsonObject["packages"]?.jsonArray.orEmpty()
        return packages.map { element ->
            val entry = element.jsonObject
            fun text(key: String) = entry[key]?.jsonPrimitive?.content.orEmpty()
            GuestPatchPackage(
                name = text("name"),
                label = text("label").ifEmpty { text("name") },
                description = text("description"),
                module = text("module"),
                conflicts = entry["conflicts"]?.jsonArray.orEmpty().map { it.jsonPrimitive.content },
            )
        }.filter { it.name.isNotEmpty() }
    }
}
