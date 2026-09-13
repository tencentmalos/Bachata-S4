package com.shadps4.android.runtime.session

import android.view.Surface
import com.shadps4.android.model.RuntimeErrorCode
import com.shadps4.android.runtime.diagnostics.DiagnosticReportContext
import com.shadps4.android.runtime.diagnostics.ProcessTerminationInfo
import com.shadps4.android.runtime.input.ControllerSnapshot
import java.util.concurrent.atomic.AtomicReference
import kotlinx.coroutines.flow.MutableStateFlow
import kotlinx.coroutines.flow.StateFlow

data class RuntimeSurface(
    val surface: Surface,
    val width: Int,
    val height: Int,
)

data class FrameTelemetry(val fps: Float = 0f, val frameTimeMs: Float = 0f)

sealed interface ManagedSessionState {
    data object Idle : ManagedSessionState
    data class Preparing(val stage: String, val generation: Long = 0L) : ManagedSessionState
    data class Ready(val gameId: String, val generation: Long) : ManagedSessionState
    data class Running(val gameId: String, val generation: Long = 0L) : ManagedSessionState
    data class Stopping(val gameId: String, val generation: Long) : ManagedSessionState
    data class Failed(
        val code: RuntimeErrorCode,
        val detail: String,
        val generation: Long = 0L,
        val reportContext: DiagnosticReportContext? = null,
    ) : ManagedSessionState
    data class Stopped(
        val exitCode: Int?,
        val generation: Long = 0L,
        val termination: ProcessTerminationInfo? = null,
        val reportContext: DiagnosticReportContext? = null,
    ) : ManagedSessionState {
        val userRequestedStop: Boolean
            get() = termination?.userRequestedStop == true || reportContext?.userRequestedStop == true
        val isUnexpected: Boolean
            get() = !userRequestedStop && (
                (exitCode != null && exitCode != 0) ||
                    termination?.terminationKind?.let {
                        it != com.shadps4.android.runtime.diagnostics.TerminationKind.EXITED &&
                            it != com.shadps4.android.runtime.diagnostics.TerminationKind.CANCELLED_BY_USER
                    } == true
                )
    }
}

object ManagedSession {
    const val ACTION_START = "com.shadps4.android.action.START_EMULATION"
    const val ACTION_STOP = "com.shadps4.android.action.STOP_EMULATION"
    const val EXTRA_GAME_ID = "game_id"
    const val EXTRA_GAME_PATH = "game_path"
    const val EXTRA_VULKAN_DRIVER = "vulkan_driver"
    const val SERVICE_CLASS = "com.shadps4.android.service.FexSessionService"

    private val mutableSurface = MutableStateFlow<RuntimeSurface?>(null)
    private val mutableState = MutableStateFlow<ManagedSessionState>(ManagedSessionState.Idle)
    private val controllerSink = AtomicReference<((ControllerSnapshot) -> Unit)?>(null)
    private val controllerSlotSink = AtomicReference<((Int, ControllerSnapshot) -> Unit)?>(null)
    private val mutableFrameTelemetry = MutableStateFlow(FrameTelemetry())
    private val frameTimes = ArrayDeque<Long>()
    private var lastFrameNanos: Long? = null
    val surface: StateFlow<RuntimeSurface?> = mutableSurface
    val state: StateFlow<ManagedSessionState> = mutableState
    val frameTelemetry: StateFlow<FrameTelemetry> = mutableFrameTelemetry

    fun attachSurface(value: RuntimeSurface) { mutableSurface.value = value }
    fun detachSurface(surface: Surface) {
        if (mutableSurface.value?.surface === surface) mutableSurface.value = null
    }
    fun update(value: ManagedSessionState) { mutableState.value = value }

    // Generation guard: a late observer from an old session must not clobber a newer one. The
    // native SessionCore is the source of truth for generation identity; this mirror lets the
    // Service publish states without a stale watcher overwriting the current session's UI state.
    @Volatile private var currentGeneration = 0L

    @Synchronized
    fun beginGeneration(generation: Long) {
        if (generation > currentGeneration) currentGeneration = generation
    }

    /** Publish [state] only if [generation] is the current one; drops stale-generation updates. */
    @Synchronized
    fun updateIfCurrent(generation: Long, state: ManagedSessionState) {
        if (generation == currentGeneration) mutableState.value = state
    }
    fun controllerPublisher(generation: Long, slot: Int = 0): (ControllerSnapshot) -> Unit {
        val sink = controllerSlotSink.get()
        return { snapshot ->
            if (generation != 0L && generation == currentGeneration) sink?.invoke(slot,snapshot)
        }
    }
    fun attachControllerSink(sink: (ControllerSnapshot) -> Unit) { controllerSink.set(sink) }
    fun detachControllerSink(sink: (ControllerSnapshot) -> Unit) { controllerSink.compareAndSet(sink, null) }
    fun submitController(snapshot: ControllerSnapshot) { submitController(0, snapshot) }
    fun attachControllerSlotSink(sink: (Int, ControllerSnapshot) -> Unit) { controllerSlotSink.set(sink) }
    fun detachControllerSlotSink(sink: (Int, ControllerSnapshot) -> Unit) { controllerSlotSink.compareAndSet(sink, null) }
    fun submitController(slot: Int, snapshot: ControllerSnapshot) {
        require(slot in 0..3) { "Controller slot must be 0..3" }
        controllerSlotSink.get()?.invoke(slot, snapshot)
        if (slot == 0) controllerSink.get()?.invoke(snapshot)
    }
    @Synchronized
    fun recordPresentedFrame(nowNanos: Long = System.nanoTime()) {
        if (lastFrameNanos?.let { nowNanos <= it } == true) frameTimes.clear()
        lastFrameNanos = nowNanos
        frameTimes.addLast(nowNanos)
        val cutoff = nowNanos - 1_000_000_000L
        while (frameTimes.firstOrNull()?.let { it < cutoff } == true) frameTimes.removeFirst()
        val intervals = frameTimes.size - 1
        if (intervals > 0) {
            val duration = nowNanos - frameTimes.first()
            val fps = intervals * 1_000_000_000f / duration.coerceAtLeast(1L)
            mutableFrameTelemetry.value = FrameTelemetry(fps, 1000f / fps.coerceAtLeast(0.01f))
        }
    }
    @Synchronized
    fun refreshFrameTelemetry(nowNanos: Long = System.nanoTime()) {
        val last = lastFrameNanos ?: return
        val idleNanos = nowNanos - last
        if (idleNanos >= 1_000_000_000L) {
            mutableFrameTelemetry.value = FrameTelemetry(0f, idleNanos / 1_000_000f)
        }
    }
}
