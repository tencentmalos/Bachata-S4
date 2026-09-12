package com.shadps4.android.runtime.input

import android.content.Context
import android.os.Build
import android.os.VibrationEffect
import android.os.Vibrator
import android.os.VibratorManager
import android.util.Log

/**
 * [HapticsSink] backed by the Android [VibratorManager] / [Vibrator].
 *
 * Maps the two DS4 motors (small = high-frequency, large = low-frequency, each
 * 0..255) to a single combined amplitude, because most phones/handhelds expose one
 * actuator. A one-shot effect is (re)issued each drain tick; the native adapter's
 * latest-wins slot means only the newest command per tick reaches here, so the
 * effect tracks the guest's requested intensity without a queue building up.
 *
 * Constructed from a [Context] (application context is fine); no static state. If
 * the device has no vibrator, every call is a safe no-op.
 */
class VibratorHapticsSink(context: Context) : HapticsSink {

    private val vibrator: Vibrator? = resolveVibrator(context)
    private val hasAmplitudeControl: Boolean = vibrator?.hasAmplitudeControl() == true

    override fun rumble(port: Int, smallMotor: Int, largeMotor: Int) {
        val v = vibrator ?: return
        // Combine the two motors: the stronger of the two drives perceived intensity.
        val level = maxOf(smallMotor.coerceIn(0, 255), largeMotor.coerceIn(0, 255))
        if (level == 0) {
            cancel(port)
            return
        }
        try {
            val effect = if (hasAmplitudeControl) {
                VibrationEffect.createOneShot(EFFECT_MS, level)
            } else {
                // No amplitude control: a plain one-shot at the default amplitude.
                VibrationEffect.createOneShot(EFFECT_MS, VibrationEffect.DEFAULT_AMPLITUDE)
            }
            v.vibrate(effect)
        } catch (t: Throwable) {
            Log.w(TAG, "vibrate failed", t)
        }
    }

    override fun cancel(port: Int) {
        try {
            vibrator?.cancel()
        } catch (t: Throwable) {
            Log.w(TAG, "cancel failed", t)
        }
    }

    override fun cancelAll() {
        cancel(-1)
    }

    private companion object {
        const val TAG = "VibratorHapticsSink"
        // Slightly longer than the pump interval so a sustained rumble is continuous;
        // a new one-shot each tick refreshes it, and a cancel stops it promptly.
        const val EFFECT_MS = 40L

        fun resolveVibrator(context: Context): Vibrator? = try {
            if (Build.VERSION.SDK_INT >= Build.VERSION_CODES.S) {
                val mgr = context.getSystemService(Context.VIBRATOR_MANAGER_SERVICE) as? VibratorManager
                mgr?.defaultVibrator
            } else {
                @Suppress("DEPRECATION")
                context.getSystemService(Context.VIBRATOR_SERVICE) as? Vibrator
            }
        } catch (t: Throwable) {
            Log.w(TAG, "no vibrator", t)
            null
        }
    }
}
