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
        assertEquals(50, InternalScale.resolve(RuntimeProfile(), RuntimeProfile()))
        val spec = RuntimeSettingCatalog.loadFromResources().shadPs4.single { it.id == InternalScale.ID }
        assertEquals(JsonPrimitive("0.5"), spec.defaultValue)
    }

    @Test fun catalogChoicesReachPercentages() {
        val spec = RuntimeSettingCatalog.loadFromResources().shadPs4.single { it.id == InternalScale.ID }
        assertEquals("GPU.internal_scale_percent", spec.nativeKey)
        spec.choices.forEachIndexed { index, choice ->
            assertEquals(listOf(50, 75, 100)[index], InternalScale.resolve(profile(choice), RuntimeProfile()))
        }
    }

    @Test fun gameOverrideWinsAndRemovingItRestoresGlobal() {
        val global = profile("0.5")
        assertEquals(75, InternalScale.resolve(global, profile("0.75")))
        assertEquals(100, InternalScale.resolve(global, profile("1.0")))
        assertEquals(50, InternalScale.resolve(global, RuntimeProfile()))
    }

    @Test fun nativePercentImportPreservesUiChoice() {
        val specs = RuntimeSettingCatalog.loadFromResources().shadPs4
        for ((percent, choice) in listOf(50 to "0.5", 75 to "0.75", 100 to "1.0")) {
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
