// SPDX-FileCopyrightText: Copyright 2026 shadPS4 Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

// App-owned JNI marshalling. The host DSO owns the sole adapter and scePad*
// state. Foundation owns no native methods or Java references.

#include <jni.h>

#include "common/path_util.h"
#include "core/user_settings.h"
#include <cstdint>
#include <vector>

#include <android/log.h>

#include "core/host_runtime/orbis_pad_adapter.h"

namespace {

using Core::HostRuntime::GlobalPadAdapter;
using Core::HostRuntime::PadResult;
using Core::HostRuntime::PadSnapshot;
using namespace spatial::input;
using namespace Libraries::Pad;

constexpr const char *kTag = "OrbisPad";

jint ResultOrdinal(PadResult r) {
    return static_cast<jint>(r);
}

} // namespace

extern "C" JNIEXPORT jlong JNICALL
Java_com_shadps4_android_runtime_input_NativePad_nativeBeginSession(JNIEnv *, jclass) {
    try {
        return static_cast<jlong>(GlobalPadAdapter().BeginSession());
    } catch (...) {
        __android_log_print(ANDROID_LOG_ERROR, kTag, "beginSession threw");
        return 0;
    }
}

extern "C" JNIEXPORT void JNICALL
Java_com_shadps4_android_runtime_input_NativePad_nativeEndSession(JNIEnv *, jclass, jlong token) {
    try {
        GlobalPadAdapter().EndSession(static_cast<std::uint64_t>(token));
    } catch (...) {
        __android_log_print(ANDROID_LOG_ERROR, kTag, "endSession threw");
    }
}

extern "C" JNIEXPORT jlong JNICALL
Java_com_shadps4_android_runtime_input_NativePad_nativeCurrentToken(JNIEnv *, jclass) {
    try {
        return static_cast<jlong>(GlobalPadAdapter().CurrentToken());
    } catch (...) {
        return 0;
    }
}

// Submits one port's snapshot. Returns a PadResult ordinal.
extern "C" JNIEXPORT jint JNICALL Java_com_shadps4_android_runtime_input_NativePad_nativeSubmit(
    JNIEnv *, jclass, jlong token, jint port, jlong buttons, jfloat left_x, jfloat left_y,
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
Java_com_shadps4_android_runtime_input_NativePad_nativeSetConnected(JNIEnv *, jclass, jlong token,
                                                                    jint port, jboolean connected) {
    try {
        return ResultOrdinal(GlobalPadAdapter().SetConnected(static_cast<std::uint64_t>(token),
                                                             port, connected == JNI_TRUE));
    } catch (...) {
        return static_cast<jint>(PadResult::Rejected);
    }
}

// Read-back of a port's current PS4 button bits (verification / telemetry).
extern "C" JNIEXPORT jlong JNICALL
Java_com_shadps4_android_runtime_input_NativePad_nativeReadButtons(JNIEnv *, jclass, jint port) {
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
Java_com_shadps4_android_runtime_input_NativePad_nativeReadAnalog(JNIEnv *env, jclass, jint port) {
    try {
        Libraries::Pad::OrbisPadData d{};
        if (!GlobalPadAdapter().ReadState(port, &d)) {
            return nullptr;
        }
        jint vals[6] = {
            d.leftStick.x,  d.leftStick.y,      d.rightStick.x,
            d.rightStick.y, d.analogButtons.l2, d.analogButtons.r2,
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
Java_com_shadps4_android_runtime_input_NativePad_nativeConnected(JNIEnv *, jclass, jint port) {
    try {
        return GlobalPadAdapter().Connected(port) ? JNI_TRUE : JNI_FALSE;
    } catch (...) {
        return JNI_FALSE;
    }
}

extern "C" JNIEXPORT jint JNICALL
Java_com_shadps4_android_runtime_input_NativePad_nativeSetVibration(JNIEnv *, jclass, jlong token,
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

extern "C" JNIEXPORT jboolean JNICALL
Java_com_shadps4_android_runtime_input_NativePad_nativeInitializeHost(JNIEnv *env, jclass,
                                                                      jstring path) {
    if (!path)
        return JNI_FALSE;
    const char *chars = env->GetStringUTFChars(path, nullptr);
    if (!chars)
        return JNI_FALSE;
    try {
        std::string value(chars);
        env->ReleaseStringUTFChars(path, chars);
        chars = nullptr;
        Common::FS::InitializeAndroidUserPaths(value);
        return JNI_TRUE;
    } catch (...) {
        if (chars)
            env->ReleaseStringUTFChars(path, chars);
        return JNI_FALSE;
    }
}
extern "C" JNIEXPORT jlong JNICALL
Java_com_shadps4_android_runtime_input_NativePad_nativeRegisterDevice(JNIEnv *env, jclass,
                                                                      jlong token, jint port,
                                                                      jlong id, jfloatArray axes,
                                                                      jboolean rumble) {
    try {
        if (!axes)
            return 0;
        const int n = env->GetArrayLength(axes);
        if (n > 24 || n % 4)
            return 0;
        std::vector<jfloat> data(n);
        env->GetFloatArrayRegion(axes, 0, n, data.data());
        if (env->ExceptionCheck())
            return 0;
        DeviceCapabilities caps;
        for (int i = 0; i < n; i += 4) {
            if (!(data[i] >= 0 && data[i] < 6) || data[i] != int(data[i]))
                return 0;
            caps.axes.push_back({Axis(int(data[i])), data[i + 1], data[i + 2], data[i + 3]});
        }
        caps.has_rumble = rumble;
        caps.rumble_actuators = rumble ? 1 : 0;
        return GlobalPadAdapter().RegisterDevice(token, port, id, caps);
    } catch (...) {
        return 0;
    }
}
extern "C" JNIEXPORT jint JNICALL Java_com_shadps4_android_runtime_input_NativePad_nativePacket(
    JNIEnv *env, jclass, jlong token, jint port, jlong id, jlong epoch, jlong sequence,
    jint control, jintArray keys, jfloatArray values) {
    try {
        if (!keys || !values || sequence <= 0 || epoch <= 0 || control < 0 || control > 5)
            return 3;
        const int n = env->GetArrayLength(values);
        if (n > 256 || env->GetArrayLength(keys) != n * 2)
            return 3;
        std::vector<jint> k(n * 2);
        std::vector<jfloat> v(n);
        env->GetIntArrayRegion(keys, 0, n * 2, k.data());
        env->GetFloatArrayRegion(values, 0, n, v.data());
        if (env->ExceptionCheck())
            return 3;
        InputPacket p;
        p.session_token = token;
        p.device = {Source::AndroidGamepad, id, u64(epoch), ""};
        p.sequence = sequence;
        p.control = ControlEvent(control);
        for (int i = 0; i < n; ++i) {
            InputEvent e;
            if (k[i * 2] == 0 && k[i * 2 + 1] >= 0 && k[i * 2 + 1] < 18 &&
                (v[i] == 0 || v[i] == 1)) {
                e.button = Button(k[i * 2 + 1]);
                e.pressed = v[i] != 0;
            } else if (k[i * 2] == 1 && k[i * 2 + 1] >= 0 && k[i * 2 + 1] < 6) {
                e.kind = InputEvent::Kind::AxisValue;
                e.axis = Axis(k[i * 2 + 1]);
                e.raw_value = v[i];
            } else
                return 3;
            p.events.push_back(e);
        }
        return ResultOrdinal(GlobalPadAdapter().SubmitPacket(token, port, p));
    } catch (...) {
        return 3;
    }
}
extern "C" JNIEXPORT void JNICALL
Java_com_shadps4_android_runtime_input_NativePad_nativeRemoveDevice(JNIEnv *, jclass, jlong t,
                                                                    jint port, jlong epoch) {
    try {
        GlobalPadAdapter().RemoveDevice(t, port, epoch);
    } catch (...) {
    }
}
extern "C" JNIEXPORT void JNICALL
Java_com_shadps4_android_runtime_input_NativePad_nativeFocusLost(JNIEnv *, jclass, jlong token) {
    try {
        GlobalPadAdapter().FocusLost(token);
    } catch (...) {
    }
}
extern "C" JNIEXPORT jlongArray JNICALL
Java_com_shadps4_android_runtime_input_NativePad_nativeDrainHaptics(JNIEnv *env, jclass,
                                                                    jlong token) {
    try {
        const auto commands = GlobalPadAdapter().DrainHaptics(token);
        std::vector<jlong> data;
        for (const auto &c : commands) {
            data.insert(data.end(),
                        {jlong(c.device.backend_id), jlong(c.device.connection_epoch),
                         jlong(c.small_motor * 255 + .5f), jlong(c.large_motor * 255 + .5f),
                         jlong(c.duration_ms), jlong(c.cancel)});
        }
        auto out = env->NewLongArray(data.size());
        if (out)
            env->SetLongArrayRegion(out, 0, data.size(), data.data());
        return out;
    } catch (...) {
        return nullptr;
    }
}
// Diagnostics invoke real HLE exports; no substitute pad implementation.
extern "C" JNIEXPORT jint JNICALL
Java_com_shadps4_android_runtime_input_NativePad_nativeOpenDefaultPad(JNIEnv *, jclass) {
    try {
        if (UserManagement.GetAllUsers().empty())
            UserSettings.Load();
        auto *real = UserManagement.GetUserByPlayerIndex(1);
        if (!real)
            return -1;
        UserManagement.LoginUser(real, 1);
        int result = scePadInit();
        if (result < 0)
            return result;
        return scePadOpen(real->user_id, ORBIS_PAD_PORT_TYPE_STANDARD, 0, nullptr);
    } catch (...) {
        return -1;
    }
}
extern "C" JNIEXPORT jlongArray JNICALL
Java_com_shadps4_android_runtime_input_NativePad_nativeReadPad(JNIEnv *env, jclass, jint handle) {
    try {
        OrbisPadData d{};
        const int result = scePadReadState(handle, &d);
        const jlong data[] = {
            result,         jlong(d.buttons),  d.leftStick.x,      d.leftStick.y,
            d.rightStick.x, d.rightStick.y,    d.analogButtons.l2, d.analogButtons.r2,
            d.connected,    jlong(d.timestamp)};
        auto out = env->NewLongArray(10);
        if (out)
            env->SetLongArrayRegion(out, 0, 10, data);
        return out;
    } catch (...) {
        return nullptr;
    }
}
extern "C" JNIEXPORT jint JNICALL Java_com_shadps4_android_runtime_input_NativePad_nativeVibratePad(
    JNIEnv *, jclass, jint handle, jint small, jint large) {
    try {
        if (small < 0 || small > 255 || large < 0 || large > 255)
            return -1;
        OrbisPadVibrationParam value{};
        value.smallMotor = u8(small);
        value.largeMotor = u8(large);
        return scePadSetVibration(handle, &value);
    } catch (...) {
        return -1;
    }
}
