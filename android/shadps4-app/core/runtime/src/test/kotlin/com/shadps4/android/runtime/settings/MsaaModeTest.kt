package com.shadps4.android.runtime.settings

import kotlinx.serialization.json.JsonPrimitive
import org.junit.Assert.*
import org.junit.Test

class MsaaModeTest {
    private fun mode(value: String) = RuntimeProfile(values = mapOf(MsaaMode.ID to JsonPrimitive(value)))
    private fun legacy(value: Boolean) = RuntimeProfile(values = mapOf(MsaaMode.LEGACY_ID to JsonPrimitive(value)))

    @Test fun defaultIsTheGameCountUnderTheHostLimit() {
        assertEquals(MsaaMode.Options(disabled = false, maxSamples = 0), MsaaMode.resolve(RuntimeProfile(), RuntimeProfile()))
        assertEquals(MsaaMode.Options(disabled = true, maxSamples = 0), MsaaMode.resolve(mode("off"), RuntimeProfile()))
        assertEquals(MsaaMode.Options(disabled = false, maxSamples = 2), MsaaMode.resolve(RuntimeProfile(), mode("2x")))
        assertEquals(MsaaMode.GAME, MsaaMode.choice(mode("off"), mode("game")))
    }

    @Test fun earlierBooleanFlagStillAppliesUntilReplaced() {
        assertEquals(MsaaMode.OFF, MsaaMode.choice(legacy(true), RuntimeProfile()))
        assertEquals(MsaaMode.GAME, MsaaMode.choice(legacy(true), legacy(false)))
        assertEquals(MsaaMode.OFF, MsaaMode.choice(RuntimeProfile(), legacy(true)))
        // The new id wins within the same profile; the game profile wins over global.
        val both = RuntimeProfile(values = mapOf(MsaaMode.LEGACY_ID to JsonPrimitive(true), MsaaMode.ID to JsonPrimitive("2x")))
        assertEquals(MsaaMode.X2, MsaaMode.choice(RuntimeProfile(), both))
        assertEquals(MsaaMode.OFF, MsaaMode.choice(mode("2x"), legacy(true)))
    }

    @Test fun catalogOffersNoEightSampleChoice() {
        val spec = RuntimeSettingCatalog.loadAndroidSettings().single { it.id == MsaaMode.ID }
        assertEquals(SettingKind.ENUM, spec.kind)
        assertEquals(MsaaMode.CHOICES, spec.choices)
        assertEquals(JsonPrimitive(MsaaMode.GAME), spec.defaultValue)
        assertFalse(RuntimeSettingCatalog.loadAndroidSettings().any { it.id == MsaaMode.LEGACY_ID })
    }

    @Test fun desktopConfigImportKeepsForceDisable() {
        val catalog = RuntimeSettingCatalog.loadFromResources()
        for (value in listOf(true, false)) {
            val imported = ShadPs4JsonCodec.applyRawJson(RuntimeProfile(),
                """{"GPU":{"force_disable_msaa":$value}}""", catalog.shadPs4)
            assertEquals(value, MsaaMode.resolve(imported, RuntimeProfile()).disabled)
        }
    }

    @Test fun invalidValuesAreRejected() {
        assertThrows(IllegalArgumentException::class.java) { MsaaMode.resolve(mode("8x"), RuntimeProfile()) }
        val broken = RuntimeProfile(values = mapOf(MsaaMode.LEGACY_ID to JsonPrimitive("broken")))
        assertThrows(IllegalArgumentException::class.java) { MsaaMode.resolve(broken, RuntimeProfile()) }
    }
}
