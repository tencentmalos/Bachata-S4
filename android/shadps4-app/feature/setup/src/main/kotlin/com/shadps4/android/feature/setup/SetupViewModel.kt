package com.shadps4.android.feature.setup

import androidx.lifecycle.ViewModel
import com.shadps4.android.model.DeviceProfile
import dagger.hilt.android.lifecycle.HiltViewModel
import javax.inject.Inject
import kotlinx.coroutines.flow.MutableStateFlow
import kotlinx.coroutines.flow.StateFlow

data class SetupUiState(
    val deviceProfile: DeviceProfile,
    val runtimeInstalled: Boolean,
    val integrityVerified: Boolean,
    val legalNotice: String,
) {
    val canEnterLibrary: Boolean
        get() = runtimeInstalled && integrityVerified

    val readiness: SetupReadiness
        get() = when {
            !runtimeInstalled -> SetupReadiness.RuntimeRequired
            !integrityVerified -> SetupReadiness.IntegrityRequired
            else -> SetupReadiness.Ready
        }
}

enum class SetupReadiness {
    Ready,
    RuntimeRequired,
    IntegrityRequired,
}

@HiltViewModel
class SetupViewModel @Inject constructor() : ViewModel() {
    private val mutableState = MutableStateFlow(
        SetupUiState(
            deviceProfile = DeviceProfile(soc = "unknown", gpu = "unverified", supported = true),
            // The FEX CPU backend is compiled into the APK (libshadps4_fex_session.so); there is no
            // external glibc runtime to install/extract as in the reference, so the app is always
            // "runtime ready". (Reference behaviour: extract assets/runtime/runtime.zip -> box64-*.)
            runtimeInstalled = true,
            integrityVerified = true,
            legalNotice = "Import only games and firmware content you legally own.",
        ),
    )

    val state: StateFlow<SetupUiState> = mutableState

    init {
        val soc = android.os.Build.SOC_MODEL.orEmpty().ifEmpty { "unknown" }
        // No SoC allowlist: report detected hardware but do not block setup.
        val profile = DeviceProfile(
            soc = soc,
            gpu = "unverified",
            supported = true,
        )
        updateDeviceProfile(profile)
    }

    fun checkRuntimeStatus() {
        // Built-in FEX backend is always present.
        mutableState.value = mutableState.value.copy(
            runtimeInstalled = true,
            integrityVerified = true,
        )
    }

    fun updateDeviceProfile(profile: DeviceProfile) {
        mutableState.value = mutableState.value.copy(deviceProfile = profile)
    }

}
