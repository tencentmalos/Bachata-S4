package com.shadps4.android.feature.settings

import android.content.Context
import android.content.pm.PackageManager
import android.os.Environment
import androidx.lifecycle.ViewModel
import androidx.lifecycle.viewModelScope
import com.shadps4.android.data.ExternalArchiveLibrary
import com.shadps4.android.data.GameRepository
import com.shadps4.android.data.RuntimeProfileStore
import com.shadps4.android.data.ZarLibraryFolder
import com.shadps4.android.runtime.settings.ConsoleLanguage
import com.shadps4.android.runtime.settings.ProfileScope
import com.shadps4.android.runtime.settings.RuntimeProfile
import com.shadps4.android.runtime.settings.RuntimeSettingCatalog
import com.shadps4.android.runtime.settings.RuntimeSettingSpec
import dagger.hilt.android.lifecycle.HiltViewModel
import dagger.hilt.android.qualifiers.ApplicationContext
import java.io.File
import javax.inject.Inject
import kotlinx.coroutines.Dispatchers
import kotlinx.coroutines.Job
import kotlinx.coroutines.flow.MutableStateFlow
import kotlinx.coroutines.flow.StateFlow
import kotlinx.coroutines.flow.combine
import kotlinx.coroutines.launch
import kotlinx.coroutines.withContext
import kotlinx.serialization.json.JsonElement
import kotlinx.serialization.json.JsonPrimitive
import kotlinx.serialization.json.booleanOrNull
import com.shadps4.android.runtime.settings.SettingKind

/** What the app can currently do with the configured ZAR folder. */
enum class ZarFolderAccess {
    /** No folder configured. */
    Unset,

    /** Configured and its entries can be listed. */
    Readable,

    /** Configured but unreadable, and all-files access would fix it. */
    NeedsPermission,

    /** Configured but missing, or unreadable for another reason. */
    Unavailable,
}

data class ZarLibraryUiState(
    val folder: String? = null,
    val appFolder: String? = null,
    val access: ZarFolderAccess = ZarFolderAccess.Unset,
    val allFilesAccessDeclared: Boolean = false,
    val allFilesAccessGranted: Boolean = false,
    val scanning: Boolean = false,
    val linkedTitles: List<String> = emptyList(),
    val removedTitles: List<String> = emptyList(),
    val rejected: List<String> = emptyList(),
    val message: String? = null,
)

data class SettingsUiState(
    val scope: ProfileScope = ProfileScope.Global,
    val profile: RuntimeProfile = RuntimeProfile(),
    val global: RuntimeProfile = RuntimeProfile(),
    val settings: List<RuntimeSettingSpec> = emptyList(),
    val error: String? = null,
    val library: ZarLibraryUiState = ZarLibraryUiState(),
) {
    fun effectiveValue(spec: RuntimeSettingSpec): JsonElement? {
        val value = profile.values[spec.id]
            ?: global.values[spec.id].takeIf { scope is ProfileScope.Game }
            ?: spec.defaultValue
        return if (spec.id == ConsoleLanguage.ID) ConsoleLanguage.displayValue(value) else value
    }
}

@HiltViewModel
class SettingsViewModel @Inject constructor(
    private val store: RuntimeProfileStore,
    private val games: GameRepository,
    @ApplicationContext private val context: Context,
) : ViewModel() {
    private val settings = RuntimeSettingCatalog.loadAndroidSettings()
    private val mutableState = MutableStateFlow(SettingsUiState(settings = settings))
    val state: StateFlow<SettingsUiState> = mutableState
    private var profileJob: Job? = null

    init {
        selectScope(ProfileScope.Global)
        refreshLibraryFolder()
    }

    fun selectScope(scope: ProfileScope) {
        profileJob?.cancel()
        mutableState.value = mutableState.value.copy(scope = scope, profile = RuntimeProfile(), error = null)
        profileJob = viewModelScope.launch {
            combine(store.observe(scope), store.observe(ProfileScope.Global)) { profile, global ->
                profile to global
            }.collect { (profile, global) ->
                mutableState.value = mutableState.value.copy(profile = profile, global = global)
            }
        }
    }

    /** Re-reads the configured folder and its access state without rescanning archives. */
    fun refreshLibraryFolder(message: String? = null) {
        val folder = ZarLibraryFolder.read(context)
        val file = folder?.let(::File)
        val granted = runCatching { Environment.isExternalStorageManager() }.getOrDefault(false)
        val declared = allFilesAccessDeclared()
        val access = when {
            file == null -> ZarFolderAccess.Unset
            ZarLibraryFolder.isReadable(file) -> ZarFolderAccess.Readable
            !granted && declared -> ZarFolderAccess.NeedsPermission
            else -> ZarFolderAccess.Unavailable
        }
        mutableState.value = mutableState.value.copy(
            library = mutableState.value.library.copy(
                folder = folder,
                appFolder = ZarLibraryFolder.appExternalDefault(context)?.absolutePath,
                access = access,
                allFilesAccessDeclared = declared,
                allFilesAccessGranted = granted,
                message = message,
            ),
        )
    }

    /** Stores [path] (null clears it) and links the archives it holds. */
    fun setLibraryFolder(path: String?) {
        val normalized = ZarLibraryFolder.normalize(path)
        if (path != null && normalized == null) {
            refreshLibraryFolder("Pick a folder on device storage")
            return
        }
        ZarLibraryFolder.write(context, normalized)
        refreshLibraryFolder()
        rescanLibrary()
    }

    /** Re-runs the link pass for the configured folder and reports what it produced. */
    fun rescanLibrary() {
        val library = mutableState.value.library
        if (library.scanning) return
        mutableState.value = mutableState.value.copy(library = library.copy(scanning = true, message = null))
        viewModelScope.launch {
            val result = runCatching { withContext(Dispatchers.IO) { games.syncLibrary() } }
            val sync = games.externalArchiveSync
            refreshLibraryFolder(result.exceptionOrNull()?.message)
            mutableState.value = mutableState.value.copy(
                library = mutableState.value.library.copy(
                    scanning = false,
                    linkedTitles = sync.linked.map { it.id },
                    removedTitles = sync.removed,
                    rejected = sync.failures.map { (name, reason) -> "$name — $reason" },
                ),
            )
        }
    }

    private fun allFilesAccessDeclared(): Boolean = runCatching {
        context.packageManager
            .getPackageInfo(context.packageName, PackageManager.GET_PERMISSIONS)
            .requestedPermissions
            ?.contains(android.Manifest.permission.MANAGE_EXTERNAL_STORAGE) == true
    }.getOrDefault(false)

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
