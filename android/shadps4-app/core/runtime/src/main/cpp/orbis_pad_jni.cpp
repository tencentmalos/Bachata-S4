// SPDX-FileCopyrightText: Copyright 2026 shadPS4 Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

// JNI surface for the native pad adapter (Core::HostRuntime::OrbisPadAdapter).
//
// This is the native consumer the Kotlin input stack was missing. The Android
// front end (GamepadInputManager for a physical pad, or the touch overlay)
// resolves every source into a ControllerSnapshot whose `buttons` are already in
// OrbisPadButtonDataOffset bit form, sticks in [-1,1], triggers in [0,1]. Before
// this bridge, ManagedSession.submitController() published that snapshot to a
// Kotlin sink that had no native consumer.
//
// This file is a THIN marshaller over the process-global OrbisPadAdapter:
//   * copies stable POD across JNI (a jlong of bits, six jfloats) — never a
//     native/guest pointer,
//   * carries a session token so a stale producer from a previous game cannot
//     write into the current session's pad (the adapter enforces it),
//   * catches every C++ exception at the boundary and returns a defined ordinal,
//     so an exception never crosses JNI.
//
// It does NOT own lifecycle: BeginSession/EndSession are driven by the Kotlin
// session so the pad generation follows the game generation. The eventual guest
// scePadReadState consumer reads from the SAME GlobalPadAdapter(); wiring that is
// a later stage (the Android host is not yet running a real PS4 game).

#include <jni.h>

#include <cstdint>

#include <android/log.h>

#include "core/host_runtime/orbis_pad_adapter.h"

namespace {

using Core::HostRuntime::GlobalPadAdapter;
using Core::HostRuntime::PadResult;
using Core::HostRuntime::PadSnapshot;
using Core::HostRuntime::PadVibration;

constexpr const char* kTag = "OrbisPad";

jint ResultOrdinal(PadResult r) {
    return static_cast<jint>(r);
}

} // namespace

extern "C" JNIEXPORT jlong JNICALL
Java_com_shadps4_android_runtime_input_NativePad_nativeBeginSession(JNIEnv*, jclass) {
    try {
        return static_cast<jlong>(GlobalPadAdapter().BeginSession());
    } catch (...) {
        __android_log_print(ANDROID_LOG_ERROR, kTag, "beginSession threw");
        return 0;
    }
}

extern "C" JNIEXPORT void JNICALL
Java_com_shadps4_android_runtime_input_NativePad_nativeEndSession(JNIEnv*, jclass, jlong token) {
    try {
        GlobalPadAdapter().EndSession(static_cast<std::uint64_t>(token));
    } catch (...) {
        __android_log_print(ANDROID_LOG_ERROR, kTag, "endSession threw");
    }
}

extern "C" JNIEXPORT jlong JNICALL
Java_com_shadps4_android_runtime_input_NativePad_nativeCurrentToken(JNIEnv*, jclass) {
    try {
        return static_cast<jlong>(GlobalPadAdapter().CurrentToken());
    } catch (...) {
        return 0;
    }
}

// Submits one port's snapshot. Returns a PadResult ordinal.
extern "C" JNIEXPORT jint JNICALL
Java_com_shadps4_android_runtime_input_NativePad_nativeSubmit(
    JNIEnv*, jclass, jlong token, jint port, jlong buttons, jfloat left_x, jfloat left_y,
    jfloat right_x, jfloat right_y, jfloat left_trigger, jfloat right_trigger, jboolean touch_down,
    jfloat touch_x, jfloat touch_y) {
    try {
        PadSnapshot snap;
        snap.buttons = static_cast<std::uint64_t>(buttons);
        snap.left_x = left_x;
        snap.left_y = left_y;
        snap.right_x = right_x;
        snap.right_y = right_y;
        snap.left_trigger = left_trigger;
        snap.right_trigger = right_trigger;
        snap.touch_down = touch_down == JNI_TRUE;
        snap.touch_x = touch_x;
        snap.touch_y = touch_y;
        return ResultOrdinal(
            GlobalPadAdapter().Submit(static_cast<std::uint64_t>(token), port, snap));
    } catch (...) {
        __android_log_print(ANDROID_LOG_ERROR, kTag, "submit threw");
        return static_cast<jint>(PadResult::Rejected);
    }
}

extern "C" JNIEXPORT jint JNICALL
Java_com_shadps4_android_runtime_input_NativePad_nativeSetConnected(JNIEnv*, jclass, jlong token,
                                                                    jint port, jboolean connected) {
    try {
        return ResultOrdinal(GlobalPadAdapter().SetConnected(
            static_cast<std::uint64_t>(token), port, connected == JNI_TRUE));
    } catch (...) {
        return static_cast<jint>(PadResult::Rejected);
    }
}

// Read-back of a port's current PS4 button bits (verification / telemetry).
extern "C" JNIEXPORT jlong JNICALL
Java_com_shadps4_android_runtime_input_NativePad_nativeReadButtons(JNIEnv*, jclass, jint port) {
    try {
        return static_cast<jlong>(GlobalPadAdapter().ReadButtons(port));
    } catch (...) {
        return 0;
    }
}

// Read-back of a port's converted analog state, packed for verification:
// [leftX, leftY, rightX, rightY, l2, r2] as u8 values in an int array. Returns
// null on a bad port.
extern "C" JNIEXPORT jintArray JNICALL
Java_com_shadps4_android_runtime_input_NativePad_nativeReadAnalog(JNIEnv* env, jclass, jint port) {
    try {
        Libraries::Pad::OrbisPadData d{};
        if (!GlobalPadAdapter().ReadState(port, &d)) {
            return nullptr;
        }
        jint vals[6] = {
            d.leftStick.x,      d.leftStick.y,      d.rightStick.x,
            d.rightStick.y,     d.analogButtons.l2, d.analogButtons.r2,
        };
        jintArray arr = env->NewIntArray(6);
        if (arr == nullptr) {
            return nullptr;
        }
        env->SetIntArrayRegion(arr, 0, 6, vals);
        return arr;
    } catch (...) {
        return nullptr;
    }
}

extern "C" JNIEXPORT jboolean JNICALL
Java_com_shadps4_android_runtime_input_NativePad_nativeConnected(JNIEnv*, jclass, jint port) {
    try {
        return GlobalPadAdapter().Connected(port) ? JNI_TRUE : JNI_FALSE;
    } catch (...) {
        return JNI_FALSE;
    }
}

extern "C" JNIEXPORT jint JNICALL
Java_com_shadps4_android_runtime_input_NativePad_nativeSetVibration(JNIEnv*, jclass, jlong token,
                                                                    jint port, jint small_motor,
                                                                    jint large_motor) {
    try {
        const auto clamp = [](jint v) -> std::uint8_t {
            return static_cast<std::uint8_t>(v < 0 ? 0 : (v > 255 ? 255 : v));
        };
        return ResultOrdinal(GlobalPadAdapter().SetVibration(
            static_cast<std::uint64_t>(token), port, clamp(small_motor), clamp(large_motor)));
    } catch (...) {
        return static_cast<jint>(PadResult::Rejected);
    }
}

// Drains a pending vibration for `port`. Returns:
//   -1 : nothing pending
//    0 : a cancel (motors zero)
//   >0 : (small << 8) | large  (both 0..255)
extern "C" JNIEXPORT jint JNICALL
Java_com_shadps4_android_runtime_input_NativePad_nativeDrainVibration(JNIEnv*, jclass, jint port) {
    try {
        PadVibration v{};
        if (!GlobalPadAdapter().DrainVibration(port, &v)) {
            return -1;
        }
        if (v.cancel) {
            return 0;
        }
        return (static_cast<jint>(v.small_motor) << 8) | static_cast<jint>(v.large_motor);
    } catch (...) {
        return -1;
    }
}
