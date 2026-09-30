package com.shadps4.android.runtime.settings

import kotlinx.serialization.json.JsonPrimitive
import org.junit.Assert.*
import org.junit.Test

class XrMoveHandsTest {
    private fun profile(value: Boolean) = RuntimeProfile(values = mapOf(XrMoveHands.ID to JsonPrimitive(value)))

    @Test fun gameOverrideAndExplicitFalse() {
        assertFalse(XrMoveHands.resolve(RuntimeProfile(), RuntimeProfile()))
        assertTrue(XrMoveHands.resolve(profile(true), RuntimeProfile()))
        assertFalse(XrMoveHands.resolve(profile(true), profile(false)))
        assertTrue(XrMoveHands.resolve(profile(false), profile(true)))
        val specs = RuntimeSettingCatalog.loadAndroidSettings()
        val spec = specs.single { it.id == XrMoveHands.ID }
        assertEquals("System", spec.category) // Visible on Android's System settings page.
        assertTrue(spec.restartRequired)
        assertEquals(SettingScope.GLOBAL_AND_GAME, spec.scope)
        val imported = ShadPs4JsonCodec.applyRawJson(RuntimeProfile(),
            """{"Input":{"xr_swap_move_hands":true}}""", specs)
        assertTrue(XrMoveHands.resolve(RuntimeProfile(), imported))
    }

    @Test fun invalidOrderIsRejected() {
        val broken = RuntimeProfile(values = mapOf(XrMoveHands.ID to JsonPrimitive("invalid")))
        assertThrows(IllegalArgumentException::class.java) { XrMoveHands.resolve(broken, RuntimeProfile()) }
    }
}
