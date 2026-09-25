package com.shadps4.android.runtime.input

import android.content.Context
import android.hardware.Sensor
import android.hardware.SensorEvent
import android.hardware.SensorEventListener
import android.hardware.SensorManager
import android.hardware.display.DisplayManager
import android.os.Handler
import android.os.Looper
import android.view.KeyEvent
import android.view.MotionEvent
import android.view.Display
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
    private var profiles = List(NativePad.MAX_PORTS) { ControllerProfile.standard() }

    fun configureProfiles(value: List<ControllerProfile>) {
        onMain()
        require(value.size <= NativePad.MAX_PORTS)
        val next = List(NativePad.MAX_PORTS) { value.getOrNull(it) ?: ControllerProfile.standard() }
        if (next == profiles) return
        profiles = next
        current?.reloadButtonMappings()
    }
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
    fun overlayPointer(id: Int, phase: Int, x: Float, y: Float): Boolean {
        onMain()
        val connection = current ?: return false
        if (!connection.focused) return false
        return NativePad.nativeOverlayPointer(connection.token, id, phase, x, y)
    }
    fun updateOverlayDensity(density: Float) { onMain(); NativePad.nativeOverlayDensity(density) }
    fun cancelOverlayPointers() { onMain(); current?.let { NativePad.nativeOverlayCancel(it.token) } }
    fun openOverlayControls() { onMain(); current?.let { NativePad.nativeOverlayControls(it.token) } }
    fun currentToken(): Long = current?.token ?: 0
    /** Read-only diagnostics. Values are sampled, not an atomic input transaction. */
    fun diagnosticStatus(): String {
        val connection = current
        return "input: generation=${connection?.generation ?: 0} token=${connection?.token ?: 0}" +
            " windowFocused=$windowFocused focused=${connection?.focused}" +
            " uiCaptured=${connection?.uiCaptured} stopping=${connection?.stopping}" +
            " overlayEvents=${connection?.overlayEvents ?: 0}" +
            " overlayResult=${connection?.overlayResult}" +
            " nativeToken=${NativePad.nativeCurrentToken()}" +
            " connected=${NativePad.nativeConnected(0)} buttons=${NativePad.nativeReadButtons(0)}"
    }
    fun hasPhysicalController(): Boolean = current?.bindings?.isNotEmpty() == true
    fun dispatchKeyEvent(event: KeyEvent): Boolean { onMain(); return current?.dispatchKey(event) ?: false }
    fun dispatchGenericMotionEvent(event: MotionEvent): Boolean { onMain(); return current?.dispatchMotion(event) ?: false }
    fun setFocused(focused: Boolean) { onMain(); if (!focused) cancelOverlayPointers(); windowFocused = focused; current?.let { it.setFocused(focused && !it.uiCaptured) } }
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
            if (captured) cancelOverlayPointers()
            it.uiCaptured = captured
            it.setFocused(windowFocused && !captured)
        }
    }

    private class Binding(val port: Int,val nativeEpoch: Long,val device: DeviceIdentity, val buttons: NativeButtonMapping)
    private class Connection(context: Context,val generation: Long,val token: Long) : InputSink, HapticFeedbackSource {
        val bindings = LinkedHashMap<DeviceIdentity,Binding>()
        private val sustained = LinkedHashMap<DeviceIdentity,HapticCommand>()
        private var lastRefresh = 0L
        private var closed = false
        var uiCaptured = false
        var stopping = false
        @Volatile var focused = true
            private set
        @Volatile var overlayEvents = 0L
            private set
        @Volatile var overlayResult: Int? = null
            private set
        private val sensorManager = context.getSystemService(Context.SENSOR_SERVICE) as? SensorManager
        private val gyro = sensorManager?.getDefaultSensor(Sensor.TYPE_GYROSCOPE)
        private val gravity = sensorManager?.getDefaultSensor(Sensor.TYPE_GRAVITY)
            ?: sensorManager?.getDefaultSensor(Sensor.TYPE_ACCELEROMETER)
        private val neutralFrame = VrGyroAxes.NeutralFrame()
        private val sensorDisplay = (context.getSystemService(Context.DISPLAY_SERVICE) as? DisplayManager)
            ?.getDisplay(Display.DEFAULT_DISPLAY)
        private val gyroListener = object : SensorEventListener {
            override fun onSensorChanged(event: SensorEvent) {
                if (closed || !focused) return
                val values = event.values
                if (values.size < 3) return
                if (event.sensor.type == gravity?.type) {
                    if (neutralFrame.observeGravity(values[0], values[1], values[2], sensorDisplay?.rotation ?: 0)) {
                        android.util.Log.i("VrGyro", "Neutral axes calibrated: rotation=${sensorDisplay?.rotation} gravity=${values.take(3)}")
                        sensorManager?.unregisterListener(this, gravity)
                    }
                    return
                }
                if (event.sensor.type != Sensor.TYPE_GYROSCOPE) return
                val rate = if (gravity != null) neutralFrame.toHead(values[0], values[1], values[2])
                    else VrGyroAxes.toDisplay(values[0], values[1], values[2], sensorDisplay?.rotation ?: 0)
                // Until gravity is stable, preserve the initial forward pose.
                if (rate != null) NativePad.nativeUpdateVrGyro(rate[0], rate[1], rate[2], event.timestamp)
            }
            override fun onAccuracyChanged(sensor: Sensor?, accuracy: Int) = Unit
        }
        // Direct executor is safe here: every source method and device-listener
        // callback runs on the same supplied main looper.
        private val source = AndroidInputSource(context,Executor { it.run() },this)
        private val actuator = AndroidHapticsExecutor(context,this)
        private val overlay: (Int,ControllerSnapshot)->Unit = { slot,snapshot ->
            // May be captured by a retiring producer. The token is immutable.
            main.post {
                overlayEvents++
                if (!closed && focused && slot in 0 until NativePad.MAX_PORTS) {
                    overlayResult = NativePad.submit(token,slot,snapshot)
                }
            }
        }
        private val tick = object : Runnable {
            override fun run() {
                if (closed) return
                if (focused) actuator.pumpOnce()
                main.postDelayed(this,16)
            }
        }
        fun start() {
            NativePad.nativeSetVrSbsEnabled(true)
            NativePad.nativeSetConnected(token,0,true)
            ManagedSession.attachControllerSlotSink(overlay)
            source.start(main)
            gyro?.let { sensorManager?.registerListener(gyroListener, it, SensorManager.SENSOR_DELAY_GAME, main) }
            if (!neutralFrame.calibrated) gravity?.let {
                sensorManager?.registerListener(gyroListener, it, SensorManager.SENSOR_DELAY_GAME, main)
            }
            main.post(tick)
        }
        fun close() {
            if (closed) return
            closed = true
            ManagedSession.detachControllerSlotSink(overlay)
            source.stop()
            sensorManager?.unregisterListener(gyroListener)
            bindings.clear(); sustained.clear()
            main.removeCallbacks(tick)
            actuator.close()
            NativePad.nativeEndSession(token)
            NativePad.nativeSetVrSbsEnabled(false)
        }
        fun setFocused(value: Boolean) {
            if (value && stopping) return
            if (closed || focused == value) return
            focused = value
            if (!value) {
                source.stop() // unregister exact epochs; queued old packets cannot reconnect
                sensorManager?.unregisterListener(gyroListener)
                bindings.clear(); sustained.clear()
                NativePad.nativeFocusLost(token)
                NativePad.nativeSetConnected(token,0,false)
                actuator.cancelAll()
            } else {
                NativePad.nativeSetConnected(token,0,true)
                source.start(main)
                gyro?.let { sensorManager?.registerListener(gyroListener, it, SensorManager.SENSOR_DELAY_GAME, main) }
                if (!neutralFrame.calibrated) gravity?.let {
                    sensorManager?.registerListener(gyroListener, it, SensorManager.SENSOR_DELAY_GAME, main)
                }
            }
        }
        fun reloadButtonMappings() {
            if (closed || !focused) return
            // Retire held buttons and exact source epochs before changing destinations.
            // Overlay owns independent state and is deliberately not reset here.
            source.stop()
            source.start(main)
        }
        fun dispatchKey(e: KeyEvent) = !closed && focused && source.dispatchKeyEvent(e)
        fun dispatchMotion(e: MotionEvent) = !closed && focused && source.dispatchGenericMotionEvent(e)
        override fun onDeviceAvailable(device: DeviceIdentity,capabilities: DeviceCapabilities) {
            if (closed || !focused) return
            val androidDevice = android.view.InputDevice.getDevice(device.backendId.toInt())
            val port = (0 until NativePad.MAX_PORTS).firstOrNull { p ->
                val selected = profiles[p].device
                bindings.values.none { it.port == p } && (selected == null ||
                    (selected.descriptor == device.profileKey && androidDevice != null &&
                     selected.vendorId == androidDevice.vendorId && selected.productId == androidDevice.productId))
            } ?: return
            val axes = capabilities.axes.flatMap { listOf(it.axis.ordinal.toFloat(),it.rawMin,it.rawMax,it.rawFlat) }.toFloatArray()
            val epoch = NativePad.nativeRegisterDevice(token,port,device.backendId,axes,capabilities.hasRumble)
            if (epoch == 0L) return
            val nativeDevice = device.copy(connectionEpoch = epoch)
            bindings[device] = Binding(port,epoch,nativeDevice,NativeButtonMapping(profiles[port]))
            actuator.register(nativeDevice)
        }
        override fun onInputPacket(packet: InputPacket) {
            val b = bindings[packet.device] ?: return
            if (packet.control == ControlEvent.DeviceRemoved) {
                NativePad.nativeRemoveDevice(token,b.port,b.nativeEpoch)
                actuator.remove(b.device); sustained.remove(b.device); bindings.remove(packet.device); return
            }
            if (closed || !focused || packet.protocolVersion != INPUT_PROTOCOL_VERSION) return
            val events = packet.events.flatMap(b.buttons::map)
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
