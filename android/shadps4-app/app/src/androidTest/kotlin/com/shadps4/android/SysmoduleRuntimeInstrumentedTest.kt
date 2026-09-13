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
class SysmoduleRuntimeInstrumentedTest {
    @Test fun initializedProvidersAndPositionedFileIo() {
        val instrumentation = InstrumentationRegistry.getInstrumentation()
        val context = instrumentation.targetContext
        assertTrue(NativePad.nativeInitializeHost(File(context.filesDir, "host").path))
        val root = File(context.filesDir, "validation/sysmodule-${System.nanoTime()}").apply { mkdirs() }
        try {
            for ((asset, path) in listOf("sysmodule.elf" to "eboot.bin", "sce_sys/param.sfo" to "sce_sys/param.sfo")) {
                val file = File(root, path).apply { parentFile!!.mkdirs() }
                instrumentation.context.assets.open(asset).use { input -> file.outputStream().use { input.copyTo(it) } }
            }
            File(root, "probe.bin").writeText("ABCDEFGH")
            repeat(3) { round ->
                val gen = NativeFexSession.nativeStartExecutable("sysmodule", File(root, "eboot.bin").path)
                assertTrue(gen > 0)
                try {
                    val outcome = NativeFexSession.nativeWaitTerminal(gen, 15000)
                    val detail = NativeFexSession.nativeTerminalDetail(gen).orEmpty()
                    android.util.Log.i("SysmoduleAcceptance", "round=$round outcome=$outcome ${NativeFexSession.nativeIdentity()} $detail")
                    assertEquals(detail, NativeFexSession.Outcome.RETURNED, outcome)
                    assertTrue(detail, detail.startsWith("guest return=51966"))
                } finally { NativeFexSession.nativeRequestStop(gen, 1000) }
            }
        } finally { if (NativeFexSession.nativeCurrentGeneration() == 0L) root.deleteRecursively() }
    }
}
