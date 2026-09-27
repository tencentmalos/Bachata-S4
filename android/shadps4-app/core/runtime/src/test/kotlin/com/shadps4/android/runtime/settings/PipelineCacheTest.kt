package com.shadps4.android.runtime.settings

import kotlinx.serialization.json.JsonPrimitive
import org.junit.Assert.*
import org.junit.Test

class PipelineCacheTest {
    private fun profile(id: String, value: Boolean) = RuntimeProfile(values = mapOf(id to JsonPrimitive(value)))

    @Test fun defaultOnAndGameOverridePreserveExplicitFalse() {
        assertTrue(PipelineCache.resolve(RuntimeProfile(), RuntimeProfile()))
        assertFalse(PipelineCache.resolve(profile(PipelineCache.ID, false), RuntimeProfile()))
        assertTrue(PipelineCache.resolve(profile(PipelineCache.ID, false), profile(PipelineCache.ID, true)))
        assertFalse(PipelineCache.resolve(profile(PipelineCache.ID, true), profile(PipelineCache.ID, false)))
    }

    @Test fun layersAreIndependent() {
        val recipeOff = profile(PipelineCache.ID, false)
        assertFalse(PipelineCache.resolve(recipeOff, RuntimeProfile()))
        assertTrue(PipelineCache.resolveDriver(recipeOff, RuntimeProfile()))
        val driverOff = profile(PipelineCache.DRIVER_ID, false)
        assertTrue(PipelineCache.resolve(driverOff, RuntimeProfile()))
        assertFalse(PipelineCache.resolveDriver(driverOff, RuntimeProfile()))
    }

    @Test fun persistedProfilesAndBothCatalogsAgree() {
        val catalog = RuntimeSettingCatalog.loadFromResources()
        for ((id, key) in listOf(PipelineCache.ID to "pipeline_cache_enabled",
                                 PipelineCache.DRIVER_ID to "driver_pipeline_cache")) {
            val spec = catalog.shadPs4.single { it.id == id }
            assertEquals(spec, RuntimeSettingCatalog.loadAndroidSettings().single { it.id == id })
            assertEquals(JsonPrimitive(true), spec.defaultValue)
            assertTrue(spec.restartRequired)
            assertEquals(SettingScope.GLOBAL_AND_GAME, spec.scope)
            for (value in listOf(false, true)) {
                val imported = ShadPs4JsonCodec.applyRawJson(RuntimeProfile(),
                    """{"Vulkan":{"$key":$value}}""", catalog.shadPs4)
                val resolved = if (id == PipelineCache.ID) PipelineCache.resolve(imported, RuntimeProfile())
                               else PipelineCache.resolveDriver(imported, RuntimeProfile())
                assertEquals(value, resolved)
            }
        }
    }

    @Test fun compileModeDefaultsToSkipAndParsesChoices() {
        assertEquals(2, PipelineCache.resolveCompileMode(RuntimeProfile(), RuntimeProfile()))
        val async = RuntimeProfile(values = mapOf(PipelineCache.COMPILE_MODE_ID to JsonPrimitive("async_accurate")))
        assertEquals(1, PipelineCache.resolveCompileMode(async, RuntimeProfile()))
        val skip = RuntimeProfile(values = mapOf(PipelineCache.COMPILE_MODE_ID to JsonPrimitive("async_graphics_skip")))
        assertEquals(2, PipelineCache.resolveCompileMode(RuntimeProfile(), skip))
        val sync = RuntimeProfile(values = mapOf(PipelineCache.COMPILE_MODE_ID to JsonPrimitive("sync")))
        assertEquals(0, PipelineCache.resolveCompileMode(async, sync))
        val broken = RuntimeProfile(values = mapOf(PipelineCache.COMPILE_MODE_ID to JsonPrimitive("fast")))
        assertThrows(IllegalArgumentException::class.java) { PipelineCache.resolveCompileMode(broken, RuntimeProfile()) }
        val catalog = RuntimeSettingCatalog.loadFromResources()
        val spec = catalog.shadPs4.single { it.id == PipelineCache.COMPILE_MODE_ID }
        assertEquals(spec, RuntimeSettingCatalog.loadAndroidSettings().single { it.id == PipelineCache.COMPILE_MODE_ID })
        assertEquals(JsonPrimitive("async_graphics_skip"), spec.defaultValue)
        assertEquals(listOf("Wait", "Background", "Skip until ready"), spec.choices.map(spec::choiceLabel))
    }

    @Test fun recorderDefaultsOnAndFollowsTheGameProfile() {
        assertTrue(PipelineCache.resolveRecorder(RuntimeProfile(), RuntimeProfile()))
        val off = RuntimeProfile(values = mapOf(PipelineCache.RECORDER_ID to JsonPrimitive(false)))
        assertFalse(PipelineCache.resolveRecorder(off, RuntimeProfile()))
        assertTrue(PipelineCache.resolveRecorder(off, RuntimeProfile(values = mapOf(
            PipelineCache.RECORDER_ID to JsonPrimitive(true)))))
        val catalog = RuntimeSettingCatalog.loadFromResources()
        val spec = catalog.shadPs4.single { it.id == PipelineCache.RECORDER_ID }
        assertEquals(spec, RuntimeSettingCatalog.loadAndroidSettings().single { it.id == PipelineCache.RECORDER_ID })
        assertEquals(JsonPrimitive(true), spec.defaultValue)
    }

    @Test fun invalidValueIsRejected() {
        val broken = RuntimeProfile(values = mapOf(PipelineCache.DRIVER_ID to JsonPrimitive("broken")))
        assertThrows(IllegalArgumentException::class.java) { PipelineCache.resolveDriver(broken, RuntimeProfile()) }
    }
}
