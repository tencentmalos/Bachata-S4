package com.shadps4.android.runtime.input

import android.util.Log
import java.util.concurrent.atomic.AtomicBoolean
import kotlin.concurrent.thread

/**
 * Drains native pad vibration commands and forwards them to a [HapticsSink].
 *
 * The native OrbisPadAdapter holds a latest-wins vibration slot per port
 * (`scePadSetVibration` -> [NativePad.nativeSetVibration]). Nothing pulled that
 * slot to real hardware before this pump. [start] runs a bounded polling loop on a
 * worker thread; each tick drains every port via [NativePad.drainVibration] and
 * calls the sink. A drain that returns null means nothing pending (no work). A
 * cancel (motors 0) maps to [HapticsSink.cancel].
 *
 * Bounded and stoppable: [stop] cancels all effects and joins the worker within a
 * short budget so session teardown is not blocked. The pump owns no session token;
 * draining is intentionally not session-guarded so a final cancel can be delivered
 * after the session ends (the native adapter clears the queue on a new session, so
 * a stale rumble cannot leak into the next game).
 *
 * Injectable: the sink is supplied by the caller (the Android VibratorManager
 * implementation in production, a fake in tests) — no device dependency here.
 */
class HapticsPump(
    private val sink: HapticsSink,
    private val pollIntervalMs: Long = DEFAULT_POLL_MS,
    private val ports: Int = NativePad.MAX_PORTS,
) {
    private val running = AtomicBoolean(false)
    @Volatile private var worker: Thread? = null

    /** Start the polling loop. Idempotent; a second start while running is a no-op. */
    fun start() {
        if (!running.compareAndSet(false, true)) return
        worker = thread(name = "haptics-pump") {
            while (running.get()) {
                try {
                    pumpOnce()
                } catch (t: Throwable) {
                    Log.w(TAG, "haptics pump tick failed", t)
                }
                if (!running.get()) break
                try {
                    Thread.sleep(pollIntervalMs)
                } catch (_: InterruptedException) {
                    // stop() interrupts; loop condition handles exit.
                }
            }
        }
    }

    /** Stop the loop, cancel all effects, and join the worker within a short budget. */
    fun stop() {
        if (!running.compareAndSet(true, false)) return
        val w = worker
        worker = null
        w?.interrupt()
        w?.join(JOIN_BUDGET_MS)
        try {
            sink.cancelAll()
        } catch (t: Throwable) {
            Log.w(TAG, "cancelAll failed", t)
        }
    }

    /** One drain pass over every port. Exposed for tests (no thread needed). */
    fun pumpOnce() {
        for (port in 0 until ports) {
            val cmd = NativePad.drainVibration(port) ?: continue
            if (cmd.cancel) {
                sink.cancel(port)
            } else {
                sink.rumble(port, cmd.smallMotor, cmd.largeMotor)
            }
        }
    }

    private companion object {
        const val TAG = "HapticsPump"
        // Fast enough that a rumble feels responsive, slow enough to be cheap.
        const val DEFAULT_POLL_MS = 16L
        const val JOIN_BUDGET_MS = 500L
    }
}
