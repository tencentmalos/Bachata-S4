package com.shadps4.android

import android.app.ActivityManager
import android.content.Intent
import android.os.SystemClock
import androidx.lifecycle.Lifecycle
import androidx.test.core.app.ActivityScenario
import androidx.test.platform.app.InstrumentationRegistry
import com.shadps4.android.runtime.input.NativePadBridge
import com.shadps4.android.runtime.session.ManagedSession
import com.shadps4.android.runtime.session.NativeFexSession
import com.shadps4.android.service.FexSessionService
import org.junit.Assert.*
import org.junit.Test

class SessionExitInstrumentedTest {
    private fun await(predicate: () -> Boolean) {
        val end = SystemClock.uptimeMillis() + 10000
        while (!predicate() && SystemClock.uptimeMillis() < end) SystemClock.sleep(10)
        assertTrue(predicate())
    }

    private fun start(scenario: ActivityScenario<MainActivity>): Long {
        scenario.onActivity { activity ->
            activity.startService(Intent(activity, FexSessionService::class.java)
                .setAction(ManagedSession.ACTION_START).putExtra(ManagedSession.EXTRA_GAME_ID, "smoke"))
        }
        await { NativeFexSession.nativeCurrentGeneration() != 0L }
        val generation = NativeFexSession.nativeCurrentGeneration()
        assertEquals(NativeFexSession.WaitPhase.REACHED_TARGET,
            NativeFexSession.nativeWaitPhase(generation, NativeFexSession.PhaseOrdinal.RUNNING, 10000))
        return generation
    }

    private fun assertExited(generation: Long) {
        assertEquals(NativeFexSession.Outcome.CANCELLED,
            NativeFexSession.nativeWaitTerminal(generation, 10000))
        await { NativeFexSession.nativeCurrentGeneration() == 0L && NativePadBridge.currentToken() == 0L }
    }

    @Test fun backgroundAndRecreationPreserveSessionButFinishStopsIt() {
        val scenario = ActivityScenario.launch(MainActivity::class.java)
        try {
            val generation = start(scenario)
            scenario.moveToState(Lifecycle.State.CREATED)
            assertEquals(generation, NativeFexSession.nativeCurrentGeneration())
            scenario.moveToState(Lifecycle.State.RESUMED)
            scenario.recreate()
            assertEquals(generation, NativeFexSession.nativeCurrentGeneration())
            scenario.close()
            assertExited(generation)
        } finally {
            scenario.close()
            InstrumentationRegistry.getInstrumentation().targetContext
                .stopService(Intent(InstrumentationRegistry.getInstrumentation().targetContext, FexSessionService::class.java))
        }
    }

    @Test fun removingTaskStopsSession() {
        val scenario = ActivityScenario.launch(MainActivity::class.java)
        try {
            val generation = start(scenario)
            scenario.onActivity { activity ->
                activity.getSystemService(ActivityManager::class.java).appTasks
                    .first { it.taskInfo.taskId == activity.taskId }.finishAndRemoveTask()
            }
            assertExited(generation)
        } finally { scenario.close() }
    }
}
