package com.shadps4.fexvalidation

/** JNI bridge to libshadps4_fex_validation.so. */
object NativeBridge {
    init {
        System.loadLibrary("shadps4_fex_validation")
    }

    /** Process/page/device and loaded-library Build ID. */
    external fun nativeIdentity(): String

    /** Host page size. */
    external fun nativePageSize(): Int

    /**
     * Run the self-contained x86-64 increment routine in a fresh guest session.
     * Returns input+1 on success, -1 on failure (with [onError] populated).
     */
    external fun runGuestIncrement(input: Long, errorOut: Array<String?>): Long
}
