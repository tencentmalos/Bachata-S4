package com.shadps4.android

import androidx.test.ext.junit.runners.AndroidJUnit4
import androidx.test.platform.app.InstrumentationRegistry
import com.shadps4.android.runtime.input.NativePad
import com.shadps4.android.runtime.session.NativeFexSession
import java.io.File
import org.junit.Assert.*
import org.junit.Test
import org.junit.runner.RunWith

@RunWith(AndroidJUnit4::class)
class PadRuntimeInstrumentedTest {
    @Test fun guestReadsApplicationInputAndQueuesHaptics() {
        val instrumentation = InstrumentationRegistry.getInstrumentation()
        val context = instrumentation.targetContext
        assertTrue(NativePad.nativeInitializeHost(File(context.filesDir, "host").path))
        val root = File(context.filesDir, "validation/guest-pad-${System.nanoTime()}").apply { mkdirs() }
        val entry = File(root, "eboot.bin")
        instrumentation.context.assets.open("pad.elf").use { input -> entry.outputStream().use { input.copyTo(it) } }
        try {
            repeat(3) { round ->
                val token = NativePad.nativeBeginSession()
                assertTrue(token != 0L)
                try {
                    assertEquals(NativePad.Result.OK, NativePad.nativeSetConnected(token, 0, true))
                    assertEquals(NativePad.Result.OK, NativePad.nativeSubmit(token, 0, 0x4000, -1f, 0f, 0f, 0f, 0f, 0f, false, 0f, 0f))
                    val epoch = NativePad.nativeRegisterDevice(token, 0, 9000, floatArrayOf(), true)
                    assertTrue(epoch != 0L)
                    val generation = NativeFexSession.nativeStartExecutable("guest-pad", entry.path)
                    assertTrue(generation > 0)
                    try {
                        val outcome = NativeFexSession.nativeWaitTerminal(generation, 15000)
                        val detail = NativeFexSession.nativeTerminalDetail(generation).orEmpty()
                        android.util.Log.i("PadRuntimeAcceptance", "round=$round outcome=$outcome ${NativeFexSession.nativeIdentity()} $detail")
                        assertEquals(detail, NativeFexSession.Outcome.RETURNED, outcome)
                        assertTrue(detail, detail.startsWith("guest return=51966"))
                        // PadClose also queues actuator cancellation; JNI observes the same host hub.
                        assertTrue(NativePad.nativeDrainHaptics(token)?.isNotEmpty() == true)
                    } finally { NativeFexSession.nativeRequestStop(generation, 1000) }
                } finally { NativePad.nativeEndSession(token) }
            }
        } finally { if (NativeFexSession.nativeCurrentGeneration() == 0L) root.deleteRecursively() }
    }
}
