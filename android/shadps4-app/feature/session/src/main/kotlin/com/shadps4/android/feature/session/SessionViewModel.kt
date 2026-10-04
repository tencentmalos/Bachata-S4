package com.shadps4.android.feature.session

import android.app.BackgroundServiceStartNotAllowedException
import android.content.Context
import android.content.Intent
import android.app.ActivityManager
import android.util.Log
import androidx.lifecycle.Lifecycle
import androidx.lifecycle.SavedStateHandle
import androidx.lifecycle.ViewModel
import androidx.lifecycle.viewModelScope
import com.shadps4.android.data.GameRepository
import com.shadps4.android.model.RuntimeErrorCode
import com.shadps4.android.runtime.session.ManagedSession
import com.shadps4.android.runtime.session.ManagedSessionState
import com.shadps4.android.runtime.driver.RuntimeVulkanDriver
import com.shadps4.android.runtime.driver.RuntimeVulkanDriverPreference
import dagger.hilt.android.lifecycle.HiltViewModel
import dagger.hilt.android.qualifiers.ApplicationContext
import javax.inject.Inject
import kotlinx.coroutines.Job
import kotlinx.coroutines.flow.StateFlow
import kotlinx.coroutines.launch
import kotlinx.coroutines.delay
import kotlinx.coroutines.flow.MutableStateFlow

data class DeviceTelemetry(val ramUsedMb: Long = 0, val ramTotalMb: Long = 0, val gpuLoad: String = "N/A")

@HiltViewModel
class SessionViewModel @Inject constructor(
    private val repository: GameRepository,
    @ApplicationContext private val context: Context,
    private val savedState: SavedStateHandle,
) : ViewModel() {
    val state: StateFlow<ManagedSessionState> = ManagedSession.state
    val frameTelemetry = ManagedSession.frameTelemetry
    private val mutableDeviceTelemetry = MutableStateFlow(DeviceTelemetry())
    val deviceTelemetry: StateFlow<DeviceTelemetry> = mutableDeviceTelemetry
    private var pendingStart: Job? = null

    init {
        viewModelScope.launch {
            val activityManager = context.getSystemService(ActivityManager::class.java)
            while (true) {
                ManagedSession.refreshFrameTelemetry()
                val memory = ActivityManager.MemoryInfo().also { activityManager.getMemoryInfo(it) }
                val gpu = runCatching {
                    java.io.File("/sys/class/kgsl/kgsl-3d0/gpu_busy_percentage").readText().trim()
                }.getOrNull()?.takeIf(String::isNotBlank) ?: "N/A"
                mutableDeviceTelemetry.value = DeviceTelemetry(
                    ramUsedMb = (memory.totalMem - memory.availMem) / (1024 * 1024),
                    ramTotalMb = memory.totalMem / (1024 * 1024),
                    gpuLoad = gpu,
                )
                delay(500)
            }
        }
    }

    /**
     * Starts [gameId] when its screen opens, once [lifecycle] (the hosting activity's) is resumed.
     * Returns false, without starting anything, if this screen already started the game once: a
     * screen restored after the process died, or recreated after its session ended, must not
     * start the game again by itself.
     */
    fun launchOnEnter(gameId: String, lifecycle: Lifecycle): Boolean {
        if (sessionActive()) return true
        if (savedState.get<Boolean>(KEY_STARTED) == true) return false
        start(gameId, lifecycle)
        return true
    }

    /** Starts [gameId] again on an explicit user request. */
    fun relaunch(gameId: String, lifecycle: Lifecycle) {
        if (!sessionActive()) start(gameId, lifecycle)
    }

    // A session in any active phase must not be re-launched.
    private fun sessionActive(): Boolean = when (state.value) {
        is ManagedSessionState.Preparing,
        is ManagedSessionState.Ready,
        is ManagedSessionState.Running,
        is ManagedSessionState.Stopping -> true
        else -> false
    }

    private fun start(gameId: String, lifecycle: Lifecycle) {
        pendingStart?.cancel()
        pendingStart = viewModelScope.launch {
            val game = repository.getGame(gameId)
            if (game == null) {
                ManagedSession.update(ManagedSessionState.Failed(RuntimeErrorCode.CONTENT_INVALID, "Game not found"))
                return@launch
            }
            // Read once: changing Settings cannot mutate an active session.
            val intent = Intent(ManagedSession.ACTION_START).setClassName(context.packageName, ManagedSession.SERVICE_CLASS)
                .putExtra(ManagedSession.EXTRA_GAME_ID, game.id)
                .putExtra(ManagedSession.EXTRA_GAME_PATH, game.relativePath)
                .putExtra(
                    ManagedSession.EXTRA_VULKAN_DRIVER,
                    RuntimeVulkanDriverPreference.decode(
                        context.getSharedPreferences(
                            RuntimeVulkanDriverPreference.FILE_NAME,
                            Context.MODE_PRIVATE,
                        ).getString(RuntimeVulkanDriverPreference.KEY, null),
                    ).name,
                )
            // Android refuses to start a service while the app is in the background, e.g. when a
            // launch reaches the activity while the device sleeps (which also leaves no Surface to
            // render to). Start only while the activity is in front; it can still be resumed for
            // a moment while the device goes to sleep, so a refused start is retried.
            val refusals = startWhenResumed(lifecycle) {
                try {
                    context.startService(intent)
                    savedState[KEY_STARTED] = true
                    true
                } catch (error: BackgroundServiceStartNotAllowedException) {
                    Log.w(TAG, "Start of ${game.id} refused in the background, waiting for the foreground: ${error.message}")
                    false
                }
            }
            if (refusals > 0) Log.i(TAG, "Started ${game.id} after $refusals refused attempts")
        }
    }

    private companion object {
        const val TAG = "SessionLaunch"
        const val KEY_STARTED = "session_started"
    }
}
