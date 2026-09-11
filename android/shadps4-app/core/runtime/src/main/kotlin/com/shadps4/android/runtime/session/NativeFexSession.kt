package com.shadps4.android.runtime.session

/**
 * JNI bridge to libshadps4_fex_session.so — the in-process FEX "smoke session".
 *
 * This drives the same guest_cpu_fex backend the CLI / fex-validation app use, inside this normal
 * app process. It runs a bounded x86-64 loop as proof the CPU backend is live; it does NOT run a
 * real PS4 game (the Android host is not yet native). See docs/validation/round2 and the plan.
 */
object NativeFexSession {
    init {
        System.loadLibrary("shadps4_fex_session")
    }

    /** page_size / pid / uid / loaded-library Build ID. */
    external fun nativeIdentity(): String

    /**
     * Start the smoke session on a dedicated native owner thread. `iterations` bounds the guest
     * loop (large but finite; <=0 uses a large default). Returns false if a session is already
     * running.
     */
    external fun nativeStart(iterations: Long): Boolean

    /** Async stop: RequestInterrupt(Cancel) + WaitStopped, then join the owner. */
    external fun nativeRequestStop(timeoutMs: Long): Boolean

    external fun nativeIsRunning(): Boolean

    /** StopReason ordinal of the last run, or -1 if none yet. */
    external fun nativeLastStopReason(): Int

    /** Last error string, or null. */
    external fun nativeLastError(): String?
}
