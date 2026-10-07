package com.shadps4.android.runtime.settings

import kotlinx.serialization.json.JsonPrimitive

/** What places a PSVR title's DualShock 4: nothing, the right or both headset controllers, or
 *  hand tracking around a real gamepad. */
object XrDs4Pose {
    const val ID = "input.xr_ds4_pose"
    /** Native values of Core::HostRuntime::Ds4PoseSource. */
    const val OFF = 0
    const val RIGHT = 1
    const val BOTH = 2
    const val HANDS = 3
    fun resolve(global: RuntimeProfile, game: RuntimeProfile): Int {
        val value = game.values[ID] ?: global.values[ID] ?: return OFF
        return when ((value as? JsonPrimitive)?.content) {
            "off" -> OFF
            "right" -> RIGHT
            "both" -> BOTH
            "hands" -> HANDS
            else -> throw IllegalArgumentException("Invalid XR DualShock 4 pose: $value")
        }
    }
}
