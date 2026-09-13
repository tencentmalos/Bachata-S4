package com.shadps4.android.runtime.input

import android.content.Context
import android.os.Handler
import android.os.Looper
import android.view.KeyEvent
import android.view.MotionEvent
import com.shadps4.android.runtime.session.ManagedSession
import java.util.concurrent.Executor
import spatial.input.android.*
import spatial.input.model.*

/** Application owner. Every queued callback closes over its original connection;
 * retirement cannot acquire the next session's token. All Android objects and
 * actuator calls are confined to the main looper, never a Java-calling C++ worker.
 */
object NativePadBridge {
    private val main = Handler(Looper.getMainLooper())
    @Volatile private var current: Connection? = null
    private var windowFocused = true
    private fun onMain() = check(Looper.myLooper() == main.looper)

    fun begin(context: Context, generation: Long): Long {
        onMain()
        current?.let { if (it.generation == generation) return it.token; it.close() }
        val t = NativePad.nativeBeginSession()
        check(t != 0L)
        current = Connection(context.applicationContext,generation,t).also { it.start(); it.setFocused(windowFocused) }
        return t
    }
    fun end(generation: Long) {
        val action = Runnable { current?.takeIf { it.generation == generation }?.let { it.close(); current = null } }
        if (Looper.myLooper() == main.looper) action.run() else main.post(action)
    }
    fun currentToken(): Long = current?.token ?: 0
    fun hasPhysicalController(): Boolean = current?.bindings?.isNotEmpty() == true
    fun dispatchKeyEvent(event: KeyEvent): Boolean { onMain(); return current?.dispatchKey(event) ?: false }
    fun dispatchGenericMotionEvent(event: MotionEvent): Boolean { onMain(); return current?.dispatchMotion(event) ?: false }
    fun setFocused(focused: Boolean) { onMain(); windowFocused = focused; current?.let { it.setFocused(focused && !it.uiCaptured) } }
    fun requestStop(generation: Long) {
        onMain()
        current?.takeIf { it.generation == generation }?.let {
            it.stopping = true
            it.setFocused(false)
        }
    }
    fun setUiCaptured(generation: Long, captured: Boolean) {
        onMain()
        current?.takeIf { it.generation == generation }?.let {
            it.uiCaptured = captured
            it.setFocused(windowFocused && !captured)
        }
    }

    private class Binding(val port: Int,val nativeEpoch: Long,val device: DeviceIdentity)
    private class Connection(context: Context,val generation: Long,val token: Long) : InputSink, HapticFeedbackSource {
        val bindings = LinkedHashMap<DeviceIdentity,Binding>()
        private val sustained = LinkedHashMap<DeviceIdentity,HapticCommand>()
        private var lastRefresh = 0L
        private var closed = false
        var uiCaptured = false
        var stopping = false
        private var focused = true
        // Direct executor is safe here: every source method and device-listener
        // callback runs on the same supplied main looper.
        private val source = AndroidInputSource(context,Executor { it.run() },this)
        private val actuator = AndroidHapticsExecutor(context,this)
        private val overlay: (Int,ControllerSnapshot)->Unit = { slot,snapshot ->
            // May be captured by a retiring producer. The token is immutable.
            main.post { if (!closed && focused && slot in 0 until NativePad.MAX_PORTS) NativePad.submit(token,slot,snapshot) }
        }
        private val tick = object : Runnable {
            override fun run() {
                if (closed) return
                if (focused) actuator.pumpOnce()
                main.postDelayed(this,16)
            }
        }
        fun start() {
            NativePad.nativeSetConnected(token,0,true)
            ManagedSession.attachControllerSlotSink(overlay)
            source.start(main)
            main.post(tick)
        }
        fun close() {
            if (closed) return
            closed = true
            ManagedSession.detachControllerSlotSink(overlay)
            source.stop()
            bindings.clear(); sustained.clear()
            main.removeCallbacks(tick)
            actuator.close()
            NativePad.nativeEndSession(token)
        }
        fun setFocused(value: Boolean) {
            if (value && stopping) return
            if (closed || focused == value) return
            focused = value
            if (!value) {
                source.stop() // unregister exact epochs; queued old packets cannot reconnect
                bindings.clear(); sustained.clear()
                NativePad.nativeFocusLost(token)
                NativePad.nativeSetConnected(token,0,false)
                actuator.cancelAll()
            } else {
                NativePad.nativeSetConnected(token,0,true)
                source.start(main)
            }
        }
        fun dispatchKey(e: KeyEvent) = !closed && focused && source.dispatchKeyEvent(e)
        fun dispatchMotion(e: MotionEvent) = !closed && focused && source.dispatchGenericMotionEvent(e)
        override fun onDeviceAvailable(device: DeviceIdentity,capabilities: DeviceCapabilities) {
            if (closed || !focused) return
            val port = (0 until NativePad.MAX_PORTS).firstOrNull { p -> bindings.values.none { it.port == p } } ?: return
            val axes = capabilities.axes.flatMap { listOf(it.axis.ordinal.toFloat(),it.rawMin,it.rawMax,it.rawFlat) }.toFloatArray()
            val epoch = NativePad.nativeRegisterDevice(token,port,device.backendId,axes,capabilities.hasRumble)
            if (epoch == 0L) return
            val nativeDevice = device.copy(connectionEpoch = epoch)
            bindings[device] = Binding(port,epoch,nativeDevice)
            actuator.register(nativeDevice)
            if (port == 0) NativePad.submit(token,0,ControllerSnapshot.Neutral)
        }
        override fun onInputPacket(packet: InputPacket) {
            val b = bindings[packet.device] ?: return
            if (packet.control == ControlEvent.DeviceRemoved) {
                NativePad.nativeRemoveDevice(token,b.port,b.nativeEpoch)
                actuator.remove(b.device); sustained.remove(b.device); bindings.remove(packet.device); return
            }
            if (closed || !focused || packet.protocolVersion != INPUT_PROTOCOL_VERSION) return
            val events = packet.events
            if (events.any { it.kind == InputEvent.Kind.SensorSample }) return // not claimed by this adapter
            val keys = events.flatMap { listOf(it.kind.ordinal,it.button?.ordinal ?: it.axis?.ordinal ?: -1) }.toIntArray()
            val values = events.map { if (it.kind == InputEvent.Kind.ButtonState) if (it.pressed) 1f else 0f else it.rawValue }.toFloatArray()
            NativePad.nativePacket(token,b.port,b.device.backendId,b.nativeEpoch,packet.sequence,packet.control.ordinal,keys,values)
        }
        override fun drainHaptics(max: Int): List<HapticCommand> {
            if (closed || !focused) return emptyList()
            val raw = NativePad.nativeDrainHaptics(token) ?: return emptyList()
            if (raw.size % 6 != 0) return emptyList()
            val changes = raw.asList().chunked(6).mapNotNull { c ->
                val b = bindings.values.firstOrNull { it.device.backendId == c[0] && it.nativeEpoch == c[1] } ?: return@mapNotNull null
                HapticCommand(b.device,0,c[3]/255f,c[2]/255f,c[4].toInt(),c[5] != 0L)
            }
            // scePad motor levels persist until changed. Refresh bounded Android
            // one-shots here; Foundation's commands retain finite duration semantics.
            changes.forEach { if (it.cancel) sustained.remove(it.device) else sustained[it.device] = it }
            val now = android.os.SystemClock.uptimeMillis()
            val refresh = if (now-lastRefresh >= 200) {
                lastRefresh = now
                sustained.values.filter { old -> changes.none { it.device == old.device } }
            } else emptyList()
            return (changes + refresh).take(max)
        }
    }
}
