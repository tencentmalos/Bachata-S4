package com.shadps4.android.runtime.input

import kotlin.math.abs

data class ConnectedController(val deviceId: Int, val key: ControllerDeviceKey, val hasHatDpad: Boolean = false)

class ControllerBindingResolver {
    fun assignSlots(profiles: List<ControllerProfile>, devices: List<ConnectedController>): Map<Int, Int> {
        require(profiles.size <= 4) { "Only four controller slots are supported" }
        val unused = devices.toMutableList()
        return buildMap {
            profiles.forEachIndexed { slot, profile ->
                val match = unused.firstOrNull { profile.device == null || sameDevice(profile.device, it.key) }
                if (match != null) { put(slot, match.deviceId); unused.remove(match) }
            }
        }
    }

    /**
     * Port for a pad that has just connected, or null when every port it may use is taken.
     * A port configured for this pad comes first. Next a port configured for the same model whose
     * own pad is not connected: the saved descriptor of a re-enumerated pad no longer matches,
     * and without this its player 1 binding would leave the pad on a port the game does not
     * read. Last the first port without a configured pad.
     */
    fun portFor(
        profiles: List<ControllerProfile>,
        occupied: Set<Int>,
        device: ControllerDeviceKey,
        otherConnected: Collection<ControllerDeviceKey>,
    ): Int? {
        val free = profiles.indices.filter { it !in occupied }
        free.firstOrNull { profiles[it].device?.sameDescriptor(device) == true }?.let { return it }
        free.firstOrNull { port ->
            val wanted = profiles[port].device
            wanted != null && wanted.sameModel(device) && otherConnected.none(wanted::sameDescriptor)
        }?.let { return it }
        return free.firstOrNull { profiles[it].device == null }
    }

    fun snapshot(profile: ControllerProfile, values: Map<PhysicalBinding, Float>): ControllerSnapshot {
        fun value(control: String): Float {
            val binding = profile.bindingFor(control)
            val raw = binding?.let { values[it] } ?: 0f
            val dead = if (control.endsWith("_x") || control.endsWith("_y")) raw.takeUnless { abs(it) < profile.deadZone } ?: 0f else raw
            return if (control in profile.invertAxes) -dead else dead
        }
        fun buttonActive(control: String): Boolean {
            val binding = profile.bindingFor(control) ?: return false
            val v = values[binding] ?: 0f
            return when (binding.direction) {
                AxisDirection.NEGATIVE -> v <= -BUTTON_THRESHOLD
                AxisDirection.POSITIVE -> v >= BUTTON_THRESHOLD
                AxisDirection.BOTH -> v >= BUTTON_THRESHOLD
            }
        }
        var buttons = 0L
        BUTTON_BITS.forEach { (control, bit) -> if (buttonActive(control)) buttons = buttons or bit }
        val leftTrigger = value("left_trigger").coerceIn(0f, 1f)
        val rightTrigger = value("right_trigger").coerceIn(0f, 1f)
        if (leftTrigger >= profile.triggerThreshold || value("l2") >= 0.5f) buttons = buttons or Ps4Button.L2
        if (rightTrigger >= profile.triggerThreshold || value("r2") >= 0.5f) buttons = buttons or Ps4Button.R2
        return ControllerSnapshot.normalized(
            buttons = buttons,
            leftX = value("left_x"), leftY = value("left_y"), rightX = value("right_x"), rightY = value("right_y"),
            leftTrigger = leftTrigger, rightTrigger = rightTrigger,
        )
    }

    fun snapshotOrNeutral(profile: ControllerProfile, device: ControllerDeviceKey?, values: Map<PhysicalBinding, Float>): ControllerSnapshot =
        if (profile.device != null && (device == null || !sameDevice(profile.device, device))) ControllerSnapshot.Neutral else snapshot(profile, values)

    private fun sameDevice(expected: ControllerDeviceKey, actual: ControllerDeviceKey): Boolean =
        expected.sameDescriptor(actual) || expected.sameModel(actual)

    private companion object {
        const val BUTTON_THRESHOLD = 0.5f
        val BUTTON_BITS = mapOf(
            "cross" to Ps4Button.CROSS, "circle" to Ps4Button.CIRCLE, "square" to Ps4Button.SQUARE, "triangle" to Ps4Button.TRIANGLE,
            "l1" to Ps4Button.L1, "r1" to Ps4Button.R1, "l3" to Ps4Button.L3, "r3" to Ps4Button.R3,
            "dpad_up" to Ps4Button.UP, "dpad_down" to Ps4Button.DOWN, "dpad_left" to Ps4Button.LEFT, "dpad_right" to Ps4Button.RIGHT,
            "options" to Ps4Button.OPTIONS, "touchpad" to Ps4Button.TOUCH_PAD, "ps" to Ps4Button.PS,
        )
    }
}
