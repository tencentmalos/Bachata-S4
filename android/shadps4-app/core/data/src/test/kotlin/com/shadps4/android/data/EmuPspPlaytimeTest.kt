package com.shadps4.android.data

import com.shadps4.android.runtime.session.NativeFexSession
import org.junit.Assert.assertEquals
import org.junit.Test

class EmuPspPlaytimeTest {
    @Test
    fun sessionTerminalsMapToProtocolOutcomes() {
        assertEquals("exit", EmuPspPlaytime.outcomeOf(NativeFexSession.Outcome.RETURNED))
        assertEquals("exit", EmuPspPlaytime.outcomeOf(NativeFexSession.Outcome.CANCELLED))
        assertEquals("crash", EmuPspPlaytime.outcomeOf(NativeFexSession.Outcome.FAULTED))
        assertEquals("crash", EmuPspPlaytime.outcomeOf(NativeFexSession.Outcome.UNEXPECTED))
        for (code in listOf(NativeFexSession.Outcome.BACKEND_FAILED, NativeFexSession.Outcome.UNSUPPORTED,
            NativeFexSession.Outcome.START_FAILED, NativeFexSession.Outcome.TIMEOUT)) {
            assertEquals("error", EmuPspPlaytime.outcomeOf(code))
        }
    }
}
