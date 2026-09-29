package com.shadps4.android.runtime.session

import android.content.Context
import android.content.Intent
import android.content.IntentFilter
import android.os.BatteryManager

/**
 * Battery values for the native status HUD. Apps may not read /sys/class/power_supply
 * (SELinux on Swan), so the service reads BatteryManager and the sticky ACTION_BATTERY_CHANGED
 * broadcast and hands the result to the host. Values a device does not report are passed as
 * missing (NaN / Long.MIN_VALUE / -1), never as 0.
 */
object HostBattery {
    private const val MISSING = Long.MIN_VALUE

    fun publish(context: Context) {
        val intent = try {
            context.registerReceiver(null, IntentFilter(Intent.ACTION_BATTERY_CHANGED))
        } catch (e: RuntimeException) {
            null
        }
        val manager = context.getSystemService(BatteryManager::class.java)
        val level = intent?.getIntExtra(BatteryManager.EXTRA_LEVEL, -1) ?: -1
        val scale = intent?.getIntExtra(BatteryManager.EXTRA_SCALE, -1) ?: -1
        val percent = if (level >= 0 && scale > 0) level * 100f / scale else Float.NaN
        // CURRENT_NOW is Long.MIN_VALUE when unsupported and 0 on devices that do not report it.
        val current = manager?.getLongProperty(BatteryManager.BATTERY_PROPERTY_CURRENT_NOW)
            ?.takeIf { it != Long.MIN_VALUE && it != 0L } ?: MISSING
        val charge = manager?.getIntProperty(BatteryManager.BATTERY_PROPERTY_CHARGE_COUNTER)
            ?.takeIf { it != Int.MIN_VALUE && it > 0 }?.toLong() ?: MISSING
        val voltage = intent?.getIntExtra(BatteryManager.EXTRA_VOLTAGE, -1)
            ?.takeIf { it > 0 }?.let { it * 1000L } ?: MISSING
        val temperature = intent?.getIntExtra(BatteryManager.EXTRA_TEMPERATURE, Int.MIN_VALUE)
            ?.takeIf { it != Int.MIN_VALUE }?.let { it / 10f } ?: Float.NaN
        val status = intent?.getIntExtra(BatteryManager.EXTRA_STATUS, -1) ?: -1
        val plugged = intent?.getIntExtra(BatteryManager.EXTRA_PLUGGED, -1) ?: -1
        val charging = when {
            status == BatteryManager.BATTERY_STATUS_CHARGING ||
                status == BatteryManager.BATTERY_STATUS_FULL || plugged > 0 -> 1
            status >= 0 || plugged == 0 -> 0
            else -> -1
        }
        NativeFexSession.nativeSetHostBattery(
            intent != null || manager != null, percent, current, voltage, charge, temperature, charging)
    }
}
