package com.shadps4.android.feature.session

import com.shadps4.android.runtime.input.NavControllerEvent

/** Edge-trigger navigation: a held stick/HAT moves once until released or reversed. */
internal class SessionMenuInput {
    enum class Command { Previous, Next, Activate, Dismiss }
    private val held = mutableMapOf<String, Int>()

    fun accept(event: NavControllerEvent): Command? {
        val direction = if (!event.pressed) 0 else if (event.value < 0) -1 else 1
        val previous = held.put(event.control, direction) ?: 0
        if (direction == 0 || direction == previous) return null
        return when (event.control) {
            "dpad_up", "dpad_left" -> Command.Previous
            "dpad_down", "dpad_right" -> Command.Next
            "left_x", "left_y" -> if (direction < 0) Command.Previous else Command.Next
            "cross" -> Command.Activate
            "circle", "options", "ps" -> Command.Dismiss
            else -> null
        }
    }
}
