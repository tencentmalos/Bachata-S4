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
class NpRuntimeInstrumentedTest {
    @Test fun offlineIdentitiesUseCheckedGuestOutputsAcrossRestarts() {
        val instrumentation = InstrumentationRegistry.getInstrumentation()
        val context = instrumentation.targetContext
        assertTrue(NativePad.nativeInitializeHost(File(context.filesDir, "host").path))
        val root = File(context.filesDir, "validation/np-${System.nanoTime()}").apply { mkdirs() }
        try {
            listOf("np", "np", "np").forEachIndexed { round, name ->
                val entry = File(root, "eboot.bin")
                instrumentation.context.assets.open("$name.elf").use { input -> entry.outputStream().use { input.copyTo(it) } }
                val gen = NativeFexSession.nativeStartExecutable("np", entry.path)
                assertTrue(gen > 0)
                try {
                    val outcome = NativeFexSession.nativeWaitTerminal(gen, 30000)
                    val detail = NativeFexSession.nativeTerminalDetail(gen).orEmpty()
                    android.util.Log.i("NpAcceptance", "round=$round case=$name outcome=$outcome ${NativeFexSession.nativeIdentity()} $detail")
                    assertEquals(detail, NativeFexSession.Outcome.RETURNED, outcome)
                    assertTrue(detail, detail.startsWith("guest return=51966"))
                } finally { NativeFexSession.nativeRequestStop(gen, 1000) }
            }
        } finally { if (NativeFexSession.nativeCurrentGeneration() == 0L) root.deleteRecursively() }
    }
}
