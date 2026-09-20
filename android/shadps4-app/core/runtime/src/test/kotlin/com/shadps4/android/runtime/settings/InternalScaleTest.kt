package com.shadps4.android.runtime.settings

import kotlinx.serialization.json.JsonPrimitive
import org.junit.Assert.assertEquals
import org.junit.Assert.assertThrows
import org.junit.Test

class InternalScaleTest {
    private fun profile(value: String) = RuntimeProfile(
        values = mapOf(InternalScale.ID to JsonPrimitive(value)),
    )

    @Test fun missingValueUsesHalfScale() {
        assertEquals(50f, InternalScale.resolve(RuntimeProfile(), RuntimeProfile()))
        val spec = RuntimeSettingCatalog.loadFromResources().shadPs4.single { it.id == InternalScale.ID }
        assertEquals(JsonPrimitive("0.5"), spec.defaultValue)
    }

    @Test fun catalogChoicesReachPercentages() {
        val spec = RuntimeSettingCatalog.loadFromResources().shadPs4.single { it.id == InternalScale.ID }
        assertEquals("GPU.internal_scale_percent", spec.nativeKey)
        spec.choices.forEachIndexed { index, choice ->
            assertEquals(listOf(25f, 37.5f, 50f, 75f, 100f)[index], InternalScale.resolve(profile(choice), RuntimeProfile()))
        }
    }

    @Test fun gameOverrideWinsAndRemovingItRestoresGlobal() {
        val global = profile("0.5")
        assertEquals(75f, InternalScale.resolve(global, profile("0.75")))
        assertEquals(100f, InternalScale.resolve(global, profile("1.0")))
        assertEquals(50f, InternalScale.resolve(global, RuntimeProfile()))
    }

    @Test fun nativePercentImportPreservesUiChoice() {
        val specs = RuntimeSettingCatalog.loadFromResources().shadPs4
        for ((percent, choice) in listOf(25 to "0.25", 37.5 to "0.375", 50 to "0.5", 75 to "0.75", 100 to "1.0")) {
            val p = ShadPs4JsonCodec.applyRawJson(RuntimeProfile(),
                """{"GPU":{"internal_scale_percent":$percent}}""", specs)
            assertEquals(JsonPrimitive(choice), p.values[InternalScale.ID])
        }
        assertThrows(IllegalArgumentException::class.java) {
            ShadPs4JsonCodec.applyRawJson(RuntimeProfile(),
                """{"GPU":{"internal_scale_percent":42}}""", specs)
        }
    }

    @Test fun malformedChoiceIsRejected() {
        assertThrows(IllegalArgumentException::class.java) {
            InternalScale.resolve(profile("invalid"), RuntimeProfile())
        }
    }
}
