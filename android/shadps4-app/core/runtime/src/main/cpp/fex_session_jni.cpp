#include "video_core/renderer_vulkan/host_passes/spatial_upscale.h"
// SPDX-FileCopyrightText: Copyright 2026 shadPS4 Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

// JNI surface for the shadps4-app in-process FEX session.
//
// This is now a THIN adapter over Core::HostRuntime::SessionCore. All
// lifecycle, threading, generation and teardown logic lives in the backend-free
// SessionCore (src/core/host_runtime), which is unit-tested on the host with a
// FakeBackend. This file only:
//   * owns one process-global SessionCore bound to the real FexSessionBackend,
//   * marshals stable POD / copied strings across JNI (never a native/guest
//     pointer),
//   * catches every C++ exception at the boundary and turns it into a defined
//     error value, so an exception never crosses JNI.
//
// The session still runs a bounded x86-64 decrement loop (CPU-alive proof); it
// is NOT a real PS4 game (the Android host is not yet native -- see HN1/HN2 in
// docs/specs/android-native-host-v1.md).
//
// The old defects this replaces: a non-owner Stop dereferenced a raw CpuContext
// the owner could free concurrently (UAF); a Stop during preparation was
// dropped; two callers raced one std::thread::join; a guest fault was reported
// as exit 0. SessionCore fixes all four; this file cannot reintroduce them
// because it holds no raw runtime pointer and performs no join itself.

#include <jni.h>
#include <nlohmann/json.hpp>
#include "core/emulator_settings.h"
#include "core/host_runtime/guest_patch_format.h"
#include "core/libraries/move/move.h"
#include "video_core/renderer_vulkan/openxr/runtime.h"
#include "core/file_sys/fs.h"

#include <atomic>
#include <cmath>
#include <cstdio>
#include <limits>
#include <stdexcept>
#include <string>

#include <unistd.h>

#include <android/log.h>
#include <sys/system_properties.h>

#include "core/diagnostics/diagnostics_service.h"
#include "core/host_runtime/guest_save_dialog.h"
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

std::mutex error_driver_mutex;
Vulkan::DriverLease error_driver;
std::mutex dialog_mutex;
std::uint64_t dialog_generation{};
std::weak_ptr<Core::HostRuntime::GuestSaveDialog> dialog_weak;
std::shared_ptr<Core::HostRuntime::GuestSaveDialog>
CreateDialog(std::uint64_t generation) {
  auto dialog = std::make_shared<Core::HostRuntime::GuestSaveDialog>();
  std::lock_guard lock(dialog_mutex);
  dialog_generation = generation;
  dialog_weak = dialog;
  return dialog;
}
std::shared_ptr<Core::HostRuntime::GuestSaveDialog>
GetDialog(std::uint64_t generation) {
  if (!generation || Session().CurrentGeneration() != generation)
    return {};
  std::lock_guard lock(dialog_mutex);
  return dialog_generation == generation ? dialog_weak.lock() : nullptr;
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

// Bounded registration metadata from the same overlay stack used at launch.
extern "C" JNIEXPORT jobjectArray JNICALL
Java_com_shadps4_android_runtime_session_NativeFexSession_nativeInspectArchive(
    JNIEnv* env, jobject, jstring path) {
    if (!path) return nullptr;
    const char* value = env->GetStringUTFChars(path, nullptr);
    if (!value) return nullptr;
    std::string filename(value);
    env->ReleaseStringUTFChars(path, value);
    try {
        auto metadata = Core::FileSys::InspectArchiveInstall(filename);
        auto byte_array = env->FindClass("[B");
        if (!byte_array) return nullptr;
        auto result = env->NewObjectArray(2, byte_array, nullptr);
        env->DeleteLocalRef(byte_array);
        if (!result) return nullptr;
        const std::vector<u8>* parts[]{&metadata.param_sfo, &metadata.icon_png};
        for (int i = 0; i < 2; ++i) {
            auto bytes = env->NewByteArray(parts[i]->size());
            if (!bytes) return nullptr;
            env->SetByteArrayRegion(bytes, 0, parts[i]->size(),
                reinterpret_cast<const jbyte*>(parts[i]->data()));
            env->SetObjectArrayElement(result, i, bytes);
            env->DeleteLocalRef(bytes);
            if (env->ExceptionCheck()) return nullptr;
        }
        return result;
    } catch (const std::exception& e) {
        __android_log_print(ANDROID_LOG_ERROR, "GameArchive", "%s", e.what());
        return nullptr;
    } catch (...) { return nullptr; }
}

// Read through the same app0 overlay as GuestRuntime::Prepare, without starting a guest.
extern "C" JNIEXPORT jbyteArray JNICALL
Java_com_shadps4_android_runtime_session_NativeFexSession_nativeReadLaunchParamSfo(
    JNIEnv* env, jobject, jstring path) {
    if (!path) return nullptr;
    const char* value = env->GetStringUTFChars(path, nullptr);
    if (!value) return nullptr;
    std::filesystem::path executable(value);
    env->ReleaseStringUTFChars(path, value);
    try {
        Core::FileSys::MntPoints mount;
        mount.Mount(std::filesystem::canonical(Core::FileSys::IsZArchiveFile(executable)
                        ? executable : executable.parent_path()), "/app0", true);
        auto file = mount.Open("/app0/sce_sys/param.sfo");
        if (!file || !file->Size() || file->Size() > 1024 * 1024) return nullptr;
        std::vector<u8> data(file->Size());
        if (file->Read(data.data(), data.size()) != s64(data.size())) return nullptr;
        auto result = env->NewByteArray(data.size());
        if (result) env->SetByteArrayRegion(result, 0, data.size(),
                                          reinterpret_cast<const jbyte*>(data.data()));
        return result;
    } catch (const std::exception& e) {
        __android_log_print(ANDROID_LOG_ERROR, "DisplayMode", "%s", e.what());
        return nullptr;
    } catch (...) { return nullptr; }
}

// Dispatches a graphics/perf debugging toolkit command (spec §3.1), e.g.
// "debug_status" or "overlay status", through the process command registry. The
// same registry is bound to the Android dumpsys bridge, so `adb shell dumpsys`
// and this JNI entry share one typed backend. Status commands read a
// non-blocking DiagnosticsHub snapshot; this never waits on the session mutex,
// VM drain, or GPU fence. An empty/null command returns the help text.
extern "C" JNIEXPORT jstring JNICALL
Java_com_shadps4_android_runtime_session_NativeFexSession_nativeDebugCommand(JNIEnv* env, jclass,
                                                                            jstring command) {
#if !defined(SHADPS4_ANDROID_DEBUG_COMMANDS)
    return env->NewStringUTF("status: disabled_in_release\n");
#else
    try {
        std::string request;
        if (command != nullptr) {
            const char* c = env->GetStringUTFChars(command, nullptr);
            if (c != nullptr) {
                request = c;
                env->ReleaseStringUTFChars(command, c);
            }
        }
        return env->NewStringUTF(Core::Diagnostics::HandleDebugCommand(request).c_str());
    } catch (...) {
        return env->NewStringUTF("debug-command-error");
    }
#endif
}

// Starts a session. Returns the new generation (>0), or 0 if a session is
// already running or the owner thread could not be spawned.
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

// The installed-content path uses the same SessionCore as CPU smoke. Copy all
// JNI strings before handing immutable parameters to its owner thread.
extern "C" JNIEXPORT jlong JNICALL
Java_com_shadps4_android_runtime_session_NativeFexSession_nativeStartExecutable(
    JNIEnv* env, jclass, jstring content_id, jstring executable_path) {
    try {
        auto copy = [&](jstring value) {
            if (!value) throw std::invalid_argument("missing production path/identity");
            const char* chars = env->GetStringUTFChars(value, nullptr);
            if (!chars) throw std::runtime_error("JNI string unavailable");
            std::string result;
            try { result = chars; } catch (...) { env->ReleaseStringUTFChars(value, chars); throw; }
            env->ReleaseStringUTFChars(value, chars);
            return result;
        };
        SessionParams params;
        params.content_id = copy(content_id);
        params.executable_path = copy(executable_path);
    params.create_save_dialog = CreateDialog;
    if (params.executable_path.empty())
      return 0;
        return static_cast<jlong>(Session().Start(params));
    } catch (const std::exception& e) {
        __android_log_print(ANDROID_LOG_ERROR, kTag, "nativeStartExecutable: %s", e.what());
        return 0;
    } catch (...) { return 0; }
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

// Waits for a terminal. Returns the RunOutcome ordinal, or -1 on timeout
// (session still owned; NOT idle).
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

// Keep the loader in the native host DSO; JNI owns neither a second Vulkan
// dispatcher nor an adrenotools namespace.
#include "video_core/renderer_vulkan/vk_driver.h"
extern "C" JNIEXPORT jstring JNICALL
Java_com_shadps4_android_runtime_session_AndroidTurnip_nativeSystemProperty(JNIEnv *env, jobject, jstring name) {
  const char *chars = name ? env->GetStringUTFChars(name, nullptr) : nullptr;
  if (!chars)
    return env->NewStringUTF("");
  char value[PROP_VALUE_MAX]{};
  __system_property_get(chars, value);
  env->ReleaseStringUTFChars(name, chars);
  return env->NewStringUTF(value);
}

extern "C" JNIEXPORT jstring JNICALL
Java_com_shadps4_android_runtime_session_AndroidTurnip_nativeLoad(
    JNIEnv *env, jobject, jstring hooks, jstring files) {
  try {
    auto copy = [&](jstring value) {
      if (!value)
        throw std::invalid_argument("Missing Turnip directory");
      const char *chars = env->GetStringUTFChars(value, nullptr);
      if (!chars)
        throw std::runtime_error("Cannot read Turnip directory");
      std::string result;
      try {
        result = chars;
      } catch (...) {
        env->ReleaseStringUTFChars(value, chars);
        throw;
      }
      env->ReleaseStringUTFChars(value, chars);
      return result;
    };
    const auto driver = Vulkan::LoadAndroidTurnip(copy(hooks), copy(files));
    return env->NewStringUTF(driver->identity.c_str());
  } catch (const std::exception &e) {
    if (!env->ExceptionCheck())
      env->ThrowNew(env->FindClass("java/lang/IllegalStateException"),
                    e.what());
    return nullptr;
  } catch (...) {
    if (!env->ExceptionCheck())
      env->ThrowNew(env->FindClass("java/lang/IllegalStateException"),
                    "Turnip native failure");
    return nullptr;
  }
}

#include "frontend/android_window.h"
#include "video_core/renderer_vulkan/vk_instance.h"
#include "video_core/renderer_vulkan/vk_swapchain.h"
#include <android/native_window_jni.h>

namespace {
std::mutex rendered_surface_mutex;
std::uint64_t rendered_surface_generation{};
std::weak_ptr<Frontend::AndroidWindow> rendered_window;
std::uint64_t pending_surface_generation{};
std::shared_ptr<ANativeWindow> pending_surface;
bool pending_surface_update{};
} // namespace
extern "C" JNIEXPORT jstring JNICALL
Java_com_shadps4_android_runtime_session_AndroidTurnip_nativeInspectSurface(
    JNIEnv *env, jobject, jstring hooks, jstring files, jobject surface) {
  try {
    auto copy = [&](jstring value) {
      if (!value)
        throw std::invalid_argument("Missing Turnip directory");
      const char *chars = env->GetStringUTFChars(value, nullptr);
      if (!chars)
        throw std::runtime_error("Cannot read Turnip directory");
      std::string result;
      try {
        result = chars;
      } catch (...) {
        env->ReleaseStringUTFChars(value, chars);
        throw;
      }
      env->ReleaseStringUTFChars(value, chars);
      return result;
    };
    if (!surface)
      throw std::invalid_argument("Missing Android Surface");
    auto native =
        std::unique_ptr<ANativeWindow, decltype(&ANativeWindow_release)>(
            ANativeWindow_fromSurface(env, surface), ANativeWindow_release);
    if (!native)
      throw std::runtime_error("Android Surface is unavailable");
    static std::atomic<u64> next_surface{1};
    auto window = std::make_shared<Frontend::AndroidWindow>(
        native.get(), next_surface.fetch_add(1));
    auto driver = Vulkan::LoadAndroidTurnip(copy(hooks), copy(files));
    Vulkan::Instance instance(*window, -1, false, false, driver);
    Vulkan::Swapchain swapchain(instance, *window);
    const auto detail = driver->identity +
                        "surface=" + std::to_string(swapchain.GetWidth()) +
                        "x" + std::to_string(swapchain.GetHeight()) +
                        " images=" + std::to_string(swapchain.GetImageCount());
    swapchain.RequestStop();
    return env->NewStringUTF(detail.c_str());
  } catch (const std::exception &e) {
    if (!env->ExceptionCheck())
      env->ThrowNew(env->FindClass("java/lang/IllegalStateException"),
                    e.what());
    return nullptr;
  } catch (...) {
    if (!env->ExceptionCheck())
      env->ThrowNew(env->FindClass("java/lang/IllegalStateException"),
                    "Turnip Surface failure");
    return nullptr;
  }
}

extern "C" JNIEXPORT jlong JNICALL
Java_com_shadps4_android_runtime_session_NativeFexSession_nativeStartRenderedExecutable(
    JNIEnv *env, jclass, jstring content_id, jstring executable_path,
    jobject surface, jstring hook_directory, jstring driver_directory) {
  try {
    Vulkan::OpenXr::HideError(); // Drain UI before any new guest renderer can acquire Vulkan.
    auto copy = [&](jstring value) {
      if (!value)
        throw std::invalid_argument("Missing rendered-session argument");
      const char *chars = env->GetStringUTFChars(value, nullptr);
      if (!chars)
        throw std::runtime_error("JNI string unavailable");
      std::string result;
      try {
        result = chars;
      } catch (...) {
        env->ReleaseStringUTFChars(value, chars);
        throw;
      }
      env->ReleaseStringUTFChars(value, chars);
      return result;
    };
    if (!surface)
      throw std::invalid_argument("Missing Android Surface");
    std::shared_ptr<ANativeWindow> native(
        ANativeWindow_fromSurface(env, surface), ANativeWindow_release);
    if (!native)
      throw std::runtime_error("Android Surface is unavailable");
    SessionParams params;
    params.content_id = copy(content_id);
    params.executable_path = copy(executable_path);
    params.create_save_dialog = CreateDialog;
    params.requires_platform_ready = true;
    params.create_window = [native](std::uint64_t generation) {
      auto window = std::make_shared<Frontend::AndroidWindow>(native.get(), generation);
      std::lock_guard lock(rendered_surface_mutex);
      rendered_surface_generation = generation;
      rendered_window = window;
      if (pending_surface_update && pending_surface_generation == generation) {
        window->UpdateSurface(pending_surface.get());
        pending_surface.reset();
        pending_surface_update = false;
      }
      return window;
    };
    char driver_property[PROP_VALUE_MAX]{};
    __system_property_get("debug.shadps4.vulkan_driver", driver_property);
    const std::string driver_kind = driver_property;
    if (!driver_kind.empty() && driver_kind != "turnip" && driver_kind != "turnip-mainline" && driver_kind != "system")
      throw std::invalid_argument("Unknown debug.shadps4.vulkan_driver (turnip/turnip-mainline/system)");
    params.load_graphics_driver = [hooks = copy(hook_directory),
                                   files = copy(driver_directory),
                                   system = driver_kind == "system"] {
      { std::scoped_lock lock(error_driver_mutex); error_driver.reset(); }
      auto driver = system ? Vulkan::LoadAndroidSystemDriver() : Vulkan::LoadAndroidTurnip(hooks, files);
      { std::scoped_lock lock(error_driver_mutex); error_driver = driver; }
      __android_log_print(ANDROID_LOG_INFO, kTag, "Selected Vulkan driver: %s", driver->identity.c_str());
      return driver;
    };
    return static_cast<jlong>(Session().Start(params));
  } catch (const std::exception &e) {
    __android_log_print(ANDROID_LOG_ERROR, kTag,
                        "nativeStartRenderedExecutable: %s", e.what());
    return 0;
  } catch (...) {
    return 0;
  }
}

extern "C" JNIEXPORT jboolean JNICALL
Java_com_shadps4_android_runtime_session_NativeFexSession_nativeUpdateRenderedSurface(
    JNIEnv* env, jclass, jlong generation, jobject surface) {
  try {
    if (generation <= 0 || Session().CurrentGeneration() != static_cast<std::uint64_t>(generation))
      return JNI_FALSE;
    std::shared_ptr<ANativeWindow> native;
    if (surface) {
      auto* window = ANativeWindow_fromSurface(env, surface);
      if (!window) return JNI_FALSE;
      native = std::shared_ptr<ANativeWindow>(window, ANativeWindow_release);
    }
    std::lock_guard lock(rendered_surface_mutex);
    if (rendered_surface_generation == static_cast<std::uint64_t>(generation)) {
      if (auto window = rendered_window.lock()) {
        window->UpdateSurface(native.get());
        return JNI_TRUE;
      }
    }
    pending_surface_generation = static_cast<std::uint64_t>(generation);
    pending_surface = std::move(native);
    pending_surface_update = true;
    return JNI_TRUE;
  } catch (...) {
    return JNI_FALSE;
  }
}
extern "C" JNIEXPORT jboolean JNICALL
Java_com_shadps4_android_runtime_session_NativeFexSession_nativePlatformReady(
    JNIEnv *, jclass, jlong generation) {
  try {
    return Session().PlatformReady(static_cast<std::uint64_t>(generation));
  } catch (...) {
    return false;
  }
}

extern "C" JNIEXPORT jstring JNICALL
Java_com_shadps4_android_runtime_session_NativeFexSession_nativeSaveDialogSnapshot(
    JNIEnv *env, jclass, jlong generation) {
  try {
    auto dialog = GetDialog(generation);
    if (!dialog)
      return nullptr;
    auto text = dialog->SnapshotJson();
    return text.empty() ? nullptr : env->NewStringUTF(text.c_str());
  } catch (...) {
    return nullptr;
  }
}
extern "C" JNIEXPORT jboolean JNICALL
Java_com_shadps4_android_runtime_session_NativeFexSession_nativeSaveDialogRespond(
    JNIEnv *, jclass, jlong generation, jlong request, jint action,
    jint selection) {
  try {
    auto dialog = GetDialog(generation);
    return dialog && dialog->Respond(request, action, selection);
  } catch (...) {
    return false;
  }
}

extern "C" JNIEXPORT void JNICALL
Java_com_shadps4_android_runtime_session_NativeFexSession_nativeSetGuestShadingQuality(
    JNIEnv*, jclass, jint quality) {
    try { EmulatorSettings.SetGuestShadingQuality(static_cast<u32>(quality)); }
    catch (...) { /* Never unwind across JNI. The default remains full rate. */ }
}

extern "C" JNIEXPORT void JNICALL
Java_com_shadps4_android_runtime_session_NativeFexSession_nativeSetSilentDialogs(
    JNIEnv*, jobject, jboolean enabled) {
    EmulatorSettings.SetGuestDialogsSilent(enabled == JNI_TRUE);
}

extern "C" JNIEXPORT void JNICALL
Java_com_shadps4_android_runtime_session_NativeFexSession_nativeSetGuestPatches(
    JNIEnv* env, jobject, jobjectArray names) {
    std::vector<std::string> list;
    try {
        const jsize count = names ? env->GetArrayLength(names) : 0;
        for (jsize i = 0; i < count; ++i) {
            auto* item = static_cast<jstring>(env->GetObjectArrayElement(names, i));
            if (!item) continue;
            if (const char* chars = env->GetStringUTFChars(item, nullptr)) {
                list.emplace_back(chars);
                env->ReleaseStringUTFChars(item, chars);
            }
            env->DeleteLocalRef(item);
        }
    } catch (...) {
        list.clear(); // Never unwind across JNI; an unreadable selection installs nothing.
    }
    EmulatorSettings.SetGuestPatches(list);
}

extern "C" JNIEXPORT jstring JNICALL
Java_com_shadps4_android_runtime_session_NativeFexSession_nativeListGuestPatches(
    JNIEnv* env, jobject, jstring directory, jstring title) {
    const auto read = [env](jstring value) {
        std::string out;
        if (const char* chars = value ? env->GetStringUTFChars(value, nullptr) : nullptr) {
            out = chars;
            env->ReleaseStringUTFChars(value, chars);
        }
        return out;
    };
    nlohmann::json result{{"packages", nlohmann::json::array()}, {"skipped", nlohmann::json::array()}};
    try {
        const auto listing = Core::GuestPatch::ListPackages(read(directory), read(title));
        for (const auto& p : listing.packages) {
            result["packages"].push_back({{"name", p.name}, {"id", p.id}, {"label", p.display_name},
                                          {"description", p.description}, {"module", p.module},
                                          {"module_sha256", p.module_sha256},
                                          {"conflicts", p.conflicts}});
        }
        result["skipped"] = listing.skipped;
    } catch (const std::exception& e) {
        result["skipped"].push_back(e.what());
    }
    // ASCII with escapes suits NewStringUTF's modified UTF-8; invalid UTF-8 from a file
    // name is replaced rather than thrown across JNI.
    return env->NewStringUTF(
        result.dump(-1, ' ', true, nlohmann::json::error_handler_t::replace).c_str());
}

extern "C" JNIEXPORT void JNICALL
Java_com_shadps4_android_runtime_session_NativeFexSession_nativeSetXrRendering(
    JNIEnv*, jobject, jint filter, jint foveation, jint level, jint sharpness, jint output_resolution) {
    Vulkan::HostPasses::SetSpatialOptions({
        .filter=static_cast<Vulkan::HostPasses::SpatialFilter>(filter),
        .foveation=static_cast<spatial::foveation::Mode>(foveation),
        .level=static_cast<spatial::foveation::Level>(level),.sharpness=static_cast<u32>(sharpness),
        .output_resolution=static_cast<Vulkan::HostPasses::XrOutputResolution>(output_resolution)});
}

extern "C" JNIEXPORT void JNICALL
Java_com_shadps4_android_runtime_session_NativeFexSession_nativeSetXrStatus(
    JNIEnv*, jobject, jint layout, jboolean psvr) {
    Vulkan::OpenXr::ConfigureStatus(layout, psvr == JNI_TRUE);
}

extern "C" JNIEXPORT jintArray JNICALL
Java_com_shadps4_android_runtime_session_NativeFexSession_nativeQueryXrOutputExtents(
    JNIEnv* env, jobject, jobject activity, jstring hooks, jstring files) {
    try {
        char selected[PROP_VALUE_MAX]{};
        __system_property_get("debug.shadps4.vulkan_driver", selected);
        if (std::string_view(selected) == "system") return nullptr;
        const auto copy = [env](jstring value) {
            if (!value) throw std::invalid_argument("Missing driver path");
            const char* chars = env->GetStringUTFChars(value, nullptr);
            if (!chars) throw std::runtime_error("Cannot read driver path");
            std::string result;
            try { result = chars; }
            catch (...) { env->ReleaseStringUTFChars(value, chars); throw; }
            env->ReleaseStringUTFChars(value, chars);
            return result;
        };
        auto driver = Vulkan::LoadAndroidTurnip(copy(hooks), copy(files));
        const auto extents = Vulkan::OpenXr::Runtime::ProbeOutputExtents(env, activity, driver);
        jint values[6];
        for (size_t i = 0; i < extents.size(); ++i) {
            values[i * 2] = static_cast<jint>(extents[i].width);
            values[i * 2 + 1] = static_cast<jint>(extents[i].height);
        }
        const auto result = env->NewIntArray(6);
        if (result) env->SetIntArrayRegion(result, 0, 6, values);
        return result;
    } catch (const std::exception& e) {
        __android_log_print(ANDROID_LOG_WARN, kTag, "XR output query: %s", e.what());
        return nullptr;
    } catch (...) { return nullptr; }
}

extern "C" JNIEXPORT void JNICALL
Java_com_shadps4_android_runtime_session_NativeFexSession_nativeSetXrSwapMoveHands(
    JNIEnv*, jobject, jboolean swap) {
    Libraries::Move::SetXrSwapHands(swap == JNI_TRUE);
}

extern "C" JNIEXPORT void JNICALL
Java_com_shadps4_android_runtime_session_NativeFexSession_nativeSetMsaaDisabled(
    JNIEnv*, jobject, jboolean disabled) {
    EmulatorSettings.SetMsaaDisabled(disabled == JNI_TRUE);
}

extern "C" JNIEXPORT void JNICALL
Java_com_shadps4_android_runtime_session_NativeFexSession_nativeSetPipelineCacheEnabled(
    JNIEnv*, jobject, jboolean enabled) {
    EmulatorSettings.SetPipelineCacheEnabled(enabled == JNI_TRUE);
}

extern "C" JNIEXPORT void JNICALL
Java_com_shadps4_android_runtime_session_NativeFexSession_nativeSetDriverPipelineCacheEnabled(
    JNIEnv*, jobject, jboolean enabled) {
    EmulatorSettings.SetDriverPipelineCache(enabled == JNI_TRUE);
}

extern "C" JNIEXPORT void JNICALL
Java_com_shadps4_android_runtime_session_NativeFexSession_nativeSetPipelineCompileMode(
    JNIEnv*, jobject, jint mode) {
    try {
        EmulatorSettings.SetPipelineCompileMode(mode == 2   ? "async_graphics_skip"
                                                : mode == 1 ? "async_accurate"
                                                            : "sync");
    }
    catch (...) { /* Never unwind across JNI. */ }
}

extern "C" JNIEXPORT void JNICALL
Java_com_shadps4_android_runtime_session_NativeFexSession_nativeSetCommandRecorder(
    JNIEnv*, jobject, jboolean enabled) {
    EmulatorSettings.SetCommandRecorder(enabled == JNI_TRUE);
}

extern "C" JNIEXPORT void JNICALL
Java_com_shadps4_android_runtime_session_NativeFexSession_nativeSetDirectMemoryAccess(
    JNIEnv *, jobject, jboolean enabled) {
  EmulatorSettings.SetDirectMemoryAccessEnabled(enabled == JNI_TRUE);
}

extern "C" JNIEXPORT void JNICALL
Java_com_shadps4_android_runtime_session_NativeFexSession_nativeSetTextureQuality(
    JNIEnv*, jclass, jint quality) {
    try { EmulatorSettings.SetTextureQuality(static_cast<u32>(quality)); }
    catch (...) { /* Never unwind across JNI. */ }
}

extern "C" JNIEXPORT void JNICALL
Java_com_shadps4_android_runtime_session_NativeFexSession_nativeSetInternalScalePercent(
    JNIEnv*, jclass, jfloat percent) {
    try { EmulatorSettings.SetInternalScalePercent(percent); }
    catch (...) { /* Never unwind across JNI. */ }
}

extern "C" JNIEXPORT void JNICALL
Java_com_shadps4_android_runtime_session_NativeFexSession_nativeSetConsoleLanguage(
    JNIEnv*, jclass, jint language) {
    if (language < 0 || language > 30) return;
    try { EmulatorSettings.SetConsoleLanguage(language); }
    catch (...) { /* Never unwind across JNI. */ }
}

extern "C" JNIEXPORT jstring JNICALL
Java_com_shadps4_android_runtime_session_NativeFexSession_nativeShowXrError(JNIEnv* env, jclass, jlong token, jstring detail) {
    try {
        if (Session().CurrentGeneration()) throw std::runtime_error("Guest session has not drained");
        if (!detail) throw std::runtime_error("Missing error detail");
        const char* text = env->GetStringUTFChars(detail, nullptr);
        if (!text) return nullptr; // pending JNI exception
        std::string copy;
        try { copy = text; } catch (...) { env->ReleaseStringUTFChars(detail, text); throw; }
        env->ReleaseStringUTFChars(detail, text);
        Vulkan::DriverLease driver;
        { std::scoped_lock lock(error_driver_mutex); driver = error_driver; }
        Vulkan::OpenXr::ShowError(std::move(copy), std::move(driver), token);
        return nullptr;
    } catch (const std::exception& e) {
        __android_log_print(ANDROID_LOG_ERROR, kTag, "XR error presentation: %s", e.what());
        return env->NewStringUTF(e.what());
    }
}
extern "C" JNIEXPORT void JNICALL
Java_com_shadps4_android_runtime_session_NativeFexSession_nativeHideXrError(JNIEnv*, jclass, jlong token) {
    Vulkan::OpenXr::HideError(token);
}
extern "C" JNIEXPORT jint JNICALL
Java_com_shadps4_android_runtime_session_NativeFexSession_nativePollXrErrorAction(JNIEnv*, jclass, jlong token) {
    return Vulkan::OpenXr::PollErrorAction(token);
}
extern "C" JNIEXPORT jboolean JNICALL
Java_com_shadps4_android_runtime_session_NativeFexSession_nativeXrErrorKey(JNIEnv*, jclass, jint key, jboolean down) {
    return Vulkan::OpenXr::ErrorKey(key, down);
}

extern "C" JNIEXPORT jstring JNICALL
Java_com_shadps4_android_runtime_session_NativeFexSession_nativeXrErrorStatus(JNIEnv* env, jclass) {
    return env->NewStringUTF(Vulkan::OpenXr::ErrorStatus().c_str());
}
