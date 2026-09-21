package com.shadps4.android.runtime.settings

import kotlinx.serialization.json.JsonPrimitive
import org.junit.Assert.*
import org.junit.Test

class ForceDisableMsaaTest {
    private fun profile(value: Boolean) = RuntimeProfile(values = mapOf(ForceDisableMsaa.ID to JsonPrimitive(value)))

    @Test fun defaultAndGameOverridePreserveExplicitFalse() {
        assertFalse(ForceDisableMsaa.resolve(RuntimeProfile(), RuntimeProfile()))
        assertTrue(ForceDisableMsaa.resolve(profile(true), RuntimeProfile()))
        assertFalse(ForceDisableMsaa.resolve(profile(true), profile(false)))
        assertTrue(ForceDisableMsaa.resolve(profile(false), profile(true)))
    }

    @Test fun persistedProfilesAndBothCatalogsAgree() {
        val catalog = RuntimeSettingCatalog.loadFromResources()
        val spec = catalog.shadPs4.single { it.id == ForceDisableMsaa.ID }
        assertEquals(spec, RuntimeSettingCatalog.loadAndroidSettings().single { it.id == ForceDisableMsaa.ID })
        assertEquals(JsonPrimitive(false), spec.defaultValue)
        assertTrue(spec.restartRequired)
        assertEquals(SettingScope.GLOBAL_AND_GAME, spec.scope)
        for (value in listOf(false, true)) {
            val imported = ShadPs4JsonCodec.applyRawJson(RuntimeProfile(),
                """{"GPU":{"force_disable_msaa":$value}}""", catalog.shadPs4)
            assertEquals(value, ForceDisableMsaa.resolve(imported, RuntimeProfile()))
        }
    }

    @Test fun invalidValueIsRejected() {
        val broken = RuntimeProfile(values = mapOf(ForceDisableMsaa.ID to JsonPrimitive("broken")))
        assertThrows(IllegalArgumentException::class.java) { ForceDisableMsaa.resolve(broken, RuntimeProfile()) }
    }
}
