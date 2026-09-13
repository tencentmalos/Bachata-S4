package com.shadps4.android.runtime.input

import android.view.InputDevice
import android.view.KeyEvent
import androidx.test.ext.junit.runners.AndroidJUnit4
import androidx.test.platform.app.InstrumentationRegistry
import com.shadps4.android.runtime.session.NativeFexSession
import java.io.File
import kotlin.test.*
import org.junit.Before
import org.junit.Test
import org.junit.Assume.assumeTrue
import org.junit.runner.RunWith

/** Ordinary APK UID, JNI -> linked host -> real scePad* exports. Synthetic events
 * are labelled explicitly; no game execution or physical button press claimed.
 */
@RunWith(AndroidJUnit4::class)
class NativePadInstrumentedTest {
    private val instrumentation get() = InstrumentationRegistry.getInstrumentation()
    private val context get() = instrumentation.targetContext
    @Before fun load() {
        NativeFexSession.nativeIdentity()
        assertTrue(NativePad.nativeInitializeHost(File(context.filesDir,"host").absolutePath))
    }
    private fun main(action: () -> Unit) = instrumentation.runOnMainSync(action)
    private fun axes() = floatArrayOf(0f,-32768f,32767f,0f,5f,0f,255f,0f)
    private fun packet(t: Long,port: Int,id: Long,e: Long,seq: Long,button: Int,pressed: Boolean) =
        NativePad.nativePacket(t,port,id,e,seq,0,intArrayOf(0,button),floatArrayOf(if(pressed)1f else 0f))

    @Test fun overlayToProductionHleAndRetiredHandle() {
        val t = NativePad.nativeBeginSession()
        try {
            NativePad.nativeSetConnected(t,0,true)
            val h=NativePad.nativeOpenDefaultPad(); assertTrue(h>0,"actual scePadOpen")
            assertEquals(0,NativePad.submit(t,0,ControllerSnapshot.normalized(buttons=Ps4Button.CROSS,leftX=-1f,leftY=1f)))
            val data=assertNotNull(NativePad.nativeReadPad(h))
            assertEquals(0L,data[0]); assertEquals(Ps4Button.CROSS,data[1])
            assertEquals(0L,data[2]); assertEquals(255L,data[3]); assertEquals(128L,data[4])
            val next=NativePad.nativeBeginSession()
            assertEquals(NativePad.Result.WRONG_SESSION,NativePad.submit(t,0,ControllerSnapshot.Neutral))
            assertTrue(assertNotNull(NativePad.nativeReadPad(h))[0]<0,"old handle rejected")
            NativePad.nativeEndSession(t); assertEquals(next,NativePad.nativeCurrentToken())
            NativePad.nativeEndSession(next)
        } finally { NativePad.nativeEndSession(t) }
    }
    @Test fun physicalPacketNormalizationIsolationAndReconnect() {
        val t=NativePad.nativeBeginSession()
        try {
            val a=NativePad.nativeRegisterDevice(t,0,700,axes(),true)
            val b=NativePad.nativeRegisterDevice(t,1,701,axes(),true)
            assertTrue(a>0 && b>0)
            assertEquals(0,packet(t,0,700,a,1,1,true))
            assertEquals(0,packet(t,1,701,b,1,0,true))
            val h=NativePad.nativeOpenDefaultPad(); assertTrue(h>0)
            assertEquals(Ps4Button.CIRCLE,assertNotNull(NativePad.nativeReadPad(h))[1])
            assertEquals(Ps4Button.CROSS,NativePad.nativeReadButtons(1))
            assertEquals(0,NativePad.nativePacket(t,0,700,a,2,0,intArrayOf(1,0,1,5),floatArrayOf(-32768f,255f)))
            val state=assertNotNull(NativePad.nativeReadPad(h)); assertEquals(0L,state[2]); assertEquals(255L,state[7])
            assertEquals(3,packet(t,0,700,a,1,1,false),"out of order refused")
            NativePad.nativeRemoveDevice(t,0,a)
            assertEquals(0L,assertNotNull(NativePad.nativeReadPad(h))[8])
            val anew=NativePad.nativeRegisterDevice(t,0,700,axes(),true); assertTrue(anew!=a)
            assertEquals(3,packet(t,0,700,a,3,1,true))
            NativePad.nativeRemoveDevice(t,0,a); assertTrue(NativePad.nativeConnected(0))
        } finally { NativePad.nativeEndSession(t) }
    }
    @Test fun productionVibrationTargetsEpochAndStaleDrainCannotSteal() {
        val t=NativePad.nativeBeginSession()
        try {
            val a=NativePad.nativeRegisterDevice(t,0,710,axes(),true); assertTrue(a>0)
            val h=NativePad.nativeOpenDefaultPad(); assertTrue(h>0)
            assertEquals(0,NativePad.nativeVibratePad(h,180,90))
            assertEquals(0,assertNotNull(NativePad.nativeDrainHaptics(t+1)).size)
            val cmd=assertNotNull(NativePad.nativeDrainHaptics(t))
            assertContentEquals(longArrayOf(710,a,180,90,1000,0),cmd)
            assertEquals(0,NativePad.nativeVibratePad(h,0,0))
            assertEquals(1L,assertNotNull(NativePad.nativeDrainHaptics(t))[5])
            NativePad.nativeVibratePad(h,100,100)
            NativePad.nativeRemoveDevice(t,0,a)
            assertEquals(0,assertNotNull(NativePad.nativeDrainHaptics(t)).size)
        } finally { NativePad.nativeEndSession(t) }
    }
    @Test fun foundationAndroidSourceToProductionPadWithSyntheticKey() {
        // Enumerated real controller capabilities, injected KeyEvent (no claim of
        // an actual physical press). Missing hardware is an explicit assumption.
        val device=InputDevice.getDeviceIds().toList().mapNotNull { InputDevice.getDevice(it) }.firstOrNull {
            !it.isVirtual && !it.name.startsWith("uinput-") &&
            ((it.sources and InputDevice.SOURCE_GAMEPAD)==InputDevice.SOURCE_GAMEPAD ||
             (it.sources and InputDevice.SOURCE_JOYSTICK)==InputDevice.SOURCE_JOYSTICK)
        }
        assumeTrue("requires enumerated Android controller",device!=null)
        val generation=7101L
        main { NativePadBridge.begin(context,generation); NativePadBridge.setFocused(true) }
        try {
            val h=NativePad.nativeOpenDefaultPad(); assertTrue(h>0)
            val now=android.os.SystemClock.uptimeMillis()
            val down=KeyEvent(now,now,KeyEvent.ACTION_DOWN,KeyEvent.KEYCODE_BUTTON_A,0,0,device!!.id,0,0,InputDevice.SOURCE_GAMEPAD)
            main { assertTrue(NativePadBridge.dispatchKeyEvent(down)) }
            assertTrue(assertNotNull(NativePad.nativeReadPad(h))[1] and Ps4Button.CROSS != 0L)
            main { NativePadBridge.setFocused(false) }
            assertEquals(0L,assertNotNull(NativePad.nativeReadPad(h))[1])
            main { NativePadBridge.setFocused(true) }
            assertEquals(0L,assertNotNull(NativePad.nativeReadPad(h))[1])
        } finally { main { NativePadBridge.end(generation) } }
    }
    @Test fun oldObserverCannotEndNewBridge() {
        main {
            NativePadBridge.begin(context,8101)
            val next=NativePadBridge.begin(context,8102)
            NativePadBridge.end(8101)
            assertEquals(next,NativePadBridge.currentToken())
            NativePadBridge.end(8102)
            assertEquals(0L,NativePad.nativeCurrentToken())
        }
    }
    @Test fun stopCannotResumeOldInputOrMuteNextSession() {
        main {
            NativePadBridge.setFocused(true)
            NativePadBridge.begin(context,8201)
            assertTrue(NativePad.nativeConnected(0))
            NativePadBridge.requestStop(8201)
            assertFalse(NativePad.nativeConnected(0))
            NativePadBridge.setFocused(true)
            assertFalse(NativePad.nativeConnected(0),"focus cannot revive a stopping session")
            NativePadBridge.end(8201)
            NativePadBridge.begin(context,8202)
            assertTrue(NativePad.nativeConnected(0),"Stop must not change the Activity focus state")
            NativePadBridge.requestStop(8201)
            assertTrue(NativePad.nativeConnected(0),"old Stop cannot mute a new session")
            NativePadBridge.end(8202)
        }
    }
}
