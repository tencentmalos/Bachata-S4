package com.shadps4.android

import android.os.SystemClock
import android.view.InputDevice
import android.view.KeyEvent
import android.view.MotionEvent
import androidx.activity.compose.setContent
import androidx.compose.ui.test.assertIsFocused
import androidx.compose.ui.test.junit4.createAndroidComposeRule
import androidx.compose.ui.test.onNodeWithText
import androidx.test.platform.app.InstrumentationRegistry
import com.shadps4.android.designsystem.theme.AppTheme
import com.shadps4.android.feature.session.SessionMenu
import org.junit.Assert.assertEquals
import org.junit.Assert.assertTrue
import org.junit.Rule
import org.junit.Test

class SessionMenuInstrumentedTest {
    @get:Rule val compose = createAndroidComposeRule<MainActivity>()

    @Test fun physicalGamepadRouteSupportsKeysHatStickAndCancel() {
        val device = InputDevice.getDeviceIds().asSequence().mapNotNull { InputDevice.getDevice(it) }.first {
            !it.isVirtual && (it.sources and InputDevice.SOURCE_GAMEPAD) == InputDevice.SOURCE_GAMEPAD
        }
        var resumed = 0
        var stopped = 0
        var overlay = 0
        InstrumentationRegistry.getInstrumentation().setInTouchMode(true)
        compose.runOnUiThread {
            compose.activity.setContent {
                AppTheme {
                    SessionMenu(false, false, {}, { overlay++ }, { resumed++ }, { stopped++ })
                }
            }
        }
        compose.onNodeWithText("Resume").assertIsFocused()
        fun key(code: Int) {
            compose.runOnUiThread {
                val now = SystemClock.uptimeMillis()
                for (action in listOf(KeyEvent.ACTION_DOWN, KeyEvent.ACTION_UP)) {
                    assertTrue(compose.activity.dispatchKeyEvent(KeyEvent(
                        now, now, action, code, 0, 0, device.id, 0, 0, InputDevice.SOURCE_GAMEPAD)))
                }
            }
            compose.waitForIdle()
        }
        fun axis(axis: Int, value: Float) {
            compose.runOnUiThread {
                val now = SystemClock.uptimeMillis()
                val properties = MotionEvent.PointerProperties().apply { id = 0 }
                val coords = MotionEvent.PointerCoords().apply { setAxisValue(axis, value) }
                val event = MotionEvent.obtain(now, now, MotionEvent.ACTION_MOVE, 1,
                    arrayOf(properties), arrayOf(coords), 0, 0, 1f, 1f, device.id, 0,
                    InputDevice.SOURCE_JOYSTICK, 0)
                try { assertTrue(compose.activity.dispatchGenericMotionEvent(event)) }
                finally { event.recycle() }
            }
            compose.waitForIdle()
        }
        key(KeyEvent.KEYCODE_DPAD_DOWN)
        compose.onNodeWithText("Stop").assertIsFocused()
        key(KeyEvent.KEYCODE_DPAD_UP)
        compose.onNodeWithText("Resume").assertIsFocused()
        axis(MotionEvent.AXIS_HAT_Y, -1f)
        compose.onNodeWithText("Status overlay controls").assertIsFocused()
        axis(MotionEvent.AXIS_HAT_Y, -1f) // A repeated sample must not move again.
        compose.onNodeWithText("Status overlay controls").assertIsFocused()
        axis(MotionEvent.AXIS_HAT_Y, 0f)
        key(KeyEvent.KEYCODE_BUTTON_A)
        assertEquals(1, overlay)
        axis(MotionEvent.AXIS_Y, 1f)
        compose.onNodeWithText("Resume").assertIsFocused()
        axis(MotionEvent.AXIS_Y, 0f)
        key(KeyEvent.KEYCODE_BUTTON_A)
        key(KeyEvent.KEYCODE_BUTTON_B)
        assertEquals(2, resumed)
        assertEquals(0, stopped)
    }
}
