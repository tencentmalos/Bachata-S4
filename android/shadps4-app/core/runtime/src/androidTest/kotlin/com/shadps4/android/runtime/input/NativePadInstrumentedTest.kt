package com.shadps4.android.runtime.input

import androidx.test.ext.junit.runners.AndroidJUnit4
import com.shadps4.android.runtime.session.ManagedSession
import com.shadps4.android.runtime.session.NativeFexSession
import kotlin.test.assertEquals
import kotlin.test.assertNotNull
import kotlin.test.assertTrue
import org.junit.Test
import org.junit.runner.RunWith

/**
 * On-device instrumented test for the native pad path: real JVM -> JNI -> native
 * OrbisPadAdapter on the target arm64/bionic runtime. Verifies the seam the Kotlin
 * input stack was missing — a resolved [ControllerSnapshot] now reaches a real PS4
 * pad-state consumer — including the [NativePadBridge] + [ManagedSession] sink
 * wiring that closes the previously-unconsumed dead end.
 *
 * This runs libshadps4_fex_session.so on the device; it exercises input marshalling
 * and pad-state conversion, not a running PS4 game.
 */
@RunWith(AndroidJUnit4::class)
class NativePadInstrumentedTest {

    private fun ensureLibraryLoaded() {
        // NativePad symbols live in the same .so NativeFexSession loads.
        NativeFexSession.nativeIdentity()
    }

    @Test
    fun jniRoundTrip_buttonsAndSticks() {
        ensureLibraryLoaded()
        val token = NativePad.nativeBeginSession()
        assertTrue(token != 0L, "native begin session token")
        try {
            NativePad.nativeSetConnected(token, 0, true)

            val snap = ControllerSnapshot.normalized(
                buttons = Ps4Button.CROSS or Ps4Button.UP,
                leftX = 1.0f,
                leftY = -1.0f,
                rightTrigger = 1.0f,
            )
            val r = NativePad.submit(token, 0, snap)
            assertEquals(NativePad.Result.OK, r, "submit ok")

            val buttons = NativePad.nativeReadButtons(0)
            assertEquals(Ps4Button.CROSS or Ps4Button.UP, buttons, "button bits round-trip via JNI")

            val analog = NativePad.nativeReadAnalog(0)
            assertNotNull(analog, "analog read")
            // leftX +1 -> 255, leftY -1 -> 1 (centre 128 - 127), r2 1.0 -> 255.
            assertEquals(255, analog[0], "leftX -> 255")
            assertEquals(1, analog[1], "leftY -> 1")
            assertEquals(128, analog[2], "rightX neutral -> 128")
            assertEquals(128, analog[3], "rightY neutral -> 128")
            assertEquals(255, analog[5], "right trigger -> 255")
            assertTrue(NativePad.nativeConnected(0), "port connected")
        } finally {
            NativePad.nativeEndSession(token)
        }
        assertEquals(0L, NativePad.nativeCurrentToken(), "token cleared after end")
    }

    @Test
    fun sessionGuard_staleTokenRefused() {
        ensureLibraryLoaded()
        val t1 = NativePad.nativeBeginSession()
        NativePad.nativeSetConnected(t1, 0, true)
        // New session invalidates the old token natively.
        val t2 = NativePad.nativeBeginSession()
        assertTrue(t2 != t1, "new token differs")
        try {
            val stale = NativePad.submit(t1, 0, ControllerSnapshot.normalized(buttons = Ps4Button.CIRCLE))
            assertEquals(NativePad.Result.WRONG_SESSION, stale, "stale token refused")
            // State cleared on the new session.
            assertEquals(0L, NativePad.nativeReadButtons(0), "state cleared on re-begin")
        } finally {
            NativePad.nativeEndSession(t2)
        }
    }

    @Test
    fun bridge_connectsManagedSessionSinkToNative() {
        ensureLibraryLoaded()
        // The bridge attaches the ManagedSession slot sink and forwards to native.
        val token = NativePadBridge.begin()
        assertTrue(token != 0L, "bridge begin")
        try {
            // Producing a snapshot the way GamepadInputManager / the overlay does must
            // now reach native pad state through the bridge (previously a dead end).
            ManagedSession.submitController(
                0,
                ControllerSnapshot.normalized(buttons = Ps4Button.TRIANGLE, leftX = -1.0f),
            )
            assertEquals(Ps4Button.TRIANGLE, NativePad.nativeReadButtons(0), "overlay/pad snapshot reached native")
            val analog = NativePad.nativeReadAnalog(0)
            assertNotNull(analog)
            assertEquals(1, analog[0], "leftX -1 -> 1 through the bridge")
        } finally {
            NativePadBridge.end()
        }
        // After end, a late producer must not write into native state.
        ManagedSession.submitController(0, ControllerSnapshot.normalized(buttons = Ps4Button.SQUARE))
        assertEquals(0L, NativePad.nativeCurrentToken(), "session ended")
    }

    @Test
    fun vibration_enqueueDrainThroughJni() {
        ensureLibraryLoaded()
        val token = NativePad.nativeBeginSession()
        try {
            assertEquals(NativePad.Result.OK, NativePad.nativeSetVibration(token, 0, 200, 100), "enqueue")
            // Latest wins.
            NativePad.nativeSetVibration(token, 0, 50, 25)
            val r = NativePad.drainVibration(0)
            assertNotNull(r, "drained")
            assertEquals(50, r.smallMotor, "small motor")
            assertEquals(25, r.largeMotor, "large motor")
            assertTrue(!r.cancel, "not a cancel")
            assertTrue(NativePad.drainVibration(0) == null, "nothing left")

            // (0,0) is a cancel.
            NativePad.nativeSetVibration(token, 0, 0, 0)
            val c = NativePad.drainVibration(0)
            assertNotNull(c)
            assertTrue(c.cancel, "cancel drained")
        } finally {
            NativePad.nativeEndSession(token)
        }
    }
}
