package com.shadps4.android.runtime.input

import org.junit.Assert.*
import org.junit.Test

class VrGyroAxesTest {
    private fun raw(v: FloatArray, rotation: Int) = VrGyroAxes.toDisplay(v[0], v[1], v[2], (4-rotation)%4)
    private fun calibrate(frame: VrGyroAxes.NeutralFrame, g: FloatArray, rotation: Int) {
        val a = raw(g, rotation)
        repeat(3) { assertFalse(frame.observeGravity(a[0], a[1], a[2], rotation)) }
        assertTrue(frame.observeGravity(a[0], a[1], a[2], rotation))
    }

    @Test fun rotationUsesDisplayBasisRatherThanRotatingTheVectorTwice() {
        // A raw +Y axis points right at ROTATION_90; raw +X points down.
        assertArrayEquals(floatArrayOf(1f, 0f, 0f), VrGyroAxes.toDisplay(0f, 1f, 0f, 1), 0f)
        assertArrayEquals(floatArrayOf(0f, -1f, 0f), VrGyroAxes.toDisplay(1f, 0f, 0f, 1), 0f)
        assertArrayEquals(floatArrayOf(-1f, 0f, 0f), VrGyroAxes.toDisplay(1f, 0f, 0f, 2), 0f)
        assertArrayEquals(floatArrayOf(0f, 1f, 0f), VrGyroAxes.toDisplay(1f, 0f, 0f, 3), 0f)
    }

    @Test fun flatClamshellHorizontalTurnIsYawAndSideTiltIsRollForAllRotations() {
        for (rotation in 0..3) {
            val f=VrGyroAxes.NeutralFrame()
            calibrate(f,floatArrayOf(0f,0f,9.81f),rotation)
            for ((physical, expected) in listOf(
                floatArrayOf(0f,0f,1f) to floatArrayOf(0f,1f,0f), // turn about gravity
                floatArrayOf(0f,-1f,0f) to floatArrayOf(0f,0f,1f), // roll about forward/back
                floatArrayOf(1f,0f,0f) to floatArrayOf(1f,0f,0f), // nod
            )) {
                val a=raw(physical,rotation)
                assertArrayEquals(expected,f.toHead(a[0],a[1],a[2]),1e-5f)
            }
        }
    }

    @Test fun uprightPhoneKeepsScreenAxesAndInclinedDeviceKeepsYawVertical() {
        for (rotation in 0..3) {
            val upright=VrGyroAxes.NeutralFrame()
            calibrate(upright,floatArrayOf(0f,9.81f,0f),rotation)
            val vector=raw(floatArrayOf(1f,2f,3f),rotation)
            assertArrayEquals(floatArrayOf(1f,2f,3f),upright.toHead(vector[0],vector[1],vector[2]),1e-5f)
            val inclined=VrGyroAxes.NeutralFrame()
            calibrate(inclined,floatArrayOf(0f,6.936718f,6.936718f),rotation)
            val turn=raw(floatArrayOf(0f,0.7071068f,0.7071068f),rotation)
            assertArrayEquals(floatArrayOf(0f,1f,0f),inclined.toHead(turn[0],turn[1],turn[2]),1e-5f)
            val roll=raw(floatArrayOf(0f,-0.7071068f,0.7071068f),rotation)
            assertArrayEquals(floatArrayOf(0f,0f,1f),inclined.toHead(roll[0],roll[1],roll[2]),1e-5f)
        }
    }

    @Test fun calibrationWaitsForGravityAndDoesNotEraseSubsequentHeadMotion() {
        val f=VrGyroAxes.NeutralFrame()
        assertNull(f.toHead(1f,2f,3f))
        repeat(5) {
            assertFalse(f.observeGravity(Float.NaN,0f,0f,0))
            assertFalse(f.observeGravity(0f,0f,0f,0))
            assertFalse(f.observeGravity(0f,0f,20f,0))
        }
        calibrate(f,floatArrayOf(0f,0f,9.81f),0)
        assertFalse(f.observeGravity(0f,9.81f,0f,3)) // device moves; basis must stay fixed
        assertArrayEquals(floatArrayOf(0f,1f,0f),f.toHead(0f,0f,1f),1e-5f)
        assertFalse(VrGyroAxes.NeutralFrame().calibrated) // next session recalibrates
    }

    @Test fun unstableOrDegeneratePoseCannotPublishABasis() {
        val f=VrGyroAxes.NeutralFrame()
        repeat(12) { assertFalse(f.observeGravity(if(it%2==0) 9.81f else 0f,0f,if(it%2==0) 0f else 9.81f,0)) }
        assertFalse(f.calibrated)
        val edge=VrGyroAxes.NeutralFrame()
        repeat(8) { assertFalse(edge.observeGravity(9.81f,0f,0f,0)) }
        assertFalse(edge.calibrated)
    }
}
