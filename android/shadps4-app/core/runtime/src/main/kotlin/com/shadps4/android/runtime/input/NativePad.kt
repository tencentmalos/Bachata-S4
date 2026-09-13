package com.shadps4.android.runtime.input

/** App JNI. State and production scePad* live once in libshadps4_host.so. */
object NativePad {
    object Result { const val OK=0; const val WRONG_SESSION=1; const val BAD_PORT=2; const val REJECTED=3; const val NO_SESSION=4 }
    const val MAX_PORTS = 4
    external fun nativeInitializeHost(path: String): Boolean
    external fun nativeBeginSession(): Long
    external fun nativeEndSession(token: Long)
    external fun nativeCurrentToken(): Long
    external fun nativeSubmit(token: Long,port: Int,buttons: Long,leftX: Float,leftY: Float,
        rightX: Float,rightY: Float,leftTrigger: Float,rightTrigger: Float,touchDown: Boolean,touchX: Float,touchY: Float): Int
    external fun nativeSetConnected(token: Long,port: Int,connected: Boolean): Int
    external fun nativeReadButtons(port: Int): Long
    external fun nativeReadAnalog(port: Int): IntArray?
    external fun nativeConnected(port: Int): Boolean
    external fun nativeSetVibration(token: Long,port: Int,smallMotor: Int,largeMotor: Int): Int
    external fun nativeRegisterDevice(token: Long,port: Int,id: Long,axes: FloatArray,rumble: Boolean): Long
    external fun nativePacket(token: Long,port: Int,id: Long,epoch: Long,sequence: Long,control: Int,
        keys: IntArray,values: FloatArray): Int
    external fun nativeRemoveDevice(token: Long,port: Int,epoch: Long)
    external fun nativeFocusLost(token: Long)
    external fun nativeDrainHaptics(token: Long): LongArray?
    // Diagnostics call the production HLE exports, not a parallel test implementation.
    external fun nativeOpenDefaultPad(): Int
    external fun nativeReadPad(handle: Int): LongArray?
    external fun nativeVibratePad(handle: Int,small: Int,large: Int): Int
    fun submit(token: Long,port: Int,snapshot: ControllerSnapshot): Int = nativeSubmit(token,port,
        snapshot.buttons,snapshot.leftX,snapshot.leftY,snapshot.rightX,snapshot.rightY,
        snapshot.leftTrigger,snapshot.rightTrigger,snapshot.touchDown,snapshot.touchX,snapshot.touchY)
}
