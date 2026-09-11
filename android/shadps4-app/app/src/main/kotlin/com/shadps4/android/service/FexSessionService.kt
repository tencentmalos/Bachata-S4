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
 * not yet native; see the plan / docs). The UI seam ([ManagedSession]) is unchanged.
 */
class FexSessionService : Service() {

    override fun onBind(intent: Intent?): IBinder? = null

    override fun onStartCommand(intent: Intent?, flags: Int, startId: Int): Int {
        when (intent?.action) {
            ManagedSession.ACTION_START -> handleStart(intent)
            ManagedSession.ACTION_STOP -> handleStop()
            else -> Log.w(TAG, "Unknown action: ${intent?.action}")
        }
        return START_NOT_STICKY
    }

    private fun handleStart(intent: Intent) {
        val gameId = intent.getStringExtra(ManagedSession.EXTRA_GAME_ID) ?: "smoke"
        if (NativeFexSession.nativeIsRunning()) {
            Log.w(TAG, "Session already running; ignoring START")
            return
        }
        ManagedSession.update(ManagedSessionState.Preparing("fex"))
        Log.i(TAG, "native: " + NativeFexSession.nativeIdentity())

        // Watcher thread: nativeStart spins its own native owner thread and returns immediately once
        // the guest is running. We publish Running, then poll until the native session ends (natural
        // Returned, user Cancel, or fault) and publish the terminal state. The UI thread never blocks.
        thread(name = "fex-session-watch") {
            val started = NativeFexSession.nativeStart(DEFAULT_ITERATIONS)
            if (!started) {
                publishFailed(gameId, "session did not start")
                return@thread
            }
            ManagedSession.update(ManagedSessionState.Running(gameId))
            // Poll for completion. Stop is driven by handleStop() on another thread.
            while (NativeFexSession.nativeIsRunning()) {
                Thread.sleep(50)
            }
            val reason = NativeFexSession.nativeLastStopReason()
            val err = NativeFexSession.nativeLastError()
            if (err != null) {
                publishFailed(gameId, err)
            } else {
                // StopReason ordinal: see api/execution.h. Returned/Cancelled are clean stops.
                val userStop = reason == STOP_CANCELLED
                ManagedSession.update(
                    ManagedSessionState.Stopped(
                        exitCode = 0,
                        termination = ProcessTerminationInfo(
                            terminationKind = if (userStop) TerminationKind.CANCELLED_BY_USER
                            else TerminationKind.EXITED,
                            exitCode = 0,
                            userRequestedStop = userStop,
                        ),
                    ),
                )
            }
            stopSelf()
        }
    }

    private fun handleStop() {
        // Async interrupt from this (non-owner) thread; the native watcher observes the stop and
        // publishes Stopped. Bounded to ~1s, matching the CPU contract's Stop budget.
        thread(name = "fex-session-stop") {
            NativeFexSession.nativeRequestStop(STOP_TIMEOUT_MS)
        }
    }

    private fun publishFailed(gameId: String, detail: String) {
        Log.e(TAG, "session failed for $gameId: $detail")
        ManagedSession.update(
            ManagedSessionState.Failed(RuntimeErrorCode.BACKEND_CRASHED, detail),
        )
        stopSelf()
    }

    private companion object {
        const val TAG = "FexSessionService"
        // Large but finite: stays in the JIT long enough for Stop to interrupt, yet a natural
        // Returned end is reachable. ~4 billion single-instruction iterations.
        const val DEFAULT_ITERATIONS = 1L shl 32
        const val STOP_TIMEOUT_MS = 1000L
        // StopReason ordinals from src/core/guest_cpu/api/execution.h (Returned=0 ... Cancelled=2).
        const val STOP_CANCELLED = 2
    }
}
