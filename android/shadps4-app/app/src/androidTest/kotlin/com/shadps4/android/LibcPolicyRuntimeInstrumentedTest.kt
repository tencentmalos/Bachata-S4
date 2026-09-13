package com.shadps4.android

import androidx.test.ext.junit.runners.AndroidJUnit4
import androidx.test.platform.app.InstrumentationRegistry
import com.shadps4.android.runtime.input.NativePad
import com.shadps4.android.runtime.session.NativeFexSession
import java.io.File
import org.junit.Assert.*
import org.junit.Assume.assumeFalse
import org.junit.Test
import org.junit.runner.RunWith

@RunWith(AndroidJUnit4::class)
class LibcPolicyRuntimeInstrumentedTest {
    @Test fun completeSystemProviderPrecedesCompatibilityAndNeverFallsBackOnFailure() {
        val instrumentation = InstrumentationRegistry.getInstrumentation()
        val context = instrumentation.targetContext
        val host = File(context.filesDir, "host")
        assertTrue(NativePad.nativeInitializeHost(host.path))
        val system = File(host, "sys_modules/libSceLibcInternal.sprx")
        assumeFalse("NOT_RUN: preserve user's installed system libc", system.exists())
        val root = File(context.filesDir, "validation/libc-policy-${System.nanoTime()}").apply { mkdirs() }
        fun copy(asset: String, out: File) {
            out.parentFile!!.mkdirs()
            instrumentation.context.assets.open("$asset.elf").use { input ->
                out.outputStream().use { input.copyTo(it) }
            }
        }
        try {
            copy("libc-policy-main", File(root, "eboot.bin"))
            copy("libc-policy-fallback", File(root, "modules/libc.prx"))
            for (mode in listOf("fallback", "system", "bad-init", "fallback-recovery", "bootstrap-legacy")) {
                if (mode == "system") copy("libc-policy-system", system)
                else if (mode == "bad-init") copy("libc-policy-bad-init", system)
                else system.delete()
                if (mode == "bootstrap-legacy") {
                    copy("bootstrap", File(root, "eboot.bin"))
                    copy("libc", File(root, "modules/libc.prx"))
                }
                val generation = NativeFexSession.nativeStartExecutable("libc-policy", File(root, "eboot.bin").path)
                assertTrue(generation > 0)
                try {
                    val outcome = NativeFexSession.nativeWaitTerminal(generation, 10000)
                    val detail = NativeFexSession.nativeTerminalDetail(generation).orEmpty()
                    android.util.Log.i("LibcPolicyAcceptance", "case=$mode outcome=$outcome ${NativeFexSession.nativeIdentity()} $detail")
                    if (mode == "bad-init") {
                        assertEquals(detail, NativeFexSession.Outcome.BACKEND_FAILED, outcome)
                        assertTrue(detail, detail.contains("_malloc_init failed: libSceLibcInternal.sprx"))
                    } else {
                        assertEquals(detail, NativeFexSession.Outcome.RETURNED, outcome)
                        assertTrue(detail, detail.startsWith("guest return=${if (mode == "system") 516 else if (mode == "bootstrap-legacy") 51966 else 260}"))
                    }
                } finally { NativeFexSession.nativeRequestStop(generation, 1000) }
            }
        } finally {
            if (NativeFexSession.nativeCurrentGeneration() == 0L) {
                system.delete()
                root.deleteRecursively()
            }
        }
    }
}
