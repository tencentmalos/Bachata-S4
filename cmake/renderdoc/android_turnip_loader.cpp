// SPDX-License-Identifier: MIT
// Replay-only adapter. Its adjacent directory contains the pinned Turnip and all
// four adrenotools hook libraries; neither the game DSO nor JNI is loaded here.
#include <adrenotools/driver.h>
#include <android/log.h>
#include <dlfcn.h>
#include <string>

extern "C" __attribute__((visibility("default"))) void* RenderDoc_LoadAndroidVulkan() {
    // Process-lived namespace/driver, just as in the production Android loader.
    static void* library = []() -> void* {
        Dl_info info{};
        if (!dladdr(reinterpret_cast<void*>(&RenderDoc_LoadAndroidVulkan), &info) || !info.dli_fname)
            return nullptr;
        std::string root{info.dli_fname};
        const auto slash = root.rfind('/');
        if (slash == std::string::npos) return nullptr;
        root.resize(slash + 1);
        auto* result = adrenotools_open_libvulkan(RTLD_NOW | RTLD_LOCAL, ADRENOTOOLS_DRIVER_CUSTOM,
            nullptr, root.c_str(), root.c_str(), "vulkan.ad07xx.so", nullptr, nullptr);
        __android_log_print(result ? ANDROID_LOG_INFO : ANDROID_LOG_ERROR, "RdocTurnip",
            "private Turnip loader %s: %s (no system fallback)", result ? "ready" : "failed", root.c_str());
        return result;
    }();
    return library;
}
