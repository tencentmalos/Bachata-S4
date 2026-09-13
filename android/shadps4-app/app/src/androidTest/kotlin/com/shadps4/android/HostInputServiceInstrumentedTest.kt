package com.shadps4.android

import android.content.Intent
import android.os.SystemClock
import androidx.test.ext.junit.runners.AndroidJUnit4
import androidx.test.platform.app.InstrumentationRegistry
import com.shadps4.android.runtime.input.NativePad
import com.shadps4.android.runtime.input.NativePadBridge
import com.shadps4.android.runtime.input.ControllerSnapshot
import com.shadps4.android.runtime.input.Ps4Button
import com.shadps4.android.runtime.session.ManagedSession
import com.shadps4.android.runtime.session.NativeFexSession
import com.shadps4.android.service.FexSessionService
import org.junit.Assert.*
import org.junit.Test
import org.junit.runner.RunWith

/** Actual application/Service/ART namespace, same-process restarts. The production
 * backend is still CPU smoke: this test does not claim PKG/rendering acceptance.
 */
@RunWith(AndroidJUnit4::class)
class HostInputServiceInstrumentedTest {
    private fun await(message: String, predicate: () -> Boolean) {
        val deadline=SystemClock.uptimeMillis()+5000
        while (!predicate() && SystemClock.uptimeMillis()<deadline) SystemClock.sleep(5)
        assertTrue(message,predicate())
    }
    @Test fun hostLoadsAndInputFollowsThreeServiceGenerations() {
        val instrumentation=InstrumentationRegistry.getInstrumentation()
        val context=instrumentation.targetContext
        val activity=instrumentation.startActivitySync(Intent(context,MainActivity::class.java).addFlags(Intent.FLAG_ACTIVITY_NEW_TASK))
        val identity=NativeFexSession.nativeIdentity()
        var previous=0L
        try {
            repeat(3) { round ->
                context.startService(Intent(context,FexSessionService::class.java).setAction(ManagedSession.ACTION_START)
                    .putExtra(ManagedSession.EXTRA_GAME_ID,"host-input-cpu-smoke"))
                await("session entered") { NativeFexSession.nativeCurrentGeneration()>previous }
                val generation=NativeFexSession.nativeCurrentGeneration()
                assertTrue(generation>previous)
                await("input bound") { NativePadBridge.currentToken()!=0L }
                val token=NativePadBridge.currentToken()
                assertEquals(token,NativePad.nativeCurrentToken())
                val ready=NativeFexSession.nativeWaitPhase(generation,NativeFexSession.PhaseOrdinal.RUNNING,3000)
                assertEquals("real CPU running",NativeFexSession.WaitPhase.REACHED_TARGET,ready)
                val handle=NativePad.nativeOpenDefaultPad()
                assertTrue("production pad open",handle>0)
                val publish=ManagedSession.controllerPublisher(generation)
                publish(ControllerSnapshot.normalized(buttons=Ps4Button.CROSS))
                await("input works after each restart") {
                    (NativePad.nativeReadPad(handle)?.get(1)?.and(Ps4Button.CROSS) ?: 0L) != 0L
                }
                publish(ControllerSnapshot.Neutral)
                await("button released") { NativePad.nativeReadPad(handle)?.get(1) == 0L }
                context.startService(Intent(context,FexSessionService::class.java).setAction(ManagedSession.ACTION_STOP))
                val terminal=NativeFexSession.nativeWaitTerminal(generation,3000)
                assertTrue("actual terminal=$terminal",terminal==NativeFexSession.Outcome.CANCELLED || terminal==NativeFexSession.Outcome.RETURNED)
                await("input retired") { NativePadBridge.currentToken()==0L && NativePad.nativeCurrentToken()==0L }
                assertEquals("same process",identity,NativeFexSession.nativeIdentity())
                android.util.Log.i("HostInputAcceptance","round=${round+1} generation=$generation input=$token pad=press_release_pass terminal=$terminal $identity")
                previous=generation
            }
        } finally {
            context.startService(Intent(context,FexSessionService::class.java).setAction(ManagedSession.ACTION_STOP))
            instrumentation.runOnMainSync { activity.finish() }
        }
    }
}
