package com.shadps4.android.runtime.input

import kotlinx.serialization.json.Json
import kotlinx.serialization.encodeToString
import org.junit.Assert.*
import org.junit.Test
import spatial.input.model.Button
import spatial.input.model.InputEvent

class NativeButtonMappingTest {
    @Test fun bothFaceLayoutsPreserveReleases() {
        val source = listOf(Button.South, Button.East, Button.West, Button.North)
        for (flip in listOf(false, true)) {
            val profile = ControllerProfile.standard().copy(swapFaceButtons = flip)
            val mapping = NativeButtonMapping(profile)
            val expected = if (flip) source else listOf(Button.East, Button.South, Button.North, Button.West)
            for ((button, target) in source.zip(expected)) for (pressed in listOf(true, false)) {
                val input = InputEvent(InputEvent.Kind.ButtonState, button = button, pressed = pressed, timestampNs = 123)
                assertEquals(listOf(input.copy(button = target)), mapping.map(input))
            }
        }
    }

    @Test fun absentFlagIsOffAndChoiceSurvivesSerialization() {
        assertFalse(Json.decodeFromString<ControllerProfile>("{}").swapFaceButtons)
        val profile = ControllerProfile.standard().copy(swapFaceButtons = true)
        assertEquals(profile, Json.decodeFromString<ControllerProfile>(Json.encodeToString(profile)))
    }

    @Test fun customButtonAndHatBindingsReachCorrectLogicalButton() {
        val base = ControllerProfile.standardWithHatDpad()
        val profile = base.copy(bindings = (base.bindings - "l1") +
            ("cross" to PhysicalBinding(PhysicalBindingKind.BUTTON, 102)))
        val mapping = NativeButtonMapping(profile)
        fun mapped(button: Button) = mapping.map(InputEvent(InputEvent.Kind.ButtonState, button = button, pressed = true)).map { it.button }
        assertEquals(listOf(Button.South), mapped(Button.L1))
        assertEquals(emptyList<Button>(), mapped(Button.East))
        assertEquals(listOf(Button.DpadUp), mapped(Button.DpadUp))
    }
}
