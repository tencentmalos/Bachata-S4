package com.shadps4.android.runtime.input

import android.util.Log
import com.shadps4.android.runtime.session.ManagedSession
import java.util.concurrent.atomic.AtomicLong

/**
 * Connects the Android controller producers to the native [NativePad] consumer.
 *
 * Before this bridge, [ManagedSession.submitController] published a resolved
 * [ControllerSnapshot] to a Kotlin sink that had no native consumer — physical
 * gamepad and touch-overlay input reached Kotlin and stopped. This bridge attaches
 * that sink and forwards every snapshot to the process-global native OrbisPadAdapter
 * under a session token, so real input reaches a real PS4 pad-state consumer.
 *
 * Lifetime follows the game session:
 *   - [begin] mints a native input session token (a new generation), marks slot 0
 *     connected, and attaches the [ManagedSession] slot sink.
 *   - [end] detaches the sink and ends the native session (clears port state).
 *
 * The token guards against a stale producer from a previous session: a snapshot
 * that arrives after [end] carries the old token and is refused natively. The sink
 * runs on the caller's thread (the input dispatch / UI thread); the native adapter
 * is internally locked, so no additional synchronization is needed here.
 */
object NativePadBridge {

    private const val TAG = "NativePadBridge"

    // The live native session token, or 0 when no session is bound.
    private val token = AtomicLong(0)

    // Held so detach uses the exact same lambda identity attach used.
    private val slotSink: (Int, ControllerSnapshot) -> Unit = ::onSnapshot

    /**
     * Begin a native input session. Returns the token (0 on failure). Idempotent
     * for a given game session: a second begin without an end is ignored.
     */
    @Synchronized
    fun begin(): Long {
        if (token.get() != 0L) {
            return token.get()
        }
        val t = NativePad.nativeBeginSession()
        if (t == 0L) {
            Log.w(TAG, "nativeBeginSession failed")
            return 0L
        }
        token.set(t)
        // Slot 0 is the primary pad; overlay + first physical controller drive it.
        NativePad.nativeSetConnected(t, 0, true)
        ManagedSession.attachControllerSlotSink(slotSink)
        Log.i(TAG, "native pad session begun token=$t")
        return t
    }

    /** End the native input session and detach the sink. Idempotent. */
    @Synchronized
    fun end() {
        val t = token.getAndSet(0)
        ManagedSession.detachControllerSlotSink(slotSink)
        if (t != 0L) {
            NativePad.nativeEndSession(t)
            Log.i(TAG, "native pad session ended token=$t")
        }
    }

    /** The live token, or 0. */
    fun currentToken(): Long = token.get()

    private fun onSnapshot(slot: Int, snapshot: ControllerSnapshot) {
        val t = token.get()
        if (t == 0L) return
        if (slot !in 0 until NativePad.MAX_PORTS) return
        val r = NativePad.submit(t, slot, snapshot)
        if (r != NativePad.Result.OK && r != NativePad.Result.WRONG_SESSION) {
            // WRONG_SESSION is expected briefly across an end/begin race; anything
            // else (BAD_PORT/REJECTED/NO_SESSION) is a real submit problem.
            Log.w(TAG, "submit slot=$slot returned $r")
        }
    }
}
