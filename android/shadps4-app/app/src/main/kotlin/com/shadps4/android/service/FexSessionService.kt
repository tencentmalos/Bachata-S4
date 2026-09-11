package com.shadps4.android.service

import android.app.Service
import android.content.Intent
import android.os.IBinder
import android.util.Log
import com.shadps4.android.model.RuntimeErrorCode
import com.shadps4.android.runtime.diagnostics.ProcessTerminationInfo
import com.shadps4.android.runtime.diagnostics.TerminationKind
import com.shadps4.android.runtime.session.ManagedSession
import com.shadps4.android.runtime.session.ManagedSessionState
import com.shadps4.android.runtime.session.NativeFexSession
import kotlin.concurrent.thread

/**
 * In-process FEX session orchestrator. Replaces the reference EmulationService (which launched an
 * external glibc shadPS4 process under Box64/FEX + Winlator X + Vortek). Here the session runs the
 * main repo's guest_cpu_fex backend *inside this app process* via [NativeFexSession] — a bounded
 * x86-64 smoke loop that proves the CPU backend is live. It is NOT a real game (the Android host is
 * not yet native; see docs/specs/android-native-host-v1.md). The UI seam ([ManagedSession]) is
 * unchanged.
 *
 * Every published state is generation-tagged. The native SessionCore mints the generation and owns
 * lifecycle; this Service only observes phase/terminal transitions and maps them to UI state. A
 * late observer from an old session cannot clobber a new one ([ManagedSession.updateIfCurrent]), and
 * a guest fault is surfaced as Failed, never as a clean exit.
 */
class FexSessionService : Service() {

    @Volatile private var observer: Thread? = null

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

        val generation = NativeFexSession.nativeStart(gameId, DEFAULT_ITERATIONS)
        if (generation == 0L) {
            Log.w(TAG, "Session already running or failed to spawn; ignoring START")
            return
        }
        ManagedSession.beginGeneration(generation)
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

            // Terminal. -1 = timeout (session still owned): report a failure, do not go Idle.
            val outcome = NativeFexSession.nativeWaitTerminal(generation, TERMINAL_DEADLINE_MS)
            publishTerminal(gameId, generation, outcome)
            stopSelf(startId)
        }
    }

    private fun handleStop() {
        val generation = NativeFexSession.nativeCurrentGeneration()
        if (generation == 0L) return
        ManagedSession.updateIfCurrent(generation, ManagedSessionState.Stopping("", generation))
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
        // Do not leave an ownerless native thread. Stop the current generation and wait bounded.
        val generation = NativeFexSession.nativeCurrentGeneration()
        if (generation != 0L) {
            NativeFexSession.nativeRequestStop(generation, STOP_TIMEOUT_MS)
            NativeFexSession.nativeWaitTerminal(generation, TERMINAL_DEADLINE_MS)
        }
        super.onDestroy()
    }

    private companion object {
        const val TAG = "FexSessionService"
        // Large but finite: stays in the JIT long enough for Stop to interrupt, yet a natural
        // Returned end is reachable. ~4 billion single-instruction iterations.
        const val DEFAULT_ITERATIONS = 1L shl 32
        const val STOP_TIMEOUT_MS = 1000L
        const val PHASE_DEADLINE_MS = 5000L
        const val TERMINAL_DEADLINE_MS = 10000L
    }
}
