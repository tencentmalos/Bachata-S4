package com.shadps4.android.runtime.settings

import kotlinx.serialization.json.JsonPrimitive
import org.junit.Assert.*
import org.junit.Test

class GpuReadbacksTest {
    private fun profile(mode: JsonPrimitive? = null, linear: Boolean? = null) = RuntimeProfile(
        values = buildMap {
            mode?.let { put(GpuReadbacks.MODE_ID, it) }
            linear?.let { put(GpuReadbacks.LINEAR_ID, JsonPrimitive(it)) }
        },
    )

    @Test fun overridesAndNextLaunchResetBothSettings() {
        val global = profile(JsonPrimitive("Precise"), true)
        assertEquals(GpuReadbacks.Options(2, true), GpuReadbacks.resolve(global, RuntimeProfile()))
        assertEquals(GpuReadbacks.Options(0, false),
            GpuReadbacks.resolve(global, profile(JsonPrimitive("Disabled"), false)))
        assertEquals(GpuReadbacks.Options(1, true),
            GpuReadbacks.resolve(global, profile(JsonPrimitive("Relaxed"))))
        assertEquals(GpuReadbacks.Options(), GpuReadbacks.resolve(RuntimeProfile(), RuntimeProfile()))
    }

    @Test fun legacyOrdinalsAndImportedSettingsAgree() {
        val catalog = RuntimeSettingCatalog.loadFromResources().shadPs4
        val android = RuntimeSettingCatalog.loadAndroidSettings()
        for (id in listOf(GpuReadbacks.MODE_ID, GpuReadbacks.LINEAR_ID)) {
            assertEquals(catalog.single { it.id == id }, android.single { it.id == id })
            assertTrue(android.single { it.id == id }.restartRequired)
        }
        for (mode in 0..2) {
            val old = profile(JsonPrimitive(mode), true)
            val imported = ShadPs4JsonCodec.applyRawJson(RuntimeProfile(),
                """{"GPU":{"readbacks_mode":$mode,"readback_linear_images_enabled":true}}""", catalog)
            assertEquals(GpuReadbacks.Options(mode, true), GpuReadbacks.resolve(old, RuntimeProfile()))
            assertEquals(GpuReadbacks.resolve(old, RuntimeProfile()),
                GpuReadbacks.resolve(imported, RuntimeProfile()))
        }
    }

    @Test fun invalidValuesAreRejected() {
        for (value in listOf(JsonPrimitive(-1), JsonPrimitive(3), JsonPrimitive(1.5), JsonPrimitive("bad"))) {
            assertThrows(IllegalArgumentException::class.java) {
                GpuReadbacks.resolve(profile(value), RuntimeProfile())
            }
        }
        assertThrows(IllegalArgumentException::class.java) {
            GpuReadbacks.resolve(RuntimeProfile(values = mapOf(
                GpuReadbacks.LINEAR_ID to JsonPrimitive("bad"))), RuntimeProfile())
        }
    }
}
