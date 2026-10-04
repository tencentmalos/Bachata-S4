package com.shadps4.android.feature.session

import androidx.lifecycle.Lifecycle
import androidx.lifecycle.LifecycleOwner
import androidx.lifecycle.LifecycleRegistry
import kotlinx.coroutines.Dispatchers
import kotlinx.coroutines.ExperimentalCoroutinesApi
import kotlinx.coroutines.launch
import kotlinx.coroutines.test.StandardTestDispatcher
import kotlinx.coroutines.test.advanceTimeBy
import kotlinx.coroutines.test.resetMain
import kotlinx.coroutines.test.runCurrent
import kotlinx.coroutines.test.runTest
import kotlinx.coroutines.test.setMain
import org.junit.After
import org.junit.Assert.assertEquals
import org.junit.Assert.assertTrue
import org.junit.Before
import org.junit.Test

@OptIn(ExperimentalCoroutinesApi::class)
class ForegroundStartTest {
    private class Owner : LifecycleOwner {
        val registry = LifecycleRegistry.createUnsafe(this)
        override val lifecycle: Lifecycle get() = registry
    }

    private val main = StandardTestDispatcher()

    @Before
    fun setUp() = Dispatchers.setMain(main)

    @After
    fun tearDown() = Dispatchers.resetMain()

    @Test
    fun waitsUntilResumed() = runTest(main) {
        val owner = Owner().apply { registry.currentState = Lifecycle.State.CREATED }
        var attempts = 0
        var refusals = -1
        val job = launch { refusals = startWhenResumed(owner.lifecycle) { attempts++; true } }
        runCurrent()
        assertEquals(0, attempts)
        owner.registry.currentState = Lifecycle.State.STARTED
        runCurrent()
        assertEquals("a started but paused screen must not start the session", 0, attempts)
        owner.registry.currentState = Lifecycle.State.RESUMED
        runCurrent()
        assertEquals(1, attempts)
        assertEquals(0, refusals)
        assertTrue(job.isCompleted)
    }

    @Test
    fun refusedStartWaitsForTheNextResume() = runTest(main) {
        val owner = Owner().apply { registry.currentState = Lifecycle.State.RESUMED }
        val results = ArrayDeque(listOf(false, true))
        var attempts = 0
        var refusals = -1
        launch { refusals = startWhenResumed(owner.lifecycle) { attempts++; results.removeFirst() } }
        runCurrent()
        assertEquals(1, attempts)
        owner.registry.currentState = Lifecycle.State.STARTED
        runCurrent()
        owner.registry.currentState = Lifecycle.State.CREATED
        advanceTimeBy(5_000)
        runCurrent()
        assertEquals("no retry while the screen is paused or stopped", 1, attempts)
        owner.registry.currentState = Lifecycle.State.RESUMED
        runCurrent()
        assertEquals(2, attempts)
        assertEquals(1, refusals)
    }

    @Test
    fun refusedStartRetriesWhileTheScreenStaysResumed() = runTest(main) {
        val owner = Owner().apply { registry.currentState = Lifecycle.State.RESUMED }
        val results = ArrayDeque(listOf(false, false, true))
        var attempts = 0
        var refusals = -1
        launch { refusals = startWhenResumed(owner.lifecycle, retryMs = 1_000) { attempts++; results.removeFirst() } }
        runCurrent()
        assertEquals(1, attempts)
        advanceTimeBy(999)
        runCurrent()
        assertEquals(1, attempts)
        advanceTimeBy(1)
        runCurrent()
        assertEquals(2, attempts)
        advanceTimeBy(1_000)
        runCurrent()
        assertEquals(3, attempts)
        assertEquals(2, refusals)
    }

    @Test
    fun destroyedScreenDropsTheStart() = runTest(main) {
        val owner = Owner().apply { registry.currentState = Lifecycle.State.CREATED }
        var attempts = 0
        val job = launch { startWhenResumed(owner.lifecycle) { attempts++; true } }
        runCurrent()
        owner.registry.currentState = Lifecycle.State.DESTROYED
        runCurrent()
        assertTrue(job.isCancelled)
        assertEquals(0, attempts)
    }
}
