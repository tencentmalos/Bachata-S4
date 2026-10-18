package com.shadps4.android.runtime.input

import org.junit.Assert.assertEquals
import org.junit.Assert.assertTrue
import org.junit.Test

class ControllerBindingResolverTest {
    private val key = ControllerDeviceKey("stable", 1, 2, "Pad")
    private val resolver = ControllerBindingResolver()

    @Test
    fun stableIdentityIgnoresTransientAndroidDeviceId() {
        val profile = ControllerProfile.standard(key)
        assertEquals(mapOf(0 to 99), resolver.assignSlots(listOf(profile), listOf(ConnectedController(99, key))))
    }

    @Test
    fun appliesDeadZoneInversionTriggersAndDisconnectNeutralization() {
        val axis = PhysicalBinding(PhysicalBindingKind.AXIS, 0)
        val trigger = PhysicalBinding(PhysicalBindingKind.AXIS, 17)
        val profile = ControllerProfile(
            device = key,
            bindings = mapOf("left_x" to axis, "left_trigger" to trigger),
            deadZone = 0.1f,
            invertAxes = setOf("left_x"),
            triggerThreshold = 0.4f,
        )
        val snapshot = resolver.snapshot(profile, mapOf(axis to 0.5f, trigger to 0.6f))
        assertEquals(-0.5f, snapshot.leftX)
        assertEquals(0.6f, snapshot.leftTrigger)
        assertTrue(snapshot.buttons and Ps4Button.L2 != 0L)
        assertEquals(ControllerSnapshot.Neutral, resolver.snapshotOrNeutral(profile, null, emptyMap()))
    }

    // Odin 3: the built-in pad came back with a new descriptor after it was enumerated again.
    private val odinSaved = ControllerDeviceKey("8e1073ea", 8224, 273, "Odin Controller")
    private val odinNow = ControllerDeviceKey("45336d6f", 8224, 273, "Odin Controller")
    private val usbPad = ControllerDeviceKey("usb", 0x045e, 0x0b12, "Xbox Wireless Controller")
    private fun slots(vararg devices: ControllerDeviceKey?) =
        List(4) { ControllerProfile.standard(devices.getOrNull(it)) }

    @Test
    fun reEnumeratedPadKeepsItsConfiguredPort() {
        assertEquals(0, resolver.portFor(slots(odinSaved), emptySet(), odinNow, emptyList()))
    }

    @Test
    fun configuredPortWinsOverALowerUnconfiguredPort() {
        assertEquals(1, resolver.portFor(slots(null, odinSaved), emptySet(), odinSaved, emptyList()))
    }

    @Test
    fun otherModelDoesNotTakeAConfiguredPort() {
        assertEquals(1, resolver.portFor(slots(odinSaved), emptySet(), usbPad, emptyList()))
    }

    @Test
    fun sameModelSkipsAPortWhosePadIsConnected() {
        val first = ControllerDeviceKey("first", 0x045e, 0x0b12, "Xbox Wireless Controller")
        val second = ControllerDeviceKey("second", 0x045e, 0x0b12, "Xbox Wireless Controller")
        val third = ControllerDeviceKey("third", 0x045e, 0x0b12, "Xbox Wireless Controller")
        val profiles = slots(first, second)
        assertEquals(1, resolver.portFor(profiles, emptySet(), third, listOf(first)))
        assertEquals(2, resolver.portFor(profiles, emptySet(), third, listOf(first, second)))
    }

    @Test
    fun occupiedPortsAreSkipped() {
        assertEquals(1, resolver.portFor(slots(odinSaved), setOf(0), odinNow, emptyList()))
        assertEquals(null, resolver.portFor(slots(odinSaved, odinSaved, odinSaved, odinSaved), setOf(0, 1, 2, 3), odinNow, emptyList()))
    }

    @Test
    fun unknownVendorAndProductNeedTheDescriptor() {
        val saved = ControllerDeviceKey("old", 0, 0, "Gamepad")
        val now = ControllerDeviceKey("new", 0, 0, "Gamepad")
        assertEquals(1, resolver.portFor(slots(saved), emptySet(), now, emptyList()))
    }

    @Test
    fun legacySnapshotAcceptsAReEnumeratedPad() {
        val button = PhysicalBinding(PhysicalBindingKind.BUTTON, 96)
        val profile = ControllerProfile(device = odinSaved, bindings = mapOf("cross" to button))
        val snapshot = resolver.snapshotOrNeutral(profile, odinNow, mapOf(button to 1f))
        assertTrue(snapshot.buttons and Ps4Button.CROSS != 0L)
        assertEquals(ControllerSnapshot.Neutral, resolver.snapshotOrNeutral(profile, usbPad, mapOf(button to 1f)))
    }

    @Test
    fun limitsProfilesToFourSlots() {
        org.junit.Assert.assertThrows(IllegalArgumentException::class.java) {
            resolver.assignSlots(List(5) { ControllerProfile() }, emptyList())
        }
    }

    @Test
    fun hatNegativeDirectionMapsToDpadUp() {
        val hatY = PhysicalBinding(PhysicalBindingKind.AXIS, 16, AxisDirection.NEGATIVE)
        val profile = ControllerProfile(device = null, bindings = mapOf("dpad_up" to hatY))
        val snapshot = resolver.snapshot(profile, mapOf(hatY to -1f))
        assertTrue(snapshot.buttons and Ps4Button.UP != 0L)
    }

    @Test
    fun hatPositiveDirectionMapsToDpadDown() {
        val hatY = PhysicalBinding(PhysicalBindingKind.AXIS, 16, AxisDirection.POSITIVE)
        val profile = ControllerProfile(device = null, bindings = mapOf("dpad_down" to hatY))
        val snapshot = resolver.snapshot(profile, mapOf(hatY to 1f))
        assertTrue(snapshot.buttons and Ps4Button.DOWN != 0L)
    }

    @Test
    fun hatDirectionDoesNotFireOnWrongSign() {
        val hatYNeg = PhysicalBinding(PhysicalBindingKind.AXIS, 16, AxisDirection.NEGATIVE)
        val profile = ControllerProfile(device = null, bindings = mapOf("dpad_up" to hatYNeg))
        // Positive value on a NEGATIVE-direction binding should NOT activate the button.
        val snapshot = resolver.snapshot(profile, mapOf(hatYNeg to 1f))
        assertTrue(snapshot.buttons and Ps4Button.UP == 0L)
    }

    @Test
    fun bothDirectionUsesPositiveThresholdOnly() {
        val axis = PhysicalBinding(PhysicalBindingKind.AXIS, 0, AxisDirection.BOTH)
        val profile = ControllerProfile(device = null, bindings = mapOf("cross" to axis))
        // Positive fires
        assertTrue(resolver.snapshot(profile, mapOf(axis to 0.8f)).buttons and Ps4Button.CROSS != 0L)
        // Negative does not (BOTH = legacy behavior, only checks >= threshold)
        assertTrue(resolver.snapshot(profile, mapOf(axis to -0.8f)).buttons and Ps4Button.CROSS == 0L)
    }
}
