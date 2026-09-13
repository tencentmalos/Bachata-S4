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
class ThreadAttributeRuntimeInstrumentedTest {
    @Test fun guestClockWritesRaceVmPublication() {
        val instrumentation = InstrumentationRegistry.getInstrumentation()
        val context = instrumentation.targetContext
        assertTrue(NativePad.nativeInitializeHost(File(context.filesDir, "host").path))
        val root = File(context.filesDir, "validation/clock-vm-${System.nanoTime()}").apply { mkdirs() }
        val entry = File(root, "eboot.bin")
        instrumentation.context.assets.open("clock-vm.elf").use { input -> entry.outputStream().use { input.copyTo(it) } }
        try {
            repeat(3) { round ->
                val generation = NativeFexSession.nativeStartExecutable("clock-vm", entry.path)
                assertTrue(generation > 0)
                try {
                    val outcome = NativeFexSession.nativeWaitTerminal(generation, 20000)
                    val detail = NativeFexSession.nativeTerminalDetail(generation).orEmpty()
                    android.util.Log.i("ThreadAttributeAcceptance", "clock-vm round=$round outcome=$outcome ${NativeFexSession.nativeIdentity()} $detail")
                    assertEquals(detail, NativeFexSession.Outcome.RETURNED, outcome)
                    assertTrue(detail, detail.startsWith("guest return=51966"))
                } finally { NativeFexSession.nativeRequestStop(generation, 1000) }
            }
        } finally { if (NativeFexSession.nativeCurrentGeneration() == 0L) root.deleteRecursively() }
    }

    @Test fun guestThreadsConsumeAttributes() {
        val instrumentation = InstrumentationRegistry.getInstrumentation()
        val context = instrumentation.targetContext
        assertTrue(NativePad.nativeInitializeHost(File(context.filesDir,"host").path))
        val root=File(context.filesDir,"validation/thread-attributes-${System.nanoTime()}").apply { mkdirs() }
        try {
            for (name in listOf("thread-attributes", "thread-attributes-fault")) {
            val entry=File(root,"eboot.bin")
            instrumentation.context.assets.open("$name.elf").use { input -> entry.outputStream().use { input.copyTo(it) } }
            val gen=NativeFexSession.nativeStartExecutable("thread-attributes",entry.path)
            assertTrue(gen>0)
            try {
                val outcome=NativeFexSession.nativeWaitTerminal(gen,15000)
                val detail=NativeFexSession.nativeTerminalDetail(gen).orEmpty()
                android.util.Log.i("ThreadAttributeAcceptance","case=$name outcome=$outcome ${NativeFexSession.nativeIdentity()} $detail")
                if (name == "thread-attributes") {
                    assertEquals(detail,NativeFexSession.Outcome.RETURNED,outcome)
                    assertTrue(detail,detail.startsWith("guest return=51966"))
                } else {
                    assertEquals(detail,NativeFexSession.Outcome.FAULTED,outcome)
                    assertTrue(detail,detail.contains("import=ChildFault0#"))
                    assertTrue(detail,detail.contains(" thread=2 "))
                }
            } finally { NativeFexSession.nativeRequestStop(gen,1000) }
            }
        } finally { if(NativeFexSession.nativeCurrentGeneration()==0L) root.deleteRecursively() }
    }
}
