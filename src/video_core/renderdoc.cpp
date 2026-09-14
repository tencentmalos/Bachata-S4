// SPDX-FileCopyrightText: Copyright 2024-2026 shadPS4 Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#include "common/logging/formatter.h"
#include "core/emulator_settings.h"
#include "video_core/renderdoc.h"
#include "video_core/renderdoc_capture.h"

#include <atomic>
#include <chrono>
#include <renderdoc_app.h>

#ifdef _WIN32
#include <windows.h>
#else
#include <dlfcn.h>
#endif

#include <filesystem>

namespace VideoCore {

enum class CaptureState {
    Idle,
    Triggered,
    InProgress,
};
// Written from the input/UI thread (TriggerCapture) and the GPU thread
// (StartCapture/EndCapture); a plain static here was a genuine cross-thread data
// race. The transitions are a tiny state machine, so each is done with a
// compare-exchange: every RenderDoc frame-capture call fires exactly once per
// armed request even if two callers race.
static std::atomic<CaptureState> capture_state{CaptureState::Idle};
static std::atomic<u32> screenshot_game_only_count{0};
static std::atomic<u32> screenshot_with_overlays_count{0};

RENDERDOC_API_1_6_0* rdoc_api{};

// Runs the RENDERDOC_GetAPI handshake against an obtained module handle and
// publishes rdoc_api on success. Fail-safe: a missing symbol or a rejected API
// version leaves rdoc_api null and logs, rather than aborting the process. The
// old code asserted ret == 1, which turned a benign version mismatch on a user's
// device into a hard crash.
static void ResolveRenderDocApi(void* mod) {
    if (!mod) {
        return;
    }
#ifdef _WIN32
    const auto get_api = reinterpret_cast<pRENDERDOC_GetAPI>(
        GetProcAddress(static_cast<HMODULE>(mod), "RENDERDOC_GetAPI"));
#else
    const auto get_api = reinterpret_cast<pRENDERDOC_GetAPI>(dlsym(mod, "RENDERDOC_GetAPI"));
#endif
    if (!get_api) {
        LOG_ERROR(Render, "RenderDoc module found but RENDERDOC_GetAPI is missing");
        return;
    }
    RENDERDOC_API_1_6_0* api{};
    const s32 ret = get_api(eRENDERDOC_API_Version_1_6_0, reinterpret_cast<void**>(&api));
    if (ret != 1 || api == nullptr) {
        LOG_ERROR(Render, "RenderDoc GetAPI(1.6.0) rejected: ret={}", ret);
        return;
    }
    rdoc_api = api;
}

void LoadRenderDoc() {
#ifdef _WIN32
    // If the RenderDoc GUI launched us, renderdoc.dll is already resident.
    HMODULE mod = GetModuleHandleA("renderdoc.dll");
    if (!mod && EmulatorSettings.IsRenderdocEnabled()) {
        // If enabled in config, try to load RDoc runtime in offline mode
        HKEY h_reg_key;
        LONG result = RegOpenKeyExW(HKEY_LOCAL_MACHINE,
                                    L"SOFTWARE\\Classes\\RenderDoc.RDCCapture.1\\DefaultIcon\\", 0,
                                    KEY_READ, &h_reg_key);
        if (result != ERROR_SUCCESS) {
            return;
        }
        std::array<wchar_t, MAX_PATH> key_str{};
        DWORD str_sz_out{key_str.size()};
        result = RegQueryValueExW(h_reg_key, L"", 0, NULL, (LPBYTE)key_str.data(), &str_sz_out);
        if (result != ERROR_SUCCESS) {
            return;
        }

        std::filesystem::path path{key_str.cbegin(), key_str.cend()};
        path = path.parent_path().append("renderdoc.dll");
        const auto path_to_lib = path.generic_string();
        mod = LoadLibraryA(path_to_lib.c_str());
    }
    ResolveRenderDocApi(mod);
#else
#ifdef ANDROID
    static constexpr const char RENDERDOC_LIB[] = "libVkLayer_GLES_RenderDoc.so";
#else
    static constexpr const char RENDERDOC_LIB[] = "librenderdoc.so";
#endif
    // If the RenderDoc layer is already injected (GUI launch or Android capture
    // layer), the library is resident and RTLD_NOLOAD returns its handle. The old
    // code only resolved the API on the offline-load branch, so an already-loaded
    // RenderDoc -- the normal capture case -- left rdoc_api null and every capture
    // silently no-opped. Resolve the API for the resident handle too.
    void* mod = dlopen(RENDERDOC_LIB, RTLD_NOW | RTLD_NOLOAD);
    if (!mod && EmulatorSettings.IsRenderdocEnabled()) {
        // If enabled in config, try to load RDoc runtime in offline mode
        mod = dlopen(RENDERDOC_LIB, RTLD_NOW);
        if (!mod) {
            LOG_ERROR(Render, "Cannot load RenderDoc: {}", dlerror());
        }
    }
    ResolveRenderDocApi(mod);
#endif
    if (rdoc_api) {
        // Disable default capture keys as they suppose to trigger present-to-present capturing
        // and it is not what we want
        rdoc_api->SetCaptureKeys(nullptr, 0);

        // Also remove rdoc crash handler
        rdoc_api->UnloadCrashHandler();
    }
}

void StartCapture() {
    if (!rdoc_api) {
        return;
    }

    // Only the thread that wins Triggered -> InProgress issues StartFrameCapture,
    // so a racing GPU-thread call cannot double-start a capture.
    CaptureState expected = CaptureState::Triggered;
    if (capture_state.compare_exchange_strong(expected, CaptureState::InProgress,
                                              std::memory_order_acq_rel)) {
        rdoc_api->StartFrameCapture(nullptr, nullptr);
    }
}

void EndCapture() {
    if (!rdoc_api) {
        return;
    }

    // Symmetric to StartCapture: exactly one InProgress -> Idle winner ends the
    // capture.
    CaptureState expected = CaptureState::InProgress;
    if (capture_state.compare_exchange_strong(expected, CaptureState::Idle,
                                              std::memory_order_acq_rel)) {
        rdoc_api->EndFrameCapture(nullptr, nullptr);
    }
}

void TriggerCapture() {
    // Arm a capture only from the idle state; a second trigger while a capture is
    // already armed or running is ignored rather than clobbering the state.
    CaptureState expected = CaptureState::Idle;
    capture_state.compare_exchange_strong(expected, CaptureState::Triggered,
                                          std::memory_order_acq_rel);
}

void SetOutputDir(const std::filesystem::path& path, const std::string& prefix) {
    if (!rdoc_api) {
        return;
    }
    LOG_WARNING(Common, "RenderDoc capture path: {}", (path / prefix).string());
    rdoc_api->SetCaptureFilePathTemplate(fmt::UTF((path / prefix).u8string()).data.data());
}

bool IsRenderDocLoaded() {
    return rdoc_api != nullptr;
}

namespace {
// Production backend adapter over the loaded RenderDoc API. Uses the top-level
// device/window (nullptr) capture like the legacy Start/EndCapture; the coordinator
// only sequences requests and reads back the capture list.
class RdocApiBackend final : public IRenderDocBackend {
public:
    bool IsLoaded() const override {
        return rdoc_api != nullptr;
    }
    u32 GetNumCaptures() override {
        return rdoc_api ? rdoc_api->GetNumCaptures() : 0;
    }
    void StartFrameCapture() override {
        if (rdoc_api) {
            rdoc_api->StartFrameCapture(nullptr, nullptr);
        }
    }
    bool EndFrameCapture() override {
        if (!rdoc_api) {
            return false;
        }
        // EndFrameCapture returns 1 if a capture was in progress.
        return rdoc_api->EndFrameCapture(nullptr, nullptr) == 1;
    }
    bool GetCapture(u32 idx, std::string& path, u64& timestamp) override {
        if (!rdoc_api) {
            return false;
        }
        u32 path_len = 0;
        // First call: query the required path length.
        if (rdoc_api->GetCapture(idx, nullptr, &path_len, nullptr) != 1 || path_len == 0) {
            return false;
        }
        std::string buf(path_len, '\0');
        u64 ts = 0;
        if (rdoc_api->GetCapture(idx, buf.data(), &path_len, &ts) != 1) {
            return false;
        }
        // path_len includes the trailing NUL; trim it.
        if (!buf.empty() && buf.back() == '\0') {
            buf.pop_back();
        }
        path = std::move(buf);
        timestamp = ts;
        return true;
    }
};

RdocApiBackend g_rdoc_backend;
}  // namespace

CaptureCoordinator& GetCaptureCoordinator() {
    static CaptureCoordinator coordinator{g_rdoc_backend};
    return coordinator;
}

void NotifyPresentBoundary() {
    // Cheap: the coordinator internally no-ops unless a request is armed/capturing.
    const u64 now_ns = static_cast<u64>(
        std::chrono::duration_cast<std::chrono::nanoseconds>(
            std::chrono::steady_clock::now().time_since_epoch())
            .count());
    GetCaptureCoordinator().OnFrameBoundary(now_ns);
}


void RequestScreenshot(const ScreenshotRequest request) {
    switch (request) {
    case ScreenshotRequest::GameOnly:
        screenshot_game_only_count.fetch_add(1, std::memory_order_relaxed);
        break;
    case ScreenshotRequest::WithOverlays:
        screenshot_with_overlays_count.fetch_add(1, std::memory_order_relaxed);
        break;
    case ScreenshotRequest::None:
    default:
        break;
    }
}

u32 ConsumeGameOnlyScreenshotRequests() {
    return screenshot_game_only_count.exchange(0, std::memory_order_acq_rel);
}

u32 ConsumeWithOverlaysScreenshotRequests() {
    return screenshot_with_overlays_count.exchange(0, std::memory_order_acq_rel);
}

ScreenshotRequests ConsumeScreenshotRequests() {
    return ScreenshotRequests{
        .game_only_count = ConsumeGameOnlyScreenshotRequests(),
        .with_overlays_count = ConsumeWithOverlaysScreenshotRequests(),
    };
}

} // namespace VideoCore
