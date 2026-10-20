package com.shadps4.android.runtime.settings

import kotlinx.serialization.json.JsonPrimitive
import org.junit.Assert.*
import org.junit.Test

class StreamSubmitTest {
    @Test fun offByDefaultAndGameOverridesGlobal() {
        val empty = RuntimeProfile()
        assertEquals(0, StreamSubmit.resolve(empty, empty))
        val global = RuntimeProfile(values = mapOf(StreamSubmit.ID to JsonPrimitive("on")))
        assertEquals(StreamSubmit.ON_DRAWS, StreamSubmit.resolve(global, empty))
        val game = RuntimeProfile(values = mapOf(StreamSubmit.ID to JsonPrimitive("off")))
        assertEquals(0, StreamSubmit.resolve(global, game))
        assertThrows(IllegalArgumentException::class.java) {
            StreamSubmit.resolve(empty, RuntimeProfile(values = mapOf(StreamSubmit.ID to JsonPrimitive("100"))))
        }
    }

    @Test fun catalogEntryMatchesTheResolver() {
        val spec = RuntimeSettingCatalog.loadAndroidSettings().single { it.id == StreamSubmit.ID }
        assertEquals(SettingScope.GLOBAL_AND_GAME, spec.scope)
        assertEquals(JsonPrimitive("off"), spec.defaultValue)
        assertTrue(spec.restartRequired)
        for (choice in spec.choices) {
            val profile = RuntimeProfile(values = mapOf(StreamSubmit.ID to JsonPrimitive(choice)))
            assertEquals(if (choice == "on") StreamSubmit.ON_DRAWS else 0,
                         StreamSubmit.resolve(RuntimeProfile(), profile))
        }
    }
}
