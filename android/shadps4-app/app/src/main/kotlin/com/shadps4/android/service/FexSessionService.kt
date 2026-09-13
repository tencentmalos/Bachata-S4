package com.shadps4.android.service

import android.app.Service
import android.content.Intent
import android.os.IBinder
import android.util.Log
import com.shadps4.android.data.GameInstallVerifier
import java.io.File
import com.shadps4.android.model.RuntimeErrorCode
import com.shadps4.android.runtime.diagnostics.ProcessTerminationInfo
import com.shadps4.android.runtime.diagnostics.TerminationKind
import com.shadps4.android.runtime.input.GamepadInputManager
import com.shadps4.android.runtime.input.NativePadBridge
import com.shadps4.android.runtime.session.ManagedSession
import com.shadps4.android.runtime.session.ManagedSessionState
import com.shadps4.android.runtime.session.NativeFexSession
import kotlin.concurrent.thread

/**
 * In-process FEX session orchestrator. Replaces the reference EmulationService (which launched an
 * external glibc shadPS4 process under Box64/FEX + Winlator X + Vortek). Here the session runs the
 * main repo's guest_cpu_fex backend *inside this app process* via [NativeFexSession] — a bounded
 * x86-64 smoke loop that proves the CPU backend is live. The native host/input DSO is loaded,
 * but the production game backend is not bound yet. The UI seam ([ManagedSession]) is
 * unchanged.
 *
 * Every published state is generation-tagged. The native SessionCore mints the generation and owns
 * lifecycle; this Service only observes phase/terminal transitions and maps them to UI state. A
 * late observer from an old session cannot clobber a new one ([ManagedSession.updateIfCurrent]), and
 * a guest fault is surfaced as Failed, never as a clean exit.
 */
class FexSessionService : Service() {

    @Volatile private var observer: Thread? = null
    // Set when a stop was requested for the live generation. Distinguishes "the
    // game is still running normally" (a WaitTerminal timeout is NOT a failure)
    // from "teardown was asked for and is now overdue" (a bounded failure).
    @Volatile private var stopRequestedGeneration: Long = 0L
    @Volatile private var stopDeadlineUptimeMs: Long = 0L

    override fun onBind(intent: Intent?): IBinder? = null

    override fun onStartCommand(intent: Intent?, flags: Int, startId: Int): Int {
        when (intent?.action) {
            ManagedSession.ACTION_START -> handleStart(intent, startId)
            ManagedSession.ACTION_STOP -> handleStop()
            else -> Log.w(TAG, "Unknown action: ${intent?.action}")
        }
        return START_NOT_STICKY
    }

    private fun handleStart(intent: Intent, startId: Int) {
        val gameId = intent.getStringExtra(ManagedSession.EXTRA_GAME_ID) ?: "smoke"

        if (!com.shadps4.android.runtime.input.NativePad.nativeInitializeHost(java.io.File(filesDir,"host").absolutePath)) {
            Log.e(TAG,"Host paths failed to initialize")
            return
        }
        val relativePath = intent.getStringExtra(ManagedSession.EXTRA_GAME_PATH)
        val generation = if (relativePath != null) {
            val executable = runCatching {
                require(GameInstallVerifier.canLaunch(filesDir, relativePath)) { "content is not installed" }
                val root = File(filesDir, relativePath).canonicalFile
                val entry = File(root, "eboot.bin").canonicalFile
                require(entry.toPath().startsWith(root.toPath())) { "entry escapes install directory" }
                entry.absolutePath
            }.getOrElse {
                Log.e(TAG, "Invalid installed game path", it)
                if (NativeFexSession.nativeCurrentGeneration() == 0L) stopSelf(startId)
                return
            }
            NativeFexSession.nativeStartExecutable(gameId, executable)
        } else if (gameId == "smoke" || gameId.endsWith("-cpu-smoke")) {
            NativeFexSession.nativeStart(gameId, DEFAULT_ITERATIONS)
        } else {
            Log.e(TAG, "Game launch requires an installed-content path")
            if (NativeFexSession.nativeCurrentGeneration() == 0L) stopSelf(startId)
            return
        }
        if (generation == 0L) {
            Log.w(TAG, "Session already running or failed to spawn; ignoring START")
            return
        }
        stopRequestedGeneration = 0L
        stopDeadlineUptimeMs = 0L
        ManagedSession.beginGeneration(generation)
        // Bind the native pad session so real controller input (physical gamepad +
        // touch overlay) reaches the native OrbisPadAdapter for this generation.
        GamepadInputManager.onSessionStart()
        NativePadBridge.begin(applicationContext,generation)
        ManagedSession.updateIfCurrent(generation, ManagedSessionState.Preparing("fex", generation))
        Log.i(TAG, "native: ${NativeFexSession.nativeIdentity()} gen=$generation")

        // One generation-tagged observer. It never fabricates Running: WaitPhase returns
        // TerminatedBeforeTarget when a session ends before reaching a phase, and we skip it.
        observer = thread(name = "fex-session-watch-$generation") {
            // Ready.
            val toReady = NativeFexSession.nativeWaitPhase(
                generation, NativeFexSession.PhaseOrdinal.READY, PHASE_DEADLINE_MS,
            )
            if (toReady == NativeFexSession.WaitPhase.REACHED_TARGET) {
                ManagedSession.updateIfCurrent(generation, ManagedSessionState.Ready(gameId, generation))
                // Running (only if it actually got there).
                val toRunning = NativeFexSession.nativeWaitPhase(
                    generation, NativeFexSession.PhaseOrdinal.RUNNING, PHASE_DEADLINE_MS,
                )
                if (toRunning == NativeFexSession.WaitPhase.REACHED_TARGET) {
                    ManagedSession.updateIfCurrent(generation, ManagedSessionState.Running(gameId, generation))
                }
            }

            // Wait for a REAL terminal. A running game has no fixed lifetime, so a
            // WaitTerminal timeout (-1) means "still owned / still running" and is
            // NOT a failure (review 2.4). Loop with a bounded slice so the thread
            // stays responsive; only publish + stop on a real terminal outcome.
            // The one exception is an overdue STOP: once a stop was requested, we
            // bound how long teardown may take, and exceeding that budget is a
            // genuine failure (teardown wedged), distinct from a happily running
            // game that was never asked to stop.
            var outcome = NativeFexSession.Outcome.TIMEOUT
            while (true) {
                outcome = NativeFexSession.nativeWaitTerminal(generation, WAIT_SLICE_MS)
                if (outcome != NativeFexSession.Outcome.TIMEOUT) {
                    break  // a real terminal (Returned/Cancelled/Faulted/...)
                }
                // Still running. If a stop was asked for this generation and the
                // stop budget has now elapsed without a terminal, treat teardown
                // as wedged and stop observing with a failure.
                val stopGen = stopRequestedGeneration
                if (stopGen == generation) {
                    val deadline = stopDeadlineUptimeMs
                    if (deadline != 0L && android.os.SystemClock.uptimeMillis() >= deadline) {
                        Log.w(TAG, "stop for gen=$generation overdue; teardown wedged")
                        break  // outcome stays TIMEOUT -> published as a failure
                    }
                }
                // else: no stop requested -> keep waiting indefinitely (the game
                // is allowed to run for hours).
            }
            publishTerminal(gameId, generation, outcome)
            // Tear down the native pad session for this generation: detaches the
            // controller sink and clears port state so a late producer cannot write
            // into the next session's pad.
            NativePadBridge.end(generation)

            stopSelf(startId)
        }
    }

    private fun handleStop() {
        val generation = NativeFexSession.nativeCurrentGeneration()
        if (generation == 0L) return
        ManagedSession.updateIfCurrent(generation, ManagedSessionState.Stopping("", generation))
        // Mark the stop and give teardown a bounded budget; the observer treats a
        // WaitTerminal timeout past this budget as a wedged teardown, not as a
        // still-running game.
        stopRequestedGeneration = generation
        stopDeadlineUptimeMs = android.os.SystemClock.uptimeMillis() + STOP_COMPLETION_BUDGET_MS
        NativePadBridge.requestStop(generation)
        // Async interrupt from a worker thread; the observer publishes the terminal.
        thread(name = "fex-session-stop-$generation") {
            NativeFexSession.nativeRequestStop(generation, STOP_TIMEOUT_MS)
        }
    }

    private fun publishTerminal(gameId: String, generation: Long, outcome: Int) {
        val detail = NativeFexSession.nativeTerminalDetail(generation)
        val state = when (outcome) {
            NativeFexSession.Outcome.RETURNED ->
                ManagedSessionState.Stopped(
                    exitCode = 0,
                    generation = generation,
                    termination = ProcessTerminationInfo(
                        terminationKind = TerminationKind.EXITED,
                        exitCode = 0,
                        userRequestedStop = false,
                    ),
                )
            NativeFexSession.Outcome.CANCELLED ->
                ManagedSessionState.Stopped(
                    exitCode = 0,
                    generation = generation,
                    termination = ProcessTerminationInfo(
                        terminationKind = TerminationKind.CANCELLED_BY_USER,
                        exitCode = 0,
                        userRequestedStop = true,
                    ),
                )
            // GuestFault / BackendFailure / Unsupported / Unexpected / StartFailed / Timeout(-1):
            // all real failures. Never exit-0.
            else ->
                ManagedSessionState.Failed(
                    code = RuntimeErrorCode.BACKEND_CRASHED,
                    detail = terminalDetailFor(outcome, detail),
                    generation = generation,
                )
        }
        ManagedSession.updateIfCurrent(generation, state)
    }

    private fun terminalDetailFor(outcome: Int, detail: String?): String {
        val base = when (outcome) {
            NativeFexSession.Outcome.FAULTED -> "guest fault"
            NativeFexSession.Outcome.BACKEND_FAILED -> "backend failure"
            NativeFexSession.Outcome.UNSUPPORTED -> "unsupported"
            NativeFexSession.Outcome.UNEXPECTED -> "unexpected stop"
            NativeFexSession.Outcome.START_FAILED -> "session failed to start"
            NativeFexSession.Outcome.TIMEOUT -> "stop timed out; session not exited"
            else -> "session failed (outcome=$outcome)"
        }
        return if (detail.isNullOrEmpty()) base else "$base: $detail"
    }

    override fun onDestroy() {
        // Do not block the main thread on a multi-second JNI wait (review 2.4).
        // Hand teardown to a detached owner thread: request the stop, mark the
        // stop budget so the observer treats an overdue teardown as wedged, and
        // let the observer publish the terminal and reclaim. onDestroy returns
        // immediately.
        val generation = NativeFexSession.nativeCurrentGeneration()
        if (generation != 0L) {
            stopRequestedGeneration = generation
            stopDeadlineUptimeMs = android.os.SystemClock.uptimeMillis() + STOP_COMPLETION_BUDGET_MS
            thread(name = "fex-session-destroy-$generation") {
                NativeFexSession.nativeRequestStop(generation, STOP_TIMEOUT_MS)
            }
        }
        // Safety net: stop haptics even if the observer never published a terminal.
        NativePadBridge.end(generation)
        super.onDestroy()
    }

    private companion object {
        const val TAG = "FexSessionService"
        // Large but finite: stays in the JIT long enough for Stop to interrupt, yet a natural
        // Returned end is reachable. ~4 billion single-instruction iterations.
        const val DEFAULT_ITERATIONS = 1L shl 32
        const val STOP_TIMEOUT_MS = 1000L
        const val PHASE_DEADLINE_MS = 5000L
        // Bounded slice the observer waits on each terminal poll. A timeout on a
        // slice means "still running", not a failure: a real game runs for hours.
        const val WAIT_SLICE_MS = 1000L
        // Once a stop is requested, teardown must complete within this budget or it
        // is treated as wedged (a real failure), distinct from a running game.
        const val STOP_COMPLETION_BUDGET_MS = 15000L
    }
}
