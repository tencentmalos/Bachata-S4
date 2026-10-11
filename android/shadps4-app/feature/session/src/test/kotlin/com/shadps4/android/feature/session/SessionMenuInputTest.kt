package com.shadps4.android.feature.session

import com.shadps4.android.runtime.input.NavControllerEvent
import org.junit.Assert.*
import org.junit.Test

class SessionMenuInputTest {
    @Test fun hatSamplesDoNotRepeatUntilReleased() {
        val input = SessionMenuInput()
        assertNull(input.accept(NavControllerEvent("dpad_up", false)))
        assertEquals(SessionMenuInput.Command.Next, input.accept(NavControllerEvent("dpad_down", true)))
        assertNull(input.accept(NavControllerEvent("dpad_down", true)))
        assertNull(input.accept(NavControllerEvent("dpad_down", false)))
        assertEquals(SessionMenuInput.Command.Next, input.accept(NavControllerEvent("dpad_down", true)))
    }

    @Test fun stickUsesSignAndDoesNotRepeatWhileHeld() {
        val input = SessionMenuInput()
        assertEquals(SessionMenuInput.Command.Previous, input.accept(NavControllerEvent("left_y", true, -0.9f)))
        assertNull(input.accept(NavControllerEvent("left_y", true, -0.8f)))
        assertEquals(SessionMenuInput.Command.Next, input.accept(NavControllerEvent("left_y", true, 0.9f)))
        assertNull(input.accept(NavControllerEvent("left_y", false, 0f)))
        assertEquals(SessionMenuInput.Command.Next, input.accept(NavControllerEvent("left_y", true, 0.9f)))
    }

    @Test fun confirmAndCancelArePressedEdges() {
        val input = SessionMenuInput()
        assertEquals(SessionMenuInput.Command.Activate, input.accept(NavControllerEvent("cross", true)))
        assertNull(input.accept(NavControllerEvent("cross", true)))
        assertNull(input.accept(NavControllerEvent("cross", false)))
        assertEquals(SessionMenuInput.Command.Activate, input.accept(NavControllerEvent("cross", true)))
        for (control in listOf("circle", "options", "ps")) {
            assertEquals(SessionMenuInput.Command.Dismiss, input.accept(NavControllerEvent(control, true)))
        }
        assertNull(input.accept(NavControllerEvent("r2", true)))
    }
}
