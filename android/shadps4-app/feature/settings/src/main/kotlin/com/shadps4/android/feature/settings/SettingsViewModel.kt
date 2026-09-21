package com.shadps4.android.feature.settings

import androidx.lifecycle.ViewModel
import androidx.lifecycle.viewModelScope
import com.shadps4.android.data.RuntimeProfileStore
import com.shadps4.android.runtime.settings.ConsoleLanguage
import com.shadps4.android.runtime.settings.ProfileScope
import com.shadps4.android.runtime.settings.RuntimeProfile
import com.shadps4.android.runtime.settings.RuntimeSettingCatalog
import com.shadps4.android.runtime.settings.RuntimeSettingSpec
import dagger.hilt.android.lifecycle.HiltViewModel
import javax.inject.Inject
import kotlinx.coroutines.Job
import kotlinx.coroutines.flow.MutableStateFlow
import kotlinx.coroutines.flow.StateFlow
import kotlinx.coroutines.flow.combine
import kotlinx.coroutines.launch
import kotlinx.serialization.json.JsonElement
import kotlinx.serialization.json.JsonPrimitive
import kotlinx.serialization.json.booleanOrNull
import com.shadps4.android.runtime.settings.SettingKind

data class SettingsUiState(
    val scope: ProfileScope = ProfileScope.Global,
    val profile: RuntimeProfile = RuntimeProfile(),
    val global: RuntimeProfile = RuntimeProfile(),
    val settings: List<RuntimeSettingSpec> = emptyList(),
    val error: String? = null,
) {
    fun effectiveValue(spec: RuntimeSettingSpec): JsonElement? {
        val value = profile.values[spec.id]
            ?: global.values[spec.id].takeIf { scope is ProfileScope.Game }
            ?: spec.defaultValue
        return if (spec.id == ConsoleLanguage.ID) ConsoleLanguage.displayValue(value) else value
    }
}

@HiltViewModel
class SettingsViewModel @Inject constructor(private val store: RuntimeProfileStore) : ViewModel() {
    private val settings = RuntimeSettingCatalog.loadAndroidSettings()
    private val mutableState = MutableStateFlow(SettingsUiState(settings = settings))
    val state: StateFlow<SettingsUiState> = mutableState
    private var profileJob: Job? = null

    init { selectScope(ProfileScope.Global) }

    fun selectScope(scope: ProfileScope) {
        profileJob?.cancel()
        mutableState.value = SettingsUiState(scope = scope, settings = settings)
        profileJob = viewModelScope.launch {
            combine(store.observe(scope), store.observe(ProfileScope.Global)) { profile, global ->
                profile to global
            }.collect { (profile, global) ->
                mutableState.value = mutableState.value.copy(profile = profile, global = global)
            }
        }
    }

    fun setText(spec: RuntimeSettingSpec, text: String) = setValue(spec, JsonPrimitive(text))

    fun setValue(spec: RuntimeSettingSpec, value: JsonElement?) {
        // A stale desktop setting or invalid choice cannot be written through this editor.
        val supported = settings.singleOrNull { it.id == spec.id }
        val choice = value as? JsonPrimitive
        val validValue = value == null || when (supported?.kind) {
            SettingKind.BOOLEAN -> choice?.isString == false && choice.booleanOrNull != null
            SettingKind.ENUM -> choice?.isString == true && choice.content in supported.choices
            else -> false
        }
        if (supported == null || !validValue) {
            mutableState.value = mutableState.value.copy(error = "Unsupported setting or value")
            return
        }
        val scope = mutableState.value.scope
        viewModelScope.launch {
            runCatching {
                store.update(scope) { profile ->
                    val values = profile.values.toMutableMap()
                    if (value == null) values.remove(spec.id) else values[spec.id] = value
                    profile.copy(values = values)
                }
            }.onSuccess { mutableState.value = mutableState.value.copy(error = null) }
                .onFailure { mutableState.value = mutableState.value.copy(error = it.message) }
        }
    }
}
