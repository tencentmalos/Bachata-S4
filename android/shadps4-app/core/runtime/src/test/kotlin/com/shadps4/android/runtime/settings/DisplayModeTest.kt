package com.shadps4.android.runtime.settings

import kotlinx.serialization.json.JsonPrimitive
import org.junit.Assert.*
import org.junit.Test

class DisplayModeTest {
    private fun profile(value: String) = RuntimeProfile(values = mapOf(DisplayMode.ID to JsonPrimitive(value)))
    @Test fun defaultAndAllPreferencePsvrCombinations() {
        assertEquals(DisplayMode.Mode.TWO_D, DisplayMode.resolve(RuntimeProfile(), RuntimeProfile(), false).effective)
        for (choice in listOf("2d", "xr")) for (psvr in listOf(false, true)) {
            val global = profile(choice)
            val result = DisplayMode.resolve(global, RuntimeProfile(), psvr)
            assertEquals(if (psvr || choice == "xr") DisplayMode.Mode.XR else DisplayMode.Mode.TWO_D, result.effective)
            assertEquals(psvr, result.forcedByPsvr)
            assertEquals(JsonPrimitive(choice), global.values[DisplayMode.ID]) // forced mode never rewrites preference
        }
    }
    @Test fun gameOverrideAndReset() {
        assertEquals(DisplayMode.Mode.TWO_D, DisplayMode.resolve(profile("xr"), profile("2d"), false).effective)
        assertEquals(DisplayMode.Mode.XR, DisplayMode.resolve(profile("xr"), RuntimeProfile(), false).effective)
        assertEquals(DisplayMode.Mode.XR, DisplayMode.resolve(profile("2d"), profile("2d"), true).effective)
    }
    @Test fun catalogMatchesResolver() {
        val spec = RuntimeSettingCatalog.loadAndroidSettings().single { it.id == DisplayMode.ID }
        assertEquals(listOf("2d", "xr"), spec.choices)
        assertEquals(listOf("2D Screen", "XR Cinema"), spec.choiceLabels)
        assertTrue(spec.restartRequired)
    }
    @Test fun corruptPreferenceDoesNotSilentlyStartAnotherMode() {
        assertThrows(IllegalStateException::class.java) { DisplayMode.resolve(profile("invalid"), RuntimeProfile(), false) }
    }
}
