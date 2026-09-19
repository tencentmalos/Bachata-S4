package com.shadps4.android.runtime.input

import spatial.input.model.Button
import spatial.input.model.InputEvent

/** App policy after Foundation's neutral collection, before the Orbis adapter.
 * Does not change overlay events, Android keycodes, or Foundation's position ABI.
 */
class NativeButtonMapping(profile: ControllerProfile) {
    private val targets: Map<Button, List<Button>> = buildMap {
        for ((control, target) in LOGICAL_BUTTONS) {
            val binding = profile.bindingFor(control) ?: continue
            val source = when (binding.kind) {
                PhysicalBindingKind.BUTTON -> KEY_BUTTONS[binding.code]
                PhysicalBindingKind.AXIS -> when {
                    binding.code == 15 && binding.direction == AxisDirection.NEGATIVE -> Button.DpadLeft
                    binding.code == 15 && binding.direction == AxisDirection.POSITIVE -> Button.DpadRight
                    binding.code == 16 && binding.direction == AxisDirection.NEGATIVE -> Button.DpadUp
                    binding.code == 16 && binding.direction == AxisDirection.POSITIVE -> Button.DpadDown
                    else -> null // General analog remapping remains with the axis policy.
                }
            } ?: continue
            put(source, get(source).orEmpty() + target)
        }
    }

    fun map(event: InputEvent): List<InputEvent> =
        if (event.kind != InputEvent.Kind.ButtonState) listOf(event)
        else targets[event.button].orEmpty().map { event.copy(button = it) }

    companion object {
        private val KEY_BUTTONS = mapOf(
            96 to Button.South, 97 to Button.East, 99 to Button.West, 100 to Button.North,
            19 to Button.DpadUp, 20 to Button.DpadDown, 21 to Button.DpadLeft, 22 to Button.DpadRight,
            102 to Button.L1, 103 to Button.R1, 104 to Button.L2, 105 to Button.R2,
            106 to Button.L3, 107 to Button.R3, 108 to Button.Start, 109 to Button.Select,
            188 to Button.Guide,
        )
        private val LOGICAL_BUTTONS = mapOf(
            "cross" to Button.South, "circle" to Button.East, "square" to Button.West, "triangle" to Button.North,
            "dpad_up" to Button.DpadUp, "dpad_down" to Button.DpadDown,
            "dpad_left" to Button.DpadLeft, "dpad_right" to Button.DpadRight,
            "l1" to Button.L1, "r1" to Button.R1, "l2" to Button.L2, "r2" to Button.R2,
            "l3" to Button.L3, "r3" to Button.R3, "options" to Button.Start, "share" to Button.Select,
            "ps" to Button.Guide, "touchpad" to Button.TouchpadClick,
        )
    }
}
