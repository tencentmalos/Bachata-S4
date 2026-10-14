package com.shadps4.android.runtime.settings

import kotlinx.serialization.json.JsonPrimitive
import org.junit.Assert.*
import org.junit.Test

class FrameLimitTest {
    @Test fun defaultsToTheGameRateAndGameOverridesGlobal() {
        val empty = RuntimeProfile()
        assertEquals(0, FrameLimit.resolve(empty, empty))
        val global = RuntimeProfile(values = mapOf(FrameLimit.ID to JsonPrimitive("30")))
        assertEquals(30, FrameLimit.resolve(global, empty))
        val game = RuntimeProfile(values = mapOf(FrameLimit.ID to JsonPrimitive("off")))
        assertEquals(0, FrameLimit.resolve(global, game))
        assertThrows(IllegalArgumentException::class.java) {
            FrameLimit.resolve(empty, RuntimeProfile(values = mapOf(FrameLimit.ID to JsonPrimitive("33"))))
        }
    }

    @Test fun catalogChoicesMatchTheResolver() {
        val spec = RuntimeSettingCatalog.loadAndroidSettings().single { it.id == FrameLimit.ID }
        assertEquals(SettingScope.GLOBAL_AND_GAME, spec.scope)
        for (choice in spec.choices) {
            val profile = RuntimeProfile(values = mapOf(FrameLimit.ID to JsonPrimitive(choice)))
            val fps = FrameLimit.resolve(RuntimeProfile(), profile)
            assertEquals(if (choice == "off") 0 else choice.toInt(), fps)
        }
    }
}
