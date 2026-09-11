// SPDX-FileCopyrightText: Copyright 2026 shadPS4 Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

// JNI surface for the shadps4-app in-process FEX session.
//
// This is now a THIN adapter over Core::HostRuntime::SessionCore. All lifecycle,
// threading, generation and teardown logic lives in the backend-free SessionCore
// (src/core/host_runtime), which is unit-tested on the host with a FakeBackend.
// This file only:
//   * owns one process-global SessionCore bound to the real FexSessionBackend,
//   * marshals stable POD / copied strings across JNI (never a native/guest
//     pointer),
//   * catches every C++ exception at the boundary and turns it into a defined
//     error value, so an exception never crosses JNI.
//
// The session still runs a bounded x86-64 decrement loop (CPU-alive proof); it is
// NOT a real PS4 game (the Android host is not yet native -- see HN1/HN2 in
// docs/specs/android-native-host-v1.md).
//
// The old defects this replaces: a non-owner Stop dereferenced a raw CpuContext
// the owner could free concurrently (UAF); a Stop during preparation was dropped;
// two callers raced one std::thread::join; a guest fault was reported as exit 0.
// SessionCore fixes all four; this file cannot reintroduce them because it holds
// no raw runtime pointer and performs no join itself.

#include <jni.h>

#include <cstdio>
#include <string>

#include <unistd.h>

#include <android/log.h>

#include "core/host_runtime/session_backend_fex.h"
#include "core/host_runtime/session_core.h"

namespace {

using Core::HostRuntime::FexSessionBackend;
using Core::HostRuntime::Phase;
using Core::HostRuntime::SessionCore;
using Core::HostRuntime::SessionParams;
using Core::HostRuntime::StopResult;
using Core::HostRuntime::Terminal;
using Core::HostRuntime::WaitPhaseResult;

constexpr const char* kTag = "FexSession";

FexSessionBackend& Backend() {
    static FexSessionBackend backend;
    return backend;
}

SessionCore& Session() {
    static SessionCore core{Backend()};
    return core;
}

std::uint64_t MsToNs(jlong ms) {
    return ms > 0 ? static_cast<std::uint64_t>(ms) * 1'000'000ull : 0ull;
}

// Phase ordinals the Kotlin side mirrors. Kept in one place; must match
// NativeFexSession.PHASE_* on the Kotlin side.
jint PhaseOrdinal(Phase phase) {
    return static_cast<jint>(phase);
}

}  // namespace

extern "C" JNIEXPORT jstring JNICALL
Java_com_shadps4_android_runtime_session_NativeFexSession_nativeIdentity(JNIEnv* env, jclass) {
    try {
        char msg[128];
        std::snprintf(msg, sizeof(msg), "page_size=%ld pid=%d uid=%d",
                      sysconf(_SC_PAGESIZE), static_cast<int>(getpid()),
                      static_cast<int>(getuid()));
        return env->NewStringUTF(msg);
    } catch (...) {
        return env->NewStringUTF("identity-error");
    }
}

// Starts a session. Returns the new generation (>0), or 0 if a session is already
// running or the owner thread could not be spawned.
extern "C" JNIEXPORT jlong JNICALL
Java_com_shadps4_android_runtime_session_NativeFexSession_nativeStart(JNIEnv* env, jclass,
                                                                      jstring content_id,
                                                                      jlong iterations) {
    try {
        SessionParams params;
        if (content_id != nullptr) {
            const char* c = env->GetStringUTFChars(content_id, nullptr);
            if (c != nullptr) {
                params.content_id = c;
                env->ReleaseStringUTFChars(content_id, c);
            }
        }
        params.iterations = iterations > 0 ? static_cast<std::uint64_t>(iterations) : 0;
        return static_cast<jlong>(Session().Start(params));
    } catch (const std::exception& e) {
        __android_log_print(ANDROID_LOG_ERROR, kTag, "nativeStart threw: %s", e.what());
        return 0;
    } catch (...) {
        __android_log_print(ANDROID_LOG_ERROR, kTag, "nativeStart threw");
        return 0;
    }
}

// Requests a stop of `generation`. Returns a StopResult ordinal.
extern "C" JNIEXPORT jint JNICALL
Java_com_shadps4_android_runtime_session_NativeFexSession_nativeRequestStop(JNIEnv*, jclass,
                                                                           jlong generation,
                                                                           jlong timeout_ms) {
    try {
        const auto r = Session().RequestStop(static_cast<std::uint64_t>(generation), MsToNs(timeout_ms));
        return static_cast<jint>(r);
    } catch (...) {
        return static_cast<jint>(StopResult::Error);
    }
}

// Waits for `generation` to reach `target_phase` (or later). Returns a
// WaitPhaseResult ordinal.
extern "C" JNIEXPORT jint JNICALL
Java_com_shadps4_android_runtime_session_NativeFexSession_nativeWaitPhase(JNIEnv*, jclass,
                                                                         jlong generation,
                                                                         jint target_phase,
                                                                         jlong deadline_ms) {
    try {
        const auto r = Session().WaitPhase(static_cast<std::uint64_t>(generation),
                                        static_cast<Phase>(target_phase), MsToNs(deadline_ms));
        return static_cast<jint>(r);
    } catch (...) {
        return static_cast<jint>(WaitPhaseResult::Timeout);
    }
}

// Waits for a terminal. Returns the RunOutcome ordinal, or -1 on timeout (session
// still owned; NOT idle).
extern "C" JNIEXPORT jint JNICALL
Java_com_shadps4_android_runtime_session_NativeFexSession_nativeWaitTerminal(JNIEnv*, jclass,
                                                                            jlong generation,
                                                                            jlong deadline_ms) {
    try {
        Terminal t;
        if (!Session().WaitTerminal(static_cast<std::uint64_t>(generation), MsToNs(deadline_ms), t))
            return -1;
        return static_cast<jint>(t.outcome);
    } catch (...) {
        return -1;
    }
}

extern "C" JNIEXPORT jint JNICALL
Java_com_shadps4_android_runtime_session_NativeFexSession_nativeTerminalErrorCategory(JNIEnv*,
                                                                                     jclass,
                                                                                     jlong gen) {
    try {
        Terminal t;
        if (!Session().TryGetTerminal(static_cast<std::uint64_t>(gen), t))
            return 0;
        return static_cast<jint>(t.error_category);
    } catch (...) {
        return 0;
    }
}

extern "C" JNIEXPORT jstring JNICALL
Java_com_shadps4_android_runtime_session_NativeFexSession_nativeTerminalDetail(JNIEnv* env, jclass,
                                                                              jlong gen) {
    try {
        Terminal t;
        if (!Session().TryGetTerminal(static_cast<std::uint64_t>(gen), t) || t.detail.empty())
            return nullptr;
        return env->NewStringUTF(t.detail.c_str());
    } catch (...) {
        return nullptr;
    }
}

extern "C" JNIEXPORT jlong JNICALL
Java_com_shadps4_android_runtime_session_NativeFexSession_nativeCurrentGeneration(JNIEnv*, jclass) {
    try {
        return static_cast<jlong>(Session().CurrentGeneration());
    } catch (...) {
        return 0;
    }
}

extern "C" JNIEXPORT jint JNICALL
Java_com_shadps4_android_runtime_session_NativeFexSession_nativePhase(JNIEnv*, jclass, jlong gen) {
    try {
        return PhaseOrdinal(Session().QueryPhase(static_cast<std::uint64_t>(gen)));
    } catch (...) {
        return PhaseOrdinal(Phase::Idle);
    }
}
