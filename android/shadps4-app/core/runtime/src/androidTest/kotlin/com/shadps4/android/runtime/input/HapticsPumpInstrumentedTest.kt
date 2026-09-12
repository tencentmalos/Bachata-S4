package com.shadps4.android.runtime.input

import androidx.test.ext.junit.runners.AndroidJUnit4
import com.shadps4.android.runtime.session.NativeFexSession
import java.util.concurrent.CopyOnWriteArrayList
import kotlin.test.assertEquals
import kotlin.test.assertTrue
import org.junit.Test
import org.junit.runner.RunWith

/**
 * On-device test for the haptics round-trip: native pad vibration slot ->
 * [HapticsPump] drain -> [HapticsSink]. Uses a recording fake sink (no device
 * Vibrator dependency) and the real JVM->JNI native drain on the target runtime.
 */
@RunWith(AndroidJUnit4::class)
class HapticsPumpInstrumentedTest {

    private class RecordingSink : HapticsSink {
        data class Event(val kind: String, val port: Int, val small: Int, val large: Int)
        val events = CopyOnWriteArrayList<Event>()
        override fun rumble(port: Int, smallMotor: Int, largeMotor: Int) {
            events.add(Event("rumble", port, smallMotor, largeMotor))
        }
        override fun cancel(port: Int) { events.add(Event("cancel", port, 0, 0)) }
        override fun cancelAll() { events.add(Event("cancelAll", -1, 0, 0)) }
    }

    private fun ensureLibraryLoaded() = NativeFexSession.nativeIdentity()

    @Test
    fun pumpDrainsNativeVibrationToSink() {
        ensureLibraryLoaded()
        val sink = RecordingSink()
        val pump = HapticsPump(sink, pollIntervalMs = 5L)
        val token = NativePad.nativeBeginSession()
        assertTrue(token != 0L)
        try {
            // Enqueue a rumble natively; one manual pump pass must deliver it.
            assertEquals(NativePad.Result.OK, NativePad.nativeSetVibration(token, 0, 180, 90), "enqueue")
            pump.pumpOnce()
            val rumble = sink.events.firstOrNull { it.kind == "rumble" }
            assertTrue(rumble != null, "rumble delivered to sink")
            assertEquals(0, rumble.port, "port 0")
            assertEquals(180, rumble.small, "small motor")
            assertEquals(90, rumble.large, "large motor")

            // A (0,0) command drains as a cancel.
            sink.events.clear()
            NativePad.nativeSetVibration(token, 0, 0, 0)
            pump.pumpOnce()
            assertTrue(sink.events.any { it.kind == "cancel" && it.port == 0 }, "cancel delivered")

            // Nothing pending -> no events.
            sink.events.clear()
            pump.pumpOnce()
            assertTrue(sink.events.isEmpty(), "no events when queue empty")
        } finally {
            NativePad.nativeEndSession(token)
        }
    }

    @Test
    fun startStopThreadedPumpDeliversAndCancelsAll() {
        ensureLibraryLoaded()
        val sink = RecordingSink()
        val pump = HapticsPump(sink, pollIntervalMs = 5L)
        val token = NativePad.nativeBeginSession()
        try {
            pump.start()
            NativePad.nativeSetVibration(token, 1, 120, 60)
            // Give the worker a few ticks to drain.
            val deadline = System.currentTimeMillis() + 1000
            while (System.currentTimeMillis() < deadline &&
                sink.events.none { it.kind == "rumble" && it.port == 1 }) {
                Thread.sleep(10)
            }
            assertTrue(sink.events.any { it.kind == "rumble" && it.port == 1 }, "threaded drain delivered")
        } finally {
            pump.stop()
            NativePad.nativeEndSession(token)
        }
        assertTrue(sink.events.any { it.kind == "cancelAll" }, "stop cancels all effects")
    }
}
