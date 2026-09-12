package com.shadps4.android.runtime.input

/**
 * Executes controller haptics drained from the native pad adapter.
 *
 * The guest's `scePadSetVibration` (later stage) — or, today, any producer —
 * enqueues a latest-wins [PadVibration][NativePad.Rumble] per port in the native
 * OrbisPadAdapter. This is the injection seam that turns a drained command into an
 * actual device effect. It is deliberately an interface so:
 *   - the [HapticsPump] can be unit-tested with a fake sink (no device Vibrator),
 *   - the Android [VibratorHapticsSink] is one implementation, matching the
 *     input-first architecture where the reusable mechanism is injected, not a
 *     hard singleton.
 *
 * The pump never calls into Java on the input lock; it drains the native queue and
 * calls the sink from its own worker thread.
 */
interface HapticsSink {
    /**
     * Play a rumble effect on [port]. [smallMotor]/[largeMotor] are 0..255 (the DS4
     * high/low-frequency motors). A (0,0) command is a stop and arrives via [cancel].
     */
    fun rumble(port: Int, smallMotor: Int, largeMotor: Int)

    /** Stop any ongoing effect on [port]. */
    fun cancel(port: Int)

    /** Stop all effects (session end / teardown). */
    fun cancelAll()
}
