package com.shadps4.android.runtime.settings
import kotlinx.serialization.json.JsonPrimitive
import org.junit.Assert.*
import org.junit.Test
class XrRenderingTest {
    @Test fun statusLayoutsMigrateBooleanUseGameOverrideAndDisableOutsideXr() {
        val enabled = RuntimeProfile(values = mapOf(XrRendering.STATUS to JsonPrimitive(true)))
        val disabled = RuntimeProfile(values = mapOf(XrRendering.STATUS to JsonPrimitive(false)))
        assertEquals(2, XrRendering.resolve(enabled, disabled, true).statusLayer)
        assertEquals(0, XrRendering.resolve(disabled, enabled, true).statusLayer)
        assertEquals(2, XrRendering.resolve(enabled, enabled, false).statusLayer)
        for ((index, choice) in listOf("horizontal", "vertical", "none").withIndex()) {
            val selected = RuntimeProfile(values = mapOf(XrRendering.STATUS to JsonPrimitive(choice)))
            assertEquals(index, XrRendering.resolve(enabled, selected, true).statusLayer)
        }
        assertThrows(IllegalArgumentException::class.java) {
            XrRendering.resolve(RuntimeProfile(), RuntimeProfile(values = mapOf(
                XrRendering.STATUS to JsonPrimitive("broken"))), true)
        }
    }
    @Test fun defaultsAndScopes() {
        val empty=RuntimeProfile()
        assertEquals(XrRendering.Options(),XrRendering.resolve(empty,empty,true))
        assertEquals(0,XrRendering.resolve(empty,empty,false).upscaler)
        val global=RuntimeProfile(values=mapOf(XrRendering.UPSCALER to JsonPrimitive("sgsr1"),
            XrRendering.FOVEATION to JsonPrimitive("fixed")))
        val game=RuntimeProfile(values=mapOf(XrRendering.UPSCALER to JsonPrimitive("off"),
            XrRendering.LEVEL to JsonPrimitive("high"),XrRendering.SHARPNESS to JsonPrimitive("100")))
        assertEquals(XrRendering.Options(0,1,2,100),XrRendering.resolve(global,game,true))
        assertEquals(0,XrRendering.resolve(global,game,false).foveation)
        for(id in listOf(XrRendering.OUTPUT,XrRendering.UPSCALER,XrRendering.FOVEATION,XrRendering.LEVEL,XrRendering.SHARPNESS)) {
            val spec=RuntimeSettingCatalog.loadAndroidSettings().single{it.id==id}
            assertEquals("GPU",spec.category)
            assertTrue(spec.restartRequired)
            assertEquals(SettingScope.GLOBAL_AND_GAME,spec.scope)
        }
    }
    @Test fun outputResolutionIsIndependentOfGuestScaleAndUsesGameOverride() {
        val high = RuntimeProfile(values=mapOf(XrRendering.OUTPUT to JsonPrimitive("high")))
        val normal = RuntimeProfile(values=mapOf(XrRendering.OUTPUT to JsonPrimitive("recommended")))
        assertEquals(1, XrRendering.resolve(high,RuntimeProfile(),true).outputResolution)
        assertEquals(0, XrRendering.resolve(high,normal,true).outputResolution)
        assertEquals(0, XrRendering.resolve(high,RuntimeProfile(),false).outputResolution)
        assertEquals(100f, InternalScale.resolve(high,RuntimeProfile(),true))
        val ultra = RuntimeProfile(values=mapOf(XrRendering.OUTPUT to JsonPrimitive("maximum")))
        assertEquals(2, XrRendering.resolve(high,ultra,true).outputResolution)
    }
    @Test fun invalidValuesRejected() {
        for(id in listOf(XrRendering.OUTPUT,XrRendering.UPSCALER,XrRendering.FOVEATION,XrRendering.LEVEL,XrRendering.SHARPNESS)) {
            val broken=RuntimeProfile(values=mapOf(id to JsonPrimitive("not-a-choice")))
            assertThrows(IllegalArgumentException::class.java){XrRendering.resolve(RuntimeProfile(),broken,true)}
        }
    }
}
