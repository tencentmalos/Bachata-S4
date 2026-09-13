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
class AjmRuntimeInstrumentedTest {
    @Test fun asyncBatchesMarshalStackArgumentsAndRecover() {
        val instrumentation = InstrumentationRegistry.getInstrumentation()
        val context = instrumentation.targetContext
        assertTrue(NativePad.nativeInitializeHost(File(context.filesDir, "host").path))
        val root = File(context.filesDir, "validation/ajm-${System.nanoTime()}").apply { mkdirs() }
        try {
            listOf("ajm", "ajm", "ajm").forEachIndexed { round, name ->
                val entry = File(root, "eboot.bin")
                instrumentation.context.assets.open("$name.elf").use { input -> entry.outputStream().use { input.copyTo(it) } }
                val gen = NativeFexSession.nativeStartExecutable("ajm", entry.path)
                assertTrue(gen > 0)
                try {
                    val outcome = NativeFexSession.nativeWaitTerminal(gen, 15000)
                    val detail = NativeFexSession.nativeTerminalDetail(gen).orEmpty()
                    android.util.Log.i("AjmAcceptance", "round=$round case=$name outcome=$outcome ${NativeFexSession.nativeIdentity()} $detail")
                    assertEquals(detail, NativeFexSession.Outcome.RETURNED, outcome)
                    assertTrue(detail, detail.startsWith("guest return=51966"))
                } finally { NativeFexSession.nativeRequestStop(gen, 1000) }
            }
        } finally { if (NativeFexSession.nativeCurrentGeneration() == 0L) root.deleteRecursively() }
    }
}
