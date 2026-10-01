package com.shadps4.android.feature.library

import com.shadps4.android.runtime.settings.*
import kotlinx.serialization.json.JsonPrimitive
import org.junit.Assert.*
import org.junit.Test

class GameLaunchOptionsTest {
    @Test fun statusOptionsMigrateBooleanAndSaveLayoutPerGameWithoutChangingOnCancel() {
        val global = RuntimeProfile(values = mapOf(XrRendering.STATUS to JsonPrimitive(false)))
        val original = RuntimeProfile()
        val options = GameLaunchOptions("psvr", loaded = true, psvr = true, global = global, game = original)
        val spec = options.specs().single { it.id == XrRendering.STATUS }
        assertEquals(SettingKind.ENUM, spec.kind)
        assertEquals("none", options.value(spec))
        assertEquals(listOf("horizontal", "vertical", "none"), spec.choices) // controller adjustment too
        val edited = options.select(spec, "vertical")
        assertEquals("none", options.value(spec))
        assertTrue(original.values.isEmpty())
        val saved = edited.applyTo(original)
        assertEquals(JsonPrimitive("vertical"), saved.values[XrRendering.STATUS])
        assertEquals(1, XrRendering.resolve(global, saved, true).statusLayer)
        assertFalse(GameLaunchOptions("2d", loaded = true).specs().any { it.id == XrRendering.STATUS })
        val cinema = GameLaunchOptions("cinema", loaded = true, game = RuntimeProfile(
            values = mapOf(DisplayMode.ID to JsonPrimitive("xr"))))
        assertTrue(cinema.specs().any { it.id == XrRendering.STATUS })
    }
    @Test fun outputLabelsUseQueriedPerEyeSizesAndDoNotInventMissingValues() {
        val options = GameLaunchOptions("psvr", loaded = true, psvr = true,
            outputExtents = listOf(2592 to 2400, 3376 to 2976, 4160 to 3552))
        val spec = options.specs().single { it.id == XrRendering.OUTPUT }
        assertEquals("Recommended\n2592 × 2400", options.choiceLabel(spec, "recommended"))
        assertEquals("High\n3376 × 2976", options.choiceLabel(spec, "high"))
        assertEquals("Ultra\n4160 × 3552", options.choiceLabel(spec, "maximum"))
        assertEquals("Ultra", options.copy(outputExtents = emptyList()).choiceLabel(spec, "maximum"))
    }
    @Test fun ordinaryGameCanChooseCinemaAndKeepBothScalePreferences() {
        val original = RuntimeProfile(values = mapOf(
            InternalScale.ID to JsonPrimitive("0.5"),
            InternalScale.XR_ID to JsonPrimitive("1.0"),
            "unrelated" to JsonPrimitive(true)))
        var options = GameLaunchOptions("ordinary", loaded = true, game = original)
        assertFalse(options.xr)
        assertTrue(options.specs().any { it.id == InternalScale.ID })
        assertTrue(options.specs().any { it.id == XrRendering.SCREEN_UPSCALER })
        assertFalse(options.specs().any { it.id == XrRendering.UPSCALER })
        options = options.select(options.specs().single { it.id == DisplayMode.ID }, "xr")
        assertTrue(options.xr)
        assertFalse(options.specs().any { it.id == InternalScale.ID })
        assertTrue(options.specs().any { it.id == InternalScale.XR_ID })
        assertFalse(options.specs().any { it.id == XrRendering.SCREEN_UPSCALER })
        options = options.select(options.specs().single { it.id == XrRendering.UPSCALER }, "sgsr1")
        val saved = options.applyTo(original)
        assertEquals(JsonPrimitive("0.5"), saved.values[InternalScale.ID])
        assertEquals(JsonPrimitive("1.0"), saved.values[InternalScale.XR_ID])
        assertEquals(JsonPrimitive(true), saved.values["unrelated"])
        assertEquals(2, XrRendering.resolve(RuntimeProfile(), saved, true).upscaler)
        assertFalse(original.values.containsKey(DisplayMode.ID)) // cancel leaves the original intact
    }
    @Test fun psvrAlwaysUsesXrAndDraftCommitDoesNotClobberOtherUpdates() {
        val original = RuntimeProfile(values = mapOf(DisplayMode.ID to JsonPrimitive("2d")))
        var options = GameLaunchOptions("psvr", loaded = true, psvr = true, game = original)
        assertTrue(options.xr)
        assertFalse(options.specs().any { it.id == DisplayMode.ID })
        options = options.select(options.specs().single { it.id == XrRendering.OUTPUT }, "high")
        val concurrent = original.copy(values = original.values + ("another" to JsonPrimitive(7)))
        val saved = options.applyTo(concurrent)
        assertEquals(JsonPrimitive(7), saved.values["another"])
        assertEquals(JsonPrimitive("2d"), saved.values[DisplayMode.ID])
        assertEquals(1, XrRendering.resolve(RuntimeProfile(), saved, true).outputResolution)
        assertFalse(options.copy(saving = true).ready)
    }
}
