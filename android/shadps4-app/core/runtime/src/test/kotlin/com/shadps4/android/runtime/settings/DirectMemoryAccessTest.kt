package com.shadps4.android.runtime.settings

import kotlinx.serialization.json.JsonPrimitive
import org.junit.Assert.*
import org.junit.Test

class DirectMemoryAccessTest {
    private fun profile(value: Boolean) = RuntimeProfile(values = mapOf(DirectMemoryAccess.ID to JsonPrimitive(value)))

    @Test fun defaultAndGameOverridePreserveExplicitFalse() {
        assertFalse(DirectMemoryAccess.resolve(RuntimeProfile(), RuntimeProfile()))
        assertTrue(DirectMemoryAccess.resolve(profile(true), RuntimeProfile()))
        assertFalse(DirectMemoryAccess.resolve(profile(true), profile(false)))
        assertTrue(DirectMemoryAccess.resolve(profile(false), profile(true)))
    }

    @Test fun persistedProfilesAndBothCatalogsAgree() {
        val catalog = RuntimeSettingCatalog.loadFromResources()
        val spec = catalog.shadPs4.single { it.id == DirectMemoryAccess.ID }
        assertEquals(spec, RuntimeSettingCatalog.loadAndroidSettings().single { it.id == DirectMemoryAccess.ID })
        assertEquals(JsonPrimitive(false), spec.defaultValue)
        assertTrue(spec.restartRequired)
        assertEquals(SettingScope.GLOBAL_AND_GAME, spec.scope)
        for (value in listOf(false, true)) {
            val imported = ShadPs4JsonCodec.applyRawJson(RuntimeProfile(),
                """{"GPU":{"direct_memory_access_enabled":$value}}""", catalog.shadPs4)
            assertEquals(value, DirectMemoryAccess.resolve(imported, RuntimeProfile()))
        }
    }

    @Test fun invalidValueIsRejected() {
        val broken = RuntimeProfile(values = mapOf(DirectMemoryAccess.ID to JsonPrimitive("broken")))
        assertThrows(IllegalArgumentException::class.java) { DirectMemoryAccess.resolve(broken, RuntimeProfile()) }
    }
}
