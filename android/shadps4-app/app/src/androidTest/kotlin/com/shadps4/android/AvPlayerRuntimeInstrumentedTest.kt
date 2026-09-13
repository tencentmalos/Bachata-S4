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
class AvPlayerRuntimeInstrumentedTest {
    @Test fun localVideoUsesOwnedGuestCallbacksAcrossRestarts() {
        val instrumentation = InstrumentationRegistry.getInstrumentation()
        val context = instrumentation.targetContext
        assertTrue(NativePad.nativeInitializeHost(File(context.filesDir, "host").path))
        val root = File(context.filesDir, "validation/avplayer-${System.nanoTime()}").apply { mkdirs() }
        val media = File(context.filesDir, "validation/avplayer-probe.mp4")
        assertTrue("Stage the generated 64x48 H264/AAC probe before this targeted test", media.isFile)
        media.copyTo(File(root, "probe.mp4"), overwrite = true)
        try {
            listOf("avplayer", "avplayer", "avplayer").forEachIndexed { round, name ->
                val entry = File(root, "eboot.bin")
                instrumentation.context.assets.open("$name.elf").use { input -> entry.outputStream().use { input.copyTo(it) } }
                val gen = NativeFexSession.nativeStartExecutable("avplayer", entry.path)
                assertTrue(gen > 0)
                try {
                    val outcome = NativeFexSession.nativeWaitTerminal(gen, 30000)
                    val detail = NativeFexSession.nativeTerminalDetail(gen).orEmpty()
                    android.util.Log.i("AvPlayerAcceptance", "round=$round case=$name outcome=$outcome ${NativeFexSession.nativeIdentity()} $detail")
                    assertEquals(detail, NativeFexSession.Outcome.RETURNED, outcome)
                    assertTrue(detail, detail.startsWith("guest return=51966"))
                } finally { NativeFexSession.nativeRequestStop(gen, 1000) }
            }
        } finally { if (NativeFexSession.nativeCurrentGeneration() == 0L) root.deleteRecursively() }
    }
}
