package com.shadps4.android.feature.settings

import androidx.lifecycle.ViewModel
import androidx.lifecycle.viewModelScope
import com.shadps4.android.data.RuntimeProfileStore
import com.shadps4.android.runtime.settings.ProfileScope
import com.shadps4.android.runtime.settings.RuntimeProfile
import com.shadps4.android.runtime.settings.RuntimeSettingCatalog
import com.shadps4.android.runtime.settings.ShadPs4JsonCodec
import dagger.hilt.android.lifecycle.HiltViewModel
import javax.inject.Inject
import kotlinx.coroutines.flow.MutableStateFlow
import kotlinx.coroutines.flow.StateFlow
import kotlinx.coroutines.launch

data class RawConfigUiState(
    val scope: ProfileScope = ProfileScope.Global,
    val shadPs4Json: String = "{}",
    val validation: String? = null,
    val valid: Boolean = false,
)

@HiltViewModel
class RawConfigViewModel @Inject constructor(private val store: RuntimeProfileStore) : ViewModel() {
    private val catalog = RuntimeSettingCatalog.loadFromResources()
    private val mutableState = MutableStateFlow(RawConfigUiState())
    val state: StateFlow<RawConfigUiState> = mutableState

    fun load(scope: ProfileScope) {
        viewModelScope.launch {
            val profile = store.load(scope)
            mutableState.value = RawConfigUiState(
                scope = scope,
                shadPs4Json = renderShad(profile),
            )
        }
    }

    fun editShadPs4(text: String) { mutableState.value = mutableState.value.copy(shadPs4Json = text, valid = false, validation = null) }

    fun validate(): Boolean = runCatching { applyDraft(RuntimeProfile()) }
        .fold(
            onSuccess = { mutableState.value = mutableState.value.copy(valid = true, validation = "Valid"); true },
            onFailure = { mutableState.value = mutableState.value.copy(valid = false, validation = it.message); false },
        )

    fun save() {
        if (!validate()) return
        viewModelScope.launch {
            runCatching { store.update(mutableState.value.scope, ::applyDraft) }
                .onSuccess { mutableState.value = mutableState.value.copy(validation = "Saved") }
                .onFailure { mutableState.value = mutableState.value.copy(valid = false, validation = it.message) }
        }
    }

    private fun applyDraft(profile: RuntimeProfile): RuntimeProfile =
        ShadPs4JsonCodec.applyRawJson(profile, mutableState.value.shadPs4Json, catalog.shadPs4)

    private fun renderShad(profile: RuntimeProfile): String {
        var document = ShadPs4JsonCodec.empty().mergeUnknown(profile.unknownShadPs4)
        catalog.shadPs4.forEach { spec ->
            profile.values[spec.id]?.let { document = document.set(spec.section, spec.nativeKey.substringAfter('.'), it) }
        }
        return document.render()
    }
}
