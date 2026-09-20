package com.shadps4.android.runtime.settings

import kotlinx.serialization.json.JsonPrimitive
import org.junit.Assert.*
import org.junit.Test

class TextureQualityTest {
    private fun profile(value: String) = RuntimeProfile(values = mapOf(TextureQuality.ID to JsonPrimitive(value)))
    @Test fun oldProfilesKeepAssetQualityIndependentOfRenderScale() {
        val old = RuntimeProfile(values = mapOf(InternalScale.ID to JsonPrimitive("0.25")))
        assertEquals(0, TextureQuality.resolve(old, RuntimeProfile()))
        assertEquals(25f, InternalScale.resolve(old, RuntimeProfile()))
        assertEquals(2, TextureQuality.resolve(old, profile("low")))
        assertEquals(25f, InternalScale.resolve(old, profile("low")))
    }
    @Test fun perGameRemovalRestoresGlobalAndMissingRestoresHigh() {
        assertEquals(2, TextureQuality.resolve(profile("medium"), profile("low")))
        assertEquals(1, TextureQuality.resolve(profile("medium"), RuntimeProfile()))
        assertEquals(0, TextureQuality.resolve(RuntimeProfile(), RuntimeProfile()))
    }
    @Test fun nativeStringImportAndBothCatalogsAgree() {
        val catalog = RuntimeSettingCatalog.loadFromResources()
        val spec = catalog.shadPs4.single { it.id == TextureQuality.ID }
        assertEquals(spec, RuntimeSettingCatalog.loadAndroidSettings().single { it.id == TextureQuality.ID })
        assertEquals("GPU.texture_quality", spec.nativeKey)
        assertEquals(JsonPrimitive("high"), spec.defaultValue)
        spec.choices.forEachIndexed { index, choice ->
            val imported = ShadPs4JsonCodec.applyRawJson(RuntimeProfile(),
                """{"GPU":{"texture_quality":"$choice"}}""", catalog.shadPs4)
            assertEquals(index, TextureQuality.resolve(imported, RuntimeProfile()))
        }
        assertThrows(IllegalArgumentException::class.java) {
            ShadPs4JsonCodec.applyRawJson(RuntimeProfile(), """{"GPU":{"texture_quality":"broken"}}""", catalog.shadPs4)
        }
        assertThrows(IllegalArgumentException::class.java) { TextureQuality.resolve(profile("broken"), RuntimeProfile()) }
    }
}
