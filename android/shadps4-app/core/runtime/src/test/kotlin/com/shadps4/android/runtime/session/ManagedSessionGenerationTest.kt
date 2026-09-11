package com.shadps4.android.runtime.session

import java.util.concurrent.atomic.AtomicLong
import org.junit.Assert.assertEquals
import org.junit.Assert.assertTrue
import org.junit.Test

/**
 * The Kotlin half of the "late old-watcher" guarantee (HN0.1 / HN-S01). SessionCore owns generation
 * identity natively; ManagedSession's mirror must drop a stale-generation state update so a late
 * observer from a finished session cannot overwrite the current one's UI state.
 *
 * ManagedSession is a process singleton and beginGeneration is monotonic, so each test claims a
 * fresh, strictly-increasing generation base via [nextBase]; that keeps the tests independent of
 * JUnit's method order without needing to reset the singleton.
 */
class ManagedSessionGenerationTest {
    @Test
    fun currentGenerationUpdateIsPublished() {
        val g = nextBase()
        ManagedSession.beginGeneration(g)
        ManagedSession.updateIfCurrent(g, ManagedSessionState.Running("g", g))
        val s = ManagedSession.state.value
        assertTrue("expected Running, got $s", s is ManagedSessionState.Running)
        assertEquals(g, (s as ManagedSessionState.Running).generation)
    }

    @Test
    fun staleGenerationUpdateIsDropped() {
        val g = nextBase()
        ManagedSession.beginGeneration(g)
        ManagedSession.updateIfCurrent(g, ManagedSessionState.Running("g", g))
        // A late update tagged with an OLDER generation must not clobber the current one.
        ManagedSession.updateIfCurrent(
            g - 1,
            ManagedSessionState.Failed(
                com.shadps4.android.model.RuntimeErrorCode.BACKEND_CRASHED, "late", g - 1,
            ),
        )
        val s = ManagedSession.state.value
        assertTrue("stale update should have been dropped, got $s", s is ManagedSessionState.Running)
        assertEquals(g, (s as ManagedSessionState.Running).generation)
    }

    @Test
    fun beginGenerationIsMonotonic() {
        val g = nextBase()
        ManagedSession.beginGeneration(g)
        // A lower generation must not move the current pointer backward.
        ManagedSession.beginGeneration(g - 5)
        // An update for the higher (still-current) generation is accepted...
        ManagedSession.updateIfCurrent(g, ManagedSessionState.Ready("gHigh", g))
        assertTrue(ManagedSession.state.value is ManagedSessionState.Ready)
        // ...and one for the lower generation is not.
        ManagedSession.updateIfCurrent(g - 5, ManagedSessionState.Running("gLow", g - 5))
        assertTrue(ManagedSession.state.value is ManagedSessionState.Ready)
    }

    private companion object {
        // Strictly-increasing, well above any generation the other tests use; steps of 100 leave
        // room for each test's own g-1 / g-5 offsets.
        val counter = AtomicLong(1_000_000L)
        fun nextBase(): Long = counter.addAndGet(100L)
    }
}
