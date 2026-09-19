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
class GuestPatchInstrumentedTest {
    @Test fun realGuestPatchThroughProductionLoaderAcrossSessions() {
        val instrumentation = InstrumentationRegistry.getInstrumentation()
        val context = instrumentation.targetContext
        assertTrue(NativePad.nativeInitializeHost(File(context.filesDir, "host").path))
        // The deploy script supplies authored files and an explicit debug property.
        val root = File(context.filesDir, "validation/guest-patch")
        val entry = File(root, "eboot.bin")
        assertTrue("deploy make_apk_fixture.py output first", entry.isFile)
        repeat(3) { round ->
            val gen = NativeFexSession.nativeStartExecutable("guest-patch", entry.path)
            assertTrue(gen > 0)
            try {
                val outcome = NativeFexSession.nativeWaitTerminal(gen, 30000)
                val detail = NativeFexSession.nativeTerminalDetail(gen).orEmpty()
                android.util.Log.i("GuestPatchAcceptance", "round=$round generation=$gen outcome=$outcome ${NativeFexSession.nativeIdentity()} $detail")
                assertEquals(detail, NativeFexSession.Outcome.RETURNED, outcome)
                assertTrue(detail, detail.startsWith("guest return=51966"))
            } finally { NativeFexSession.nativeRequestStop(gen, 1000) }
        }
    }
}
