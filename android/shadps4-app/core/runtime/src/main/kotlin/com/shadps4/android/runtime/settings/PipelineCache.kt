package com.shadps4.android.runtime.settings

import kotlinx.serialization.json.JsonPrimitive
import kotlinx.serialization.json.booleanOrNull

/** Both captured before Vulkan creation; each layer can be switched off on its own. */
object PipelineCache {
    /** Guest shader translations and pipeline recipes, rebuilt at startup. */
    const val ID = "vulkan.pipeline_cache_enabled"
    /** The driver's VkPipelineCache file. */
    const val DRIVER_ID = "vulkan.driver_pipeline_cache"
    /** Guest command buffers replayed on a separate recording thread. */
    const val RECORDER_ID = "vulkan.command_recorder"

    /** How new pipelines are created: 0 sync, 1 async_accurate, 2 async_graphics_skip (JNI). */
    const val COMPILE_MODE_ID = "vulkan.pipeline_compile_mode"
    private val compileModes = mapOf("sync" to 0, "async_accurate" to 1, "async_graphics_skip" to 2)
    const val DEFAULT_COMPILE_MODE = 2

    fun resolve(global: RuntimeProfile, game: RuntimeProfile): Boolean = resolve(ID, global, game)
    fun resolveCompileMode(global: RuntimeProfile, game: RuntimeProfile): Int {
        val value = game.values[COMPILE_MODE_ID] ?: global.values[COMPILE_MODE_ID] ?: return DEFAULT_COMPILE_MODE
        val choice = (value as? JsonPrimitive)?.content
        return requireNotNull(compileModes[choice]) { "Invalid pipeline compile mode: $choice" }
    }
    fun resolveDriver(global: RuntimeProfile, game: RuntimeProfile): Boolean =
        resolve(DRIVER_ID, global, game)
    fun resolveRecorder(global: RuntimeProfile, game: RuntimeProfile): Boolean =
        resolve(RECORDER_ID, global, game)

    private fun resolve(id: String, global: RuntimeProfile, game: RuntimeProfile): Boolean {
        val value = game.values[id] ?: global.values[id] ?: return true
        return requireNotNull((value as? JsonPrimitive)?.booleanOrNull) { "Invalid $id setting: $value" }
    }
}
