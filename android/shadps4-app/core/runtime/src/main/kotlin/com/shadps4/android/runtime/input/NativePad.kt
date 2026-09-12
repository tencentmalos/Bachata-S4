package com.shadps4.android.runtime.input

/**
 * JNI bridge to the native OrbisPadAdapter in libshadps4_fex_session.so — the
 * native consumer of Android controller input.
 *
 * The Android front end (physical gamepad via [GamepadInputManager], or the touch
 * overlay) resolves every source into a [ControllerSnapshot] whose `buttons` are
 * already in PS4 [Ps4Button] bit form, sticks in [-1,1], triggers in [0,1]. This
 * object forwards that snapshot to the process-global native adapter, which stores
 * per-port `OrbisPadData` in the exact production layout the eventual guest
 * `scePadReadState` reads.
 *
 * Session/generation model (mirrors the native SessionCore):
 *   - [beginSession] starts a new input generation and returns a non-zero token.
 *     Every [submit]/[setVibration] carries that token; a stale token from a
 *     previous session is refused natively, so a late producer cannot write into
 *     the current session's pad.
 *   - [endSession] with the live token clears all port state and queued feedback.
 *
 * This does NOT itself run a game: it makes real controller input reach a real PS4
 * pad-state consumer. Wiring that native state into the FEX guest's
 * `scePadReadState` is a later stage (the Android host is not yet native).
 *
 * The native library is loaded by [com.shadps4.android.runtime.session.NativeFexSession];
 * the pad symbols live in the same `.so`, so this object does not re-load it.
 */
object NativePad {

    /** PadResult ordinals — mirror Core::HostRuntime::PadResult. */
    object Result {
        const val OK = 0
        const val WRONG_SESSION = 1
        const val BAD_PORT = 2
        const val REJECTED = 3
        const val NO_SESSION = 4
    }

    const val MAX_PORTS = 4

    /** Start a new input session; returns the new non-zero token (0 on failure). */
    external fun nativeBeginSession(): Long

    /** End [token] if it is the live session; clears all port state. */
    external fun nativeEndSession(token: Long)

    /** The live session token, or 0 when idle. */
    external fun nativeCurrentToken(): Long

    /** Submit one port's snapshot. Returns a [Result] ordinal. */
    external fun nativeSubmit(
        token: Long,
        port: Int,
        buttons: Long,
        leftX: Float,
        leftY: Float,
        rightX: Float,
        rightY: Float,
        leftTrigger: Float,
        rightTrigger: Float,
        touchDown: Boolean,
        touchX: Float,
        touchY: Float,
    ): Int

    /** Mark [port] connected/disconnected within the live session. */
    external fun nativeSetConnected(token: Long, port: Int, connected: Boolean): Int

    /** Current PS4 button bits stored for [port] (verification/telemetry). */
    external fun nativeReadButtons(port: Int): Long

    /** Converted analog state [leftX,leftY,rightX,rightY,l2,r2] as u8 ints, or null. */
    external fun nativeReadAnalog(port: Int): IntArray?

    external fun nativeConnected(port: Int): Boolean

    /** Enqueue a latest-wins vibration (0..255 each); (0,0) is a cancel. */
    external fun nativeSetVibration(token: Long, port: Int, smallMotor: Int, largeMotor: Int): Int

    /**
     * Drain a pending vibration for [port]. Returns -1 (none), 0 (cancel), or
     * (small shl 8) or large for an active command.
     */
    external fun nativeDrainVibration(port: Int): Int

    // --- Kotlin-friendly wrappers ------------------------------------------------

    /** Forward a resolved [snapshot] for [port] under [token]. */
    fun submit(token: Long, port: Int, snapshot: ControllerSnapshot): Int =
        nativeSubmit(
            token,
            port,
            snapshot.buttons,
            snapshot.leftX,
            snapshot.leftY,
            snapshot.rightX,
            snapshot.rightY,
            snapshot.leftTrigger,
            snapshot.rightTrigger,
            snapshot.touchDown,
            snapshot.touchX,
            snapshot.touchY,
        )

    /** A drained vibration command, or null when nothing is pending. */
    data class Rumble(val smallMotor: Int, val largeMotor: Int, val cancel: Boolean)

    fun drainVibration(port: Int): Rumble? {
        val v = nativeDrainVibration(port)
        return when {
            v < 0 -> null
            v == 0 -> Rumble(0, 0, cancel = true)
            else -> Rumble((v ushr 8) and 0xFF, v and 0xFF, cancel = false)
        }
    }
}
