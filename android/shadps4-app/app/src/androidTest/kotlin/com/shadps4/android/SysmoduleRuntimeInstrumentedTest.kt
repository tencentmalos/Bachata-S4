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
            for ((asset, path) in listOf("sysmodule.elf" to "eboot.bin", "network-wait.elf" to "network-wait.bin", "sce_sys/param.sfo" to "sce_sys/param.sfo")) {
                val file = File(root, path).apply { parentFile!!.mkdirs() }
                instrumentation.context.assets.open(asset).use { input -> file.outputStream().use { input.copyTo(it) } }
            }
            File(root, "probe.bin").writeText("ABCDEFGH")
            repeat(5) { round ->
                val cancel = round == 3
                val temp = File(context.filesDir, "host/temp")
                val previous = temp.listFiles()?.map { it.name }?.toSet().orEmpty()
                val gen = NativeFexSession.nativeStartExecutable("sysmodule", File(root,
                    if (cancel) "network-wait.bin" else "eboot.bin").path)
                assertTrue(gen > 0)
                try {
                    if (cancel) {
                        val deadline = android.os.SystemClock.uptimeMillis() + 15000
                        var entered = false
                        while (!entered && android.os.SystemClock.uptimeMillis() < deadline) {
                            entered = temp.listFiles().orEmpty().any { directory ->
                                directory.name !in previous && directory.name.startsWith("CUSA99991-") &&
                                    File(directory, "network-callback-entered").let {
                                        it.isFile && it.readBytes().contentEquals(byteArrayOf(1, 0, 0, 0))
                                    }
                            }
                            if (!entered) android.os.SystemClock.sleep(10)
                        }
                        assertTrue("guest callback entry handshake", entered)
                        val stop = NativeFexSession.nativeRequestStop(gen, 1000)
                        assertTrue("stop=$stop", stop == NativeFexSession.Stop.ACCEPTED ||
                            stop == NativeFexSession.Stop.CANCEL_PENDING)
                    }
                    val outcome = NativeFexSession.nativeWaitTerminal(gen, 15000)
                    val detail = NativeFexSession.nativeTerminalDetail(gen).orEmpty()
                    android.util.Log.i("SysmoduleAcceptance", "round=$round cancel=$cancel outcome=$outcome ${NativeFexSession.nativeIdentity()} $detail")
                    assertEquals(detail, if (cancel) NativeFexSession.Outcome.CANCELLED else NativeFexSession.Outcome.RETURNED, outcome)
                    if (!cancel) assertTrue(detail, detail.startsWith("guest return=51966"))
                } finally { NativeFexSession.nativeRequestStop(gen, 1000) }
            }
        } finally { if (NativeFexSession.nativeCurrentGeneration() == 0L) root.deleteRecursively() }
    }
}
