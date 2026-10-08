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
    @Test fun msaaRowIsOfferedForEveryGameAndReplacesTheEarlierFlag() {
        for (options in listOf(GameLaunchOptions("2d", loaded = true), GameLaunchOptions("psvr", loaded = true, psvr = true))) {
            val spec = options.specs().single { it.id == MsaaMode.ID }
            assertEquals(listOf("off", "2x", "game"), spec.choices)
            assertEquals("game", options.value(spec))
        }
        val earlier = RuntimeProfile(values = mapOf(MsaaMode.LEGACY_ID to JsonPrimitive(true)))
        val options = GameLaunchOptions("bs", loaded = true, game = earlier)
        val spec = options.specs().single { it.id == MsaaMode.ID }
        assertEquals("off", options.value(spec))
        val edited = options.select(spec, "2x")
        assertEquals("2x", edited.value(spec))
        val saved = edited.applyTo(earlier)
        assertFalse(MsaaMode.LEGACY_ID in saved.values)
        assertEquals(MsaaMode.Options(disabled = false, maxSamples = 2), MsaaMode.resolve(RuntimeProfile(), saved))
        assertThrows(IllegalArgumentException::class.java) { options.select(spec, "8x") }
        // Saving other rows keeps the earlier flag.
        val scale = options.specs().single { it.id == InternalScale.ID }
        assertEquals(JsonPrimitive(true), options.select(scale, scale.choices.first()).applyTo(earlier).values[MsaaMode.LEGACY_ID])
    }
    @Test fun psvrXrOffersDualShock4PlacementAndSavesItPerGame() {
        val options = GameLaunchOptions("psvr", loaded = true, psvr = true)
        val spec = options.specs().single { it.id == XrDs4Pose.ID }
        assertEquals("off", options.value(spec))
        assertEquals(listOf("off", "right", "both", "hands"), spec.choices)
        val saved = options.select(spec, "right").applyTo(RuntimeProfile())
        assertEquals(XrDs4Pose.RIGHT, XrDs4Pose.resolve(RuntimeProfile(), saved))
        val cinema = GameLaunchOptions("cinema", loaded = true, game = RuntimeProfile(
            values = mapOf(DisplayMode.ID to JsonPrimitive("xr"))))
        assertFalse(cinema.specs().any { it.id == XrDs4Pose.ID })
        assertFalse(GameLaunchOptions("2d", loaded = true).specs().any { it.id == XrDs4Pose.ID })
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
    @Test fun guestPatchesToggleInPanelOrderAndBlockConflicts() {
        val fps = GuestPatchPackage("bloodborne_60fps_v1", "60 FPS", conflicts = listOf("other_60fps"))
        val other = GuestPatchPackage("other_60fps", "Other 60 FPS", conflicts = listOf("bloodborne_60fps_v1"))
        val sound = GuestPatchPackage("bloodborne_sound_fix_v1", "Sound bank reload fix")
        val original = RuntimeProfile(values = mapOf(
            GuestPatches.ID to GuestPatches.encode(listOf("bloodborne_sound_fix_v1", "gone"))))
        var options = GameLaunchOptions("CUSA03023", loaded = true, game = original,
            patches = listOf(fps, sound, other))
        // Settings rows, then the Guest Patches row; the panel lists every package plus the
        // selected name whose file is gone, so it can still be switched off.
        assertEquals(options.specs().size + 1, options.rowCount(patchPanel = false))
        assertEquals(4, options.rowCount(patchPanel = true))
        assertEquals("File not found", options.patchBlocker(options.patchChoices().last()))
        options = options.togglePatch("bloodborne_60fps_v1")
        // Install order is the panel order, not the click order.
        assertEquals(listOf("bloodborne_60fps_v1", "bloodborne_sound_fix_v1", "gone"), options.patchSelection())
        assertEquals("Changes the same code as 60 FPS", options.patchBlocker(other))
        assertSame(options, options.togglePatch("other_60fps"))
        options = options.togglePatch("gone").togglePatch("bloodborne_sound_fix_v1")
        assertEquals(listOf("bloodborne_60fps_v1"), options.patchSelection())
        assertSame(options, options.togglePatch("gone")) // a missing file cannot come back
        val saved = options.applyTo(original)
        assertEquals(listOf("bloodborne_60fps_v1"), GuestPatches.resolve(RuntimeProfile(), saved))
        assertEquals(listOf("bloodborne_sound_fix_v1", "gone"), GuestPatches.resolve(RuntimeProfile(), original))
        assertEquals(emptyList<String>(), GuestPatches.resolve(RuntimeProfile(), RuntimeProfile()))
        assertEquals(options.specs().size, GameLaunchOptions("none", loaded = true).rowCount(patchPanel = false))
    }
    @Test fun guestPatchCatalogReadsTheNativeListing() {
        val listed = GuestPatchCatalog.parse("""{"packages":[{"name":"a","id":"pkg_a","label":"A",
            "description":"does A","module":"eboot.bin","module_sha256":"00","conflicts":["b"]},
            {"name":"b","id":"pkg_b","label":"","description":"","module":"eboot.bin",
            "module_sha256":"00","conflicts":[]}],"skipped":["c.json: invalid"]}""")
        assertEquals(listOf(GuestPatchPackage("a", "A", "does A", "eboot.bin", listOf("b")),
            GuestPatchPackage("b", "b", "", "eboot.bin")), listed)
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
    @Test fun psvrCanSwitchToSbsWindowForDevicesWithoutOpenXr() {
        var options = GameLaunchOptions("psvr", loaded = true, psvr = true)
        val spec = options.specs().single { it.id == DisplayMode.PSVR_ID }
        assertEquals(listOf("xr", "sbs"), spec.choices)
        assertEquals("xr", options.value(spec))
        options = options.select(spec, "sbs")
        assertFalse(options.xr)
        // A 2D window: the ordinary scale and screen upscaler, no XR rendering rows.
        assertTrue(options.specs().any { it.id == InternalScale.ID })
        assertTrue(options.specs().any { it.id == XrRendering.SCREEN_UPSCALER })
        assertFalse(options.specs().any { it.id == XrRendering.OUTPUT })
        assertFalse(options.specs().any { it.id == DisplayMode.ID })
        val saved = options.applyTo(RuntimeProfile())
        assertEquals(JsonPrimitive("sbs"), saved.values[DisplayMode.PSVR_ID])
        // Ordinary games do not offer it.
        assertFalse(GameLaunchOptions("2d", loaded = true).specs().any { it.id == DisplayMode.PSVR_ID })
    }
}
