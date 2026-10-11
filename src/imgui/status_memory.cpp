// SPDX-FileCopyrightText: Copyright 2026 shadPS4 Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later
#include "imgui/status_memory.h"
#include "common/thread.h"
#ifdef __ANDROID__
#include "spatial/platform/android/JniHelper.h"
#endif

namespace ImGui {
namespace {
#ifdef __ANDROID__
void ReadAndroidMemory(StatusMemorySnapshot& out) {
    auto* env = spatial::platform::JniHelper::getJniEnv();
    const auto context = spatial::platform::JniHelper::getActivityContext();
    if (!env || !context || env->ExceptionCheck()) return;
    if (env->PushLocalFrame(16) != JNI_OK) {
        env->ExceptionClear();
        return;
    }
    const auto failed = [&] {
        if (!env->ExceptionCheck()) return false;
        env->ExceptionClear();
        return true;
    };
    // Use the same public API as the old Compose System RAM. Do not substitute a kernel
    // estimate or assume OEM implementations use any particular /proc formula.
    [&] {
        const auto context_class = env->FindClass("android/content/Context");
        if (failed() || !context_class) return;
        const auto get_service = env->GetMethodID(context_class, "getSystemService", "(Ljava/lang/String;)Ljava/lang/Object;");
        if (failed() || !get_service) return;
        const auto name = env->NewStringUTF("activity");
        if (failed() || !name) return;
        const auto manager = env->CallObjectMethod(context, get_service, name);
        if (failed() || !manager) return;
        const auto info_class = env->FindClass("android/app/ActivityManager$MemoryInfo");
        if (failed() || !info_class) return;
        const auto ctor = env->GetMethodID(info_class, "<init>", "()V");
        if (failed() || !ctor) return;
        const auto info = env->NewObject(info_class, ctor);
        if (failed() || !info) return;
        const auto manager_class = env->GetObjectClass(manager);
        if (failed() || !manager_class) return;
        const auto get_info = env->GetMethodID(manager_class, "getMemoryInfo", "(Landroid/app/ActivityManager$MemoryInfo;)V");
        if (failed() || !get_info) return;
        env->CallVoidMethod(manager, get_info, info);
        if (failed()) return;
        const auto total_field = env->GetFieldID(info_class, "totalMem", "J");
        if (failed() || !total_field) return;
        const auto available_field = env->GetFieldID(info_class, "availMem", "J");
        if (failed() || !available_field) return;
        const auto total = env->GetLongField(info, total_field);
        const auto available = env->GetLongField(info, available_field);
        if (failed() || total <= 0 || available < 0 || available > total) return;
        out.system_total = total;
        out.system_available = available;
        out.system_used = total - available;
    }();
    env->PopLocalFrame(nullptr);
}
#endif
} // namespace

#ifdef __ANDROID__
StatusMemorySampler::StatusMemorySampler() : worker([this](std::stop_token stop) {
    Common::SetCurrentThreadName("shadPS4:Memory");
    while (!stop.stop_requested()) {
        StatusMemorySnapshot next;
        ReadAndroidMemory(next);
        {
            std::lock_guard lock(mutex);
            latest = next;
        }
        Common::StoppableTimedWait(stop, std::chrono::seconds(2));
    }
}) {}
#else
StatusMemorySampler::StatusMemorySampler() = default;
#endif
} // namespace ImGui
