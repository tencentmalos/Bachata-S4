package com.shadps4.android

import android.content.Intent
import android.os.SystemClock
import androidx.test.ext.junit.runners.AndroidJUnit4
import androidx.test.platform.app.InstrumentationRegistry
import com.shadps4.android.data.InstallManifest
import com.shadps4.android.data.InstallManifestIo
import com.shadps4.android.runtime.input.NativePadBridge
import com.shadps4.android.runtime.session.ManagedSession
import com.shadps4.android.runtime.session.ManagedSessionState
import com.shadps4.android.runtime.session.NativeFexSession
import com.shadps4.android.service.FexSessionService
import java.io.File
import org.junit.Assert.*
import org.junit.Test
import org.junit.runner.RunWith

/** Exercises production SessionCore/Module/Linker/VM/threads through the ordinary
 * app Service. Content is source-generated synthetic ELF, not game acceptance. */
@RunWith(AndroidJUnit4::class)
class ProductionRuntimeInstrumentedTest {
    private fun await(message: String, predicate: () -> Boolean) {
        val deadline = SystemClock.uptimeMillis() + 10000
        while (!predicate() && SystemClock.uptimeMillis() < deadline) SystemClock.sleep(5)
        assertTrue(message, predicate())
    }

    @Test fun productionContentRunsCancelsAndRecoversAcrossServiceGenerations() {
        val instrumentation = InstrumentationRegistry.getInstrumentation()
        val context = instrumentation.targetContext
        val activity = instrumentation.startActivitySync(Intent(context, MainActivity::class.java)
            .addFlags(Intent.FLAG_ACTIVITY_NEW_TASK))
        val identity = NativeFexSession.nativeIdentity()
        val runDeadline = InstrumentationRegistry.getArguments().getString("runtimeDeadlineMs")?.toLong() ?: 10000L
        val root = File(context.filesDir, "games/runtime-synthetic-${SystemClock.uptimeMillis()}")
        root.mkdirs()
        File(root, "modules").mkdirs()
        fun observedGeneration(): Long = when (val state = ManagedSession.state.value) {
            is ManagedSessionState.Preparing -> state.generation
            is ManagedSessionState.Ready -> state.generation
            is ManagedSessionState.Running -> state.generation
            is ManagedSessionState.Stopping -> state.generation
            is ManagedSessionState.Stopped -> state.generation
            is ManagedSessionState.Failed -> state.generation
            ManagedSessionState.Idle -> 0L
        }
        var previous = observedGeneration()
        try {
            val scenarios = listOf("self", "wait", "fixture", "wait", "fixture", "wait", "bad", "fixture", "unknown", "fixture", "init-wait", "fixture")
            scenarios.forEachIndexed { round, scenario ->
                val cancel = scenario == "wait" || scenario == "init-wait"
                val fault = scenario == "bad" || scenario == "unknown"
                val entryAsset = if (scenario == "init-wait") "fixture.elf" else "$scenario.elf"
                val dependencyAsset = if (scenario == "init-wait") "dependency-wait.elf" else "fixture_dependency.sprx"
                instrumentation.context.assets.open(dependencyAsset).use { input ->
                    File(root, "modules/fixture_dependency.sprx").outputStream().use { input.copyTo(it) }
                }
                instrumentation.context.assets.open(entryAsset).use { input ->
                    File(root, "eboot.bin").outputStream().use { input.copyTo(it) }
                }
                InstallManifestIo.write(root, InstallManifest(status = InstallManifestIo.STATUS_INSTALLED,
                    gameId = "runtime-synthetic", contentId = null, mode = "synthetic-elf-test",
                    sourceUri = "generated:test", installedAtMs = System.currentTimeMillis(),
                    requiredFiles = listOf("eboot.bin", "modules/fixture_dependency.sprx"),
                    bytesTotal = File(root, "eboot.bin").length() + File(root, "modules/fixture_dependency.sprx").length()))
                context.startService(Intent(context, FexSessionService::class.java)
                    .setAction(ManagedSession.ACTION_START)
                    .putExtra(ManagedSession.EXTRA_GAME_ID, "runtime-synthetic")
                    .putExtra(ManagedSession.EXTRA_GAME_PATH, root.relativeTo(context.filesDir).path))
                await("production generation published") { observedGeneration() > previous }
                val generation = observedGeneration()
                previous = generation
                assertTrue("production generation", generation > 0)
                if (cancel) {
                    assertEquals(NativeFexSession.WaitPhase.REACHED_TARGET,
                        NativeFexSession.nativeWaitPhase(generation, NativeFexSession.PhaseOrdinal.RUNNING, 10000))
                    context.startService(Intent(context, FexSessionService::class.java).setAction(ManagedSession.ACTION_STOP))
                }
                val outcome = NativeFexSession.nativeWaitTerminal(generation, runDeadline)
                val detail = NativeFexSession.nativeTerminalDetail(generation).orEmpty()
                assertEquals(detail, if (cancel) NativeFexSession.Outcome.CANCELLED else if (fault) NativeFexSession.Outcome.FAULTED else NativeFexSession.Outcome.RETURNED, outcome)
                if (!cancel && !fault) assertTrue(detail, detail.startsWith("guest return=51966"))
                if (fault) assertTrue(detail, detail.contains("operation=") && detail.contains("invocation="))
                if (fault) assertTrue(detail, detail.contains("import=" + if (scenario == "unknown") "UnknownRunt#" else "OxhIB8LB-PQ#"))
                assertTrue("actual production backend: $detail", detail.contains("production Linker/VM"))
                await("input retired") { NativePadBridge.currentToken() == 0L }
                assertEquals("same app PID/UID", identity, NativeFexSession.nativeIdentity())
                android.util.Log.i("ProductionRuntimeAcceptance", "round=${round + 1} gen=$generation outcome=$outcome $identity $detail")
            }
        } finally {
            context.startService(Intent(context, FexSessionService::class.java).setAction(ManagedSession.ACTION_STOP))
            instrumentation.runOnMainSync { activity.finish() }
            if (NativeFexSession.nativeCurrentGeneration() == 0L) root.deleteRecursively()
        }
    }
}
