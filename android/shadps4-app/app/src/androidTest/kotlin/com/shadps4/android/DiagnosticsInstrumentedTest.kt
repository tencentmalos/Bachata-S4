package com.shadps4.android

import android.content.Intent
import android.os.SystemClock
import androidx.test.ext.junit.runners.AndroidJUnit4
import androidx.test.platform.app.InstrumentationRegistry
import com.shadps4.android.runtime.input.NativePad
import com.shadps4.android.runtime.session.AndroidTurnip
import com.shadps4.android.runtime.session.ManagedSession
import com.shadps4.android.runtime.session.NativeFexSession
import com.shadps4.android.service.FexSessionService
import java.io.File
import org.junit.Assert.*
import org.junit.Test
import org.junit.runner.RunWith

/**
 * On-device verification of the graphics/performance debugging toolkit control entry
 * (spec §3.1): the DiagnosticsHub, its command registry, and the nativeDebugCommand
 * JNI path are exercised inside the real app process. The backend here is CPU smoke
 * (no GuestGraphics), so this asserts session identity/run_uuid/stage/advance-signal
 * reporting and the honest not-implemented boundary -- NOT a rendering claim.
 */
@RunWith(AndroidJUnit4::class)
class DiagnosticsInstrumentedTest {
    @Test fun destroyedSurfaceFaultsAndNextGenerationRenders() {
        val instrumentation = InstrumentationRegistry.getInstrumentation()
        val context = instrumentation.targetContext
        assertTrue(NativePad.nativeInitializeHost(File(context.filesDir, "host").absolutePath))
        val paths = AndroidTurnip.prepare(context)
        val root = File(context.filesDir, "validation/surface-loss-${System.nanoTime()}").apply { mkdirs() }
        val entry = File(root, "eboot.bin")
        instrumentation.context.assets.open("gpu-flip.elf").use { input -> entry.outputStream().use { input.copyTo(it) } }
        try {
            repeat(2) { round ->
                RuntimeTestSurface(instrumentation).use { window ->
                    val generation = NativeFexSession.nativeStartRenderedExecutable(
                        "surface-loss-recovery", entry.path, window.surface, paths.hooks, paths.driver)
                    assertTrue(generation > 0)
                    try {
                        assertEquals(NativeFexSession.WaitPhase.REACHED_TARGET,
                            NativeFexSession.nativeWaitPhase(generation, NativeFexSession.PhaseOrdinal.READY, 10000))
                        // Exercise native WSI's dead-window failure, independent of the
                        // Service's normal Surface observer which proactively requests Stop.
                        if (round == 0) { window.close(); assertFalse(window.surface.isValid) }
                        assertTrue(NativeFexSession.nativePlatformReady(generation))
                        val outcome = NativeFexSession.nativeWaitTerminal(generation, 15000)
                        val detail = NativeFexSession.nativeTerminalDetail(generation).orEmpty()
                        assertEquals(detail, if (round == 0) NativeFexSession.Outcome.FAULTED else NativeFexSession.Outcome.RETURNED, outcome)
                        if (round == 0) assertTrue(detail, detail.contains("import=Up36PTk687E#libSceVideoOut#") &&
                            detail.contains("graphics=not-created"))
                        else assertTrue(detail, (Regex("guest_presents=(\\d+)").find(detail)?.groupValues?.get(1)?.toInt() ?: 0) >= 4)
                        android.util.Log.i("DiagnosticsAcceptance", "surface round=$round generation=$generation outcome=$outcome $detail")
                    } finally { NativeFexSession.nativeRequestStop(generation, 1000) }
                }
            }
        } finally { if (NativeFexSession.nativeCurrentGeneration() == 0L) root.deleteRecursively() }
    }

    private fun await(message: String, predicate: () -> Boolean) {
        val deadline = SystemClock.uptimeMillis() + 5000
        while (!predicate() && SystemClock.uptimeMillis() < deadline) SystemClock.sleep(5)
        assertTrue(message, predicate())
    }

    @Test fun debugStatusReflectsLiveSessionAndHonestBoundaries() {
        val context = InstrumentationRegistry.getInstrumentation().targetContext
        try {
            // Before any session: the registry builds lazily and reports no session.
            val idle = NativeFexSession.nativeDebugCommand("debug_status")
            assertTrue("idle status: $idle", idle.contains("session: none"))

            // Start a real CPU-smoke session through the ordinary Service. No Activity
            // is needed -- nativeDebugCommand reaches the hub directly.
            context.startService(Intent(context, FexSessionService::class.java)
                .setAction(ManagedSession.ACTION_START)
                .putExtra(ManagedSession.EXTRA_GAME_ID, "diagnostics-cpu-smoke"))
            await("session entered") { NativeFexSession.nativeCurrentGeneration() > 0L }
            val generation = NativeFexSession.nativeCurrentGeneration()
            assertEquals("real CPU running", NativeFexSession.WaitPhase.REACHED_TARGET,
                NativeFexSession.nativeWaitPhase(generation, NativeFexSession.PhaseOrdinal.RUNNING, 3000))

            // The hub now reports this exact generation, a real run_uuid, and every
            // advance signal, with gpu_retire UNAVAILABLE (never a misleading 0).
            val status = NativeFexSession.nativeDebugCommand("debug_status")
            android.util.Log.i("DiagnosticsAcceptance", "debug_status:\n$status")
            assertTrue("running phase: $status", status.contains("phase: 3"))
            assertTrue("overlay unavailable: $status", status.contains("overlay_redraw: unavailable"))
            assertTrue("active: $status", status.contains("session: active"))
            assertTrue("generation: $status", status.contains("generation: $generation"))
            assertTrue("run_uuid present: $status",
                Regex("run_uuid: [0-9a-f]{32}").containsMatchIn(status))
            assertFalse("run_uuid not unknown: $status", status.contains("run_uuid: unknown"))
            assertTrue("gpu_retire unavailable: $status", status.contains("gpu_retire: unavailable"))
            for (signal in listOf("guest_flip", "pm4_consumed", "host_draw", "queue_submit",
                                  "host_present", "overlay_redraw")) {
                assertTrue("$signal listed: $status", status.contains("$signal:"))
            }

            // renderdoc_status works; the capture coordinator is wired.
            val rdoc = NativeFexSession.nativeDebugCommand("renderdoc_status")
            assertTrue("rdoc: $rdoc", rdoc.contains("renderdoc_api_loaded:"))
            assertTrue("rdoc capture backend: $rdoc", rdoc.contains("capture_backend: coordinator"))

            // renderdoc_capture is a real request/receipt command now (spec §3.2).
            // RenderDoc is not injected on this device, so it must honestly report a
            // failed receipt with a reason -- never a faked ready/capture file.
            val cap = NativeFexSession.nativeDebugCommand("renderdoc_capture 1")
            android.util.Log.i("DiagnosticsAcceptance", "renderdoc_capture:\n$cap")
            assertTrue("capture has a receipt state: $cap", cap.contains("state:"))
            assertTrue("capture rejected without renderer: $cap", cap.contains("status: no_matching_renderer"))
            assertFalse("capture not faked ready: $cap", cap.contains("state: ready"))
            assertTrue(NativeFexSession.nativeDebugCommand("renderdoc_capture -1").contains("invalid_arguments"))
            assertTrue(NativeFexSession.nativeDebugCommand("renderdoc_capture_cancel").contains("invalid_arguments"))
            val capStatus = NativeFexSession.nativeDebugCommand("renderdoc_capture_status")
            assertTrue("capture status has a receipt: $capStatus", capStatus.contains("state:"))

            // Still-pending commands never fake success.
            for (cmd in listOf("profiler_ring", "gpu_command_trace")) {
                val r = NativeFexSession.nativeDebugCommand(cmd)
                assertTrue("$cmd not-implemented: $r", r.contains("status: not-implemented"))
                assertFalse("$cmd not ready: $r", r.contains("ready"))
            }

            // Help lists the real command set.
            val help = NativeFexSession.nativeDebugCommand("")
            assertTrue("help: $help", help.contains("debug_status") && help.contains("profiler_ring"))

            // Stop retains terminal evidence while reporting no active session.
            context.startService(Intent(context, FexSessionService::class.java)
                .setAction(ManagedSession.ACTION_STOP))
            val terminal = NativeFexSession.nativeWaitTerminal(generation, 3000)
            assertTrue("terminal=$terminal",
                terminal == NativeFexSession.Outcome.CANCELLED ||
                terminal == NativeFexSession.Outcome.RETURNED)
            await("session revoked from hub") {
                NativeFexSession.nativeDebugCommand("debug_status").contains("session: none")
            }
            val retained = NativeFexSession.nativeDebugCommand("debug_status")
            assertTrue("retained generation: $retained", retained.contains("generation: $generation"))
            assertTrue("terminal phase: $retained", retained.contains("phase: 5"))
            android.util.Log.i("DiagnosticsAcceptance", "retained:\n$retained")
            android.util.Log.i("DiagnosticsAcceptance",
                "generation=$generation terminal=$terminal PASS")
        } finally {
            context.startService(Intent(context, FexSessionService::class.java)
                .setAction(ManagedSession.ACTION_STOP))
        }
    }

    /**
     * Verifies the graphics advance producers (guest_flip / queue_submit / host_present)
     * fire in a REAL rendering session (spec §3.1). Uses the synthetic gpu-flip fixture
     * that drives actual VideoOut/GPU submission and >=4 real guest presents through
     * Turnip -- no copyrighted assets. Not a game/playability claim.
     */
    @Test fun graphicsProducersAdvanceInRenderingSession() {
        val instrumentation = InstrumentationRegistry.getInstrumentation()
        val context = instrumentation.targetContext
        assertTrue(NativePad.nativeInitializeHost(File(context.filesDir, "host").absolutePath))
        val paths = AndroidTurnip.prepare(context)
        val root = File(context.filesDir, "validation/diag-gpuflip-${System.nanoTime()}").apply { mkdirs() }
        val entry = File(root, "eboot.bin")
        instrumentation.context.assets.open("gpu-flip.elf").use { input ->
            entry.outputStream().use { input.copyTo(it) }
        }
        // Sample the hub while the rendering session runs; capture the peak signals seen.
        // A final post-terminal read backstops the race where a fast synthetic returns
        // between polls: the final publisher retains its identity and counts after teardown.
        var sawActive = false
        var peakFlip = 0L
        var peakSubmit = 0L
        var peakPresent = 0L
        var peakPm4 = 0L
        var peakDraw = 0L
        fun signal(status: String, name: String): Long =
            Regex("$name: (\\d+)").find(status)?.groupValues?.get(1)?.toLong() ?: 0L
        fun sample(status: String) {
            if (status.contains("session: active")) sawActive = true
            peakFlip = maxOf(peakFlip, signal(status, "guest_flip"))
            peakSubmit = maxOf(peakSubmit, signal(status, "queue_submit"))
            peakPresent = maxOf(peakPresent, signal(status, "host_present"))
            peakPm4 = maxOf(peakPm4, signal(status, "pm4_consumed"))
            peakDraw = maxOf(peakDraw, signal(status, "host_draw"))
        }
        try {
            RuntimeTestSurface(instrumentation).use { window ->
                val generation = NativeFexSession.nativeStartRenderedExecutable(
                    "diagnostics-gpu-flip", entry.path, window.surface, paths.hooks, paths.driver)
                assertTrue("rendering generation", generation > 0)
                assertTrue(NativeFexSession.nativePlatformReady(generation))
                // Poll debug_status until the session ends, tracking peak signals.
                val deadline = SystemClock.uptimeMillis() + 15000
                while (SystemClock.uptimeMillis() < deadline) {
                    sample(NativeFexSession.nativeDebugCommand("debug_status"))
                    if (NativeFexSession.nativeWaitTerminal(generation, 50) != NativeFexSession.Outcome.TIMEOUT) break
                }
                // Final read after the terminal, from the retained publisher:
                // the retained counts reflect the whole run, immune to poll timing.
                sample(NativeFexSession.nativeDebugCommand("debug_status"))
                // host_present is published from the async GPU present, which can land
                // shortly after the guest returns. Give it a bounded settle window
                // (the publisher retains the terminal evidence after teardown).
                val presentDeadline = SystemClock.uptimeMillis() + 3000
                while (peakPresent < 1 && SystemClock.uptimeMillis() < presentDeadline) {
                    sample(NativeFexSession.nativeDebugCommand("debug_status"))
                    SystemClock.sleep(25)
                }
                val detail = NativeFexSession.nativeTerminalDetail(generation).orEmpty()
                val detailPresents = Regex("guest_presents=(\\d+)").find(detail)?.groupValues?.get(1)?.toLong() ?: 0L
                android.util.Log.i("DiagnosticsAcceptance",
                    "gpu-flip gen=$generation flip=$peakFlip submit=$peakSubmit present=$peakPresent pm4=$peakPm4 draw=$peakDraw detailPresents=$detailPresents detail=$detail")
                assertTrue("saw active rendering session", sawActive)
                assertTrue("graphics=ready: $detail", detail.contains("graphics=ready"))
                // The synthetic gpu-flip presents real frames; the producers must
                // reflect actual GPU submission and presentation, not stay at 0.
                assertTrue("guest_flip advanced (was $peakFlip)", peakFlip >= 1)
                assertTrue("queue_submit advanced (was $peakSubmit)", peakSubmit >= 1)
                assertEquals("live presents match terminal summary", detailPresents, peakPresent)
                assertTrue("pm4_consumed advanced (was $peakPm4)", peakPm4 >= 1)
                // host_present: the hub's live count OR the terminal detail's
                // authoritative guest_presents must show a real present.
                assertTrue("host_present advanced (hub=$peakPresent detail=$detailPresents)",
                    peakPresent >= 1)
                // NOTE: host_draw stays 0 here by design -- the gpu-flip synthetic
                // fixture's DCB is a PrepareFlip only and issues no draw packets, so
                // Rasterizer::Draw is never called. The host_draw producer is wired at
                // the real IncDrawCall/IncDispatch sites (verified by NDK compile);
                // it is simply not exercised by a flip-only fixture. Logged, not asserted.
                assertEquals("host_draw is 0 for a flip-only fixture (draw=$peakDraw)", 0L, peakDraw)
            }
        } finally {
            NativeFexSession.nativeRequestStop(NativeFexSession.nativeCurrentGeneration(), 1000)
            if (NativeFexSession.nativeCurrentGeneration() == 0L) root.deleteRecursively()
        }
    }
}
