package com.shadps4.android.runtime.settings

import kotlinx.serialization.json.JsonPrimitive
import org.junit.Assert.*
import org.junit.Test

class TouchpadEmulationTest {
    private fun profile(value: Boolean) = RuntimeProfile(values = mapOf(TouchpadEmulation.ID to JsonPrimitive(value)))

    @Test fun offByDefaultAndPerGame() {
        assertFalse(TouchpadEmulation.resolve(RuntimeProfile(), RuntimeProfile()))
        assertTrue(TouchpadEmulation.resolve(RuntimeProfile(), profile(true)))
        assertFalse(TouchpadEmulation.resolve(profile(true), profile(false)))
        val spec = RuntimeSettingCatalog.loadAndroidSettings().single { it.id == TouchpadEmulation.ID }
        assertEquals(false, (spec.defaultValue as? JsonPrimitive)?.content?.toBooleanStrictOrNull() ?: false)
        assertTrue(spec.restartRequired)
        assertEquals(SettingScope.GLOBAL_AND_GAME, spec.scope)
    }

    @Test fun invalidValueIsRejected() {
        val broken = RuntimeProfile(values = mapOf(TouchpadEmulation.ID to JsonPrimitive("x")))
        assertThrows(IllegalArgumentException::class.java) { TouchpadEmulation.resolve(broken, RuntimeProfile()) }
    }
}
