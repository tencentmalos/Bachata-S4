package com.shadps4.android.feature.settings.input

import androidx.lifecycle.ViewModel
import androidx.lifecycle.viewModelScope
import com.shadps4.android.data.RuntimeProfileStore
import com.shadps4.android.runtime.input.ControllerDeviceKey
import com.shadps4.android.runtime.input.NativeButtonMapping
import com.shadps4.android.runtime.input.ControllerProfile
import com.shadps4.android.runtime.input.GamepadInputManager
import com.shadps4.android.runtime.input.PhysicalBinding
import com.shadps4.android.runtime.settings.ProfileScope
import dagger.hilt.android.lifecycle.HiltViewModel
import javax.inject.Inject
import kotlinx.coroutines.flow.MutableStateFlow
import kotlinx.coroutines.flow.StateFlow
import kotlinx.coroutines.launch

data class BindingConflict(val target: String, val existing: String, val binding: PhysicalBinding)
data class ControllerMappingUiState(
    val scope: ProfileScope = ProfileScope.Global,
    val profiles: List<ControllerProfile> = List(4) { ControllerProfile() },
    val slot: Int = 0,
    val captureQueue: List<String> = emptyList(),
    val conflict: BindingConflict? = null,
    val error: String? = null,
)

@HiltViewModel
class ControllerMappingViewModel @Inject constructor(private val store: RuntimeProfileStore) : ViewModel() {
    private val mutableState = MutableStateFlow(ControllerMappingUiState())
    val state: StateFlow<ControllerMappingUiState> = mutableState

    fun load(scope: ProfileScope) {
        viewModelScope.launch {
            val local = store.load(scope).controllerSlots
            val stored = if (scope is ProfileScope.Game && local.isEmpty())
                store.load(ProfileScope.Global).controllerSlots else local
            mutableState.value = mutableState.value.copy(scope = scope, profiles = List(4) { stored.getOrNull(it) ?: ControllerProfile.standard() })
        }
    }

    fun selectSlot(slot: Int) { require(slot in 0..3); mutableState.value = mutableState.value.copy(slot = slot, conflict = null) }
    fun capture(control: String) {
        require(control in NativeButtonMapping.supportedControls)
        mutableState.value = mutableState.value.copy(captureQueue = listOf(control))
        startCapture()
    }
    fun captureSequential() {
        mutableState.value = mutableState.value.copy(captureQueue = NativeButtonMapping.supportedControls.toList().sorted())
        startCapture()
    }

    override fun onCleared() {
        stopCapture()
        super.onCleared()
    }

    fun accept(binding: PhysicalBinding) {
        val target = mutableState.value.captureQueue.firstOrNull() ?: return
        if (!NativeButtonMapping.supports(binding)) {
            mutableState.value = mutableState.value.copy(error = "Use a controller button or D-pad direction")
            return
        }
        mutableState.value = mutableState.value.copy(error = null)
        val profile = current()
        val existing = ControllerProfile.LOGICAL_CONTROLS.firstOrNull { it != target && profile.bindingFor(it) == binding }
        if (existing != null) {
            mutableState.value = mutableState.value.copy(conflict = BindingConflict(target, existing, binding))
        } else applyBinding(target, binding, null)
    }

    fun replaceConflict() {
        val conflict = mutableState.value.conflict ?: return
        applyBinding(conflict.target, conflict.binding, conflict.existing)
    }

    fun cancelCapture() {
        mutableState.value = mutableState.value.copy(captureQueue = emptyList(), conflict = null)
        stopCapture()
    }

    fun cancelConflict() { mutableState.value = mutableState.value.copy(conflict = null) }
    fun autoMap(device: ControllerDeviceKey? = current().device, useHatDpad: Boolean = false) {
        val std = if (useHatDpad) ControllerProfile.standardWithHatDpad(device) else ControllerProfile.standard(device)
        replaceCurrent(std.copy(swapFaceButtons = current().swapFaceButtons)); save()
    }
    fun clear() { replaceCurrent(ControllerProfile(device = current().device)); save() }
    fun setDevice(device: ControllerDeviceKey) { replaceCurrent(current().copy(device = device)); save() }
    fun setSwapFaceButtons(enabled: Boolean) { replaceCurrent(current().copy(swapFaceButtons = enabled)); save() }

    fun inherit() {
        val scope = mutableState.value.scope
        viewModelScope.launch {
            store.update(scope) { it.copy(controllerSlots = emptyList()) }
            load(scope)
        }
    }

    private fun applyBinding(target: String, binding: PhysicalBinding, remove: String?) {
        val profile = current()
        val bindings = profile.bindings.toMutableMap().apply {
            if (remove != null) remove(profile.bindingKey(remove))
            put(profile.bindingKey(target), binding)
        }
        replaceCurrent(current().copy(bindings = bindings))
        val next = mutableState.value.captureQueue.drop(1)
        mutableState.value = mutableState.value.copy(captureQueue = next, conflict = null)
        if (next.isEmpty()) stopCapture()
        save()
    }

    private fun startCapture() {
        GamepadInputManager.registerCaptureListener(::accept)
    }
    private fun stopCapture() {
        GamepadInputManager.unregisterCaptureListener()
    }

    private fun current() = mutableState.value.profiles[mutableState.value.slot]
    private fun replaceCurrent(profile: ControllerProfile) {
        val profiles = mutableState.value.profiles.toMutableList().apply { set(mutableState.value.slot, profile) }
        mutableState.value = mutableState.value.copy(profiles = profiles)
    }
    private fun save() {
        viewModelScope.launch {
            runCatching { store.update(mutableState.value.scope) { it.copy(controllerSlots = mutableState.value.profiles) } }
                .onFailure { mutableState.value = mutableState.value.copy(error = it.message) }
        }
    }
}
