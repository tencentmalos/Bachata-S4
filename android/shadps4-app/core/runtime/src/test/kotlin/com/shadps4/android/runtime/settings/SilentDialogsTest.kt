package com.shadps4.android.runtime.settings

import kotlinx.serialization.json.JsonPrimitive
import org.junit.Assert.*
import org.junit.Test

class SilentDialogsTest {
    private fun profile(value: Boolean) = RuntimeProfile(values = mapOf(SilentDialogs.ID to JsonPrimitive(value)))
    @Test fun defaultAndOverrides() {
        assertTrue(SilentDialogs.resolve(RuntimeProfile(), RuntimeProfile()))
        assertFalse(SilentDialogs.resolve(profile(false), RuntimeProfile()))
        assertFalse(SilentDialogs.resolve(profile(true), profile(false)))
        assertTrue(SilentDialogs.resolve(profile(false), profile(true)))
    }
    @Test fun catalogsDescribeTheSameDefault() {
        val spec = RuntimeSettingCatalog.loadFromResources().shadPs4.single { it.id == SilentDialogs.ID }
        assertEquals(spec, RuntimeSettingCatalog.loadAndroidSettings().single { it.id == SilentDialogs.ID })
        assertEquals(JsonPrimitive(true), spec.defaultValue)
        assertTrue(spec.restartRequired)
    }
    @Test fun invalidValuesAreRejected() {
        val broken = RuntimeProfile(values = mapOf(SilentDialogs.ID to JsonPrimitive("broken")))
        assertThrows(IllegalArgumentException::class.java) { SilentDialogs.resolve(broken, RuntimeProfile()) }
    }
}
