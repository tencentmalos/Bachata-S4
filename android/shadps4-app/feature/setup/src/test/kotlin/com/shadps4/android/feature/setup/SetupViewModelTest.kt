package com.shadps4.android.feature.setup

import android.content.ContextWrapper
import com.shadps4.android.model.DeviceProfile
import java.io.File
import org.junit.Assert.assertEquals
import org.junit.Assert.assertFalse
import org.junit.Assert.assertTrue
import org.junit.Test
import org.junit.Rule
import org.junit.rules.TemporaryFolder

class SetupViewModelTest {
    @get:Rule val temporaryFolder = TemporaryFolder()

    @Test
    fun anyDeviceReportsDetectedSocAndGpuWithoutBlocking() {
        val viewModel = SetupViewModel()

        viewModel.updateDeviceProfile(DeviceProfile(soc = "SM8150", gpu = "Adreno 640", supported = true))

        val state = viewModel.state.value
        assertEquals("SM8150", state.deviceProfile.soc)
        assertEquals("Adreno 640", state.deviceProfile.gpu)
        assertTrue(state.deviceProfile.supported)
        // The native FEX runtime is built into the APK.
        assertTrue(state.canEnterLibrary)
    }

    @Test
    fun reportsMissingRuntimeBeforeTheContinueGateOpens() {
        val state = SetupUiState(
            deviceProfile = DeviceProfile("SM8150", "Adreno", supported = true),
            runtimeInstalled = false,
            integrityVerified = false,
            legalNotice = "notice",
        )

        assertEquals(SetupReadiness.RuntimeRequired, state.readiness)
        assertFalse(state.canEnterLibrary)
    }

    @Test
    fun continueGateIgnoresSocAndOnlyNeedsRuntime() {
        val state = SetupUiState(
            deviceProfile = DeviceProfile("SM8150", "Adreno 640", supported = true),
            runtimeInstalled = true,
            integrityVerified = true,
            legalNotice = "notice",
        )

        assertEquals(SetupReadiness.Ready, state.readiness)
        assertTrue(state.canEnterLibrary)
    }
}
