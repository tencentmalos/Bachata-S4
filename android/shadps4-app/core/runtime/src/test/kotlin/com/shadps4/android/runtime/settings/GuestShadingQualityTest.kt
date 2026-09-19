package com.shadps4.android.runtime.settings

import kotlinx.serialization.json.JsonPrimitive
import org.junit.Assert.assertEquals
import org.junit.Assert.assertThrows
import org.junit.Test

class GuestShadingQualityTest {
    private fun profile(value: String) = RuntimeProfile(
        values = mapOf(GuestShadingQuality.ID to JsonPrimitive(value)),
    )

    @Test fun missingValueUsesFullRate() {
        assertEquals(2, GuestShadingQuality.resolve(RuntimeProfile(), RuntimeProfile()))
    }

    @Test fun catalogChoicesReachNativeOrdinals() {
        val spec = RuntimeSettingCatalog.loadFromResources().shadPs4.single { it.id == GuestShadingQuality.ID }
        assertEquals("GPU.fdm_quality", spec.nativeKey)
        spec.choices.forEachIndexed { index, choice ->
            assertEquals(index, GuestShadingQuality.resolve(profile(choice), RuntimeProfile()))
        }
    }

    @Test fun gameOverrideWinsAndRemovingItRestoresGlobal() {
        val global = profile("Low (1/4)")
        assertEquals(1, GuestShadingQuality.resolve(global, profile("Medium (1/2)")))
        assertEquals(2, GuestShadingQuality.resolve(global, profile("High (1/1)")))
        assertEquals(0, GuestShadingQuality.resolve(global, RuntimeProfile()))
    }

    @Test fun malformedChoiceIsRejected() {
        assertThrows(IllegalArgumentException::class.java) {
            GuestShadingQuality.resolve(profile("invalid"), RuntimeProfile())
        }
    }
}
