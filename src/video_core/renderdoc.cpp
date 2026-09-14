// SPDX-FileCopyrightText: Copyright 2024-2026 shadPS4 Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later
#include "video_core/renderdoc.h"
#include "video_core/renderdoc_capture.h"
#include "core/emulator_settings.h"
#include "core/diagnostics/diagnostics_hub_registry.h"
#include "core/diagnostics/trace_identity.h"
#include "common/logging/formatter.h"
#include "common/path_util.h"
#include <renderdoc_app.h>
#include <openssl/sha.h>
#include <atomic>
#include <array>
#include <fstream>
#include <sstream>
#include <mutex>
#ifdef _WIN32
#include <windows.h>
#else
#include <dlfcn.h>
#endif

namespace VideoCore {
namespace {
std::atomic<RENDERDOC_API_1_6_0*> loaded_api{};
std::mutex loader_mutex;
std::atomic<u32> screenshot_game_only_count{}, screenshot_with_overlays_count{};
std::string FileStem(std::string text) {
    for (char& c : text) if (c == ':') c = '_';
    return text;
}
RENDERDOC_API_1_6_0* Api() { return loaded_api.load(std::memory_order_acquire); }
}
void LoadRenderDoc() {
    std::lock_guard lock(loader_mutex);
    if (Api()) return;
    pRENDERDOC_GetAPI get{};
#ifdef _WIN32
    auto mod = GetModuleHandleA("renderdoc.dll");
    if (!mod && EmulatorSettings.IsRenderdocEnabled()) {
        HKEY key{};
        if (RegOpenKeyExW(HKEY_LOCAL_MACHINE,
                L"SOFTWARE\\Classes\\RenderDoc.RDCCapture.1\\DefaultIcon", 0, KEY_READ, &key) == ERROR_SUCCESS) {
            std::array<wchar_t, MAX_PATH> value{};
            DWORD size = sizeof(value), type{};
            const auto result = RegQueryValueExW(key, L"", nullptr, &type,
                reinterpret_cast<LPBYTE>(value.data()), &size);
            RegCloseKey(key);
            if (result == ERROR_SUCCESS && (type == REG_SZ || type == REG_EXPAND_SZ)) {
                value.back() = 0;
                std::wstring icon{value.data()};
                if (auto comma = icon.rfind(L','); comma != std::wstring::npos) icon.resize(comma);
                if (icon.size() >= 2 && icon.front() == L'"' && icon.back() == L'"')
                    icon = icon.substr(1, icon.size() - 2);
                const auto library = std::filesystem::path(icon).parent_path() / L"renderdoc.dll";
                mod = LoadLibraryW(library.c_str());
            }
        }
    }
    if (mod) get = reinterpret_cast<pRENDERDOC_GetAPI>(GetProcAddress(mod, "RENDERDOC_GetAPI"));
#else
    // Injected layers can already be globally visible under another absolute path.
    get = reinterpret_cast<pRENDERDOC_GetAPI>(dlsym(RTLD_DEFAULT, "RENDERDOC_GetAPI"));
#if defined(__ANDROID__)
    constexpr const char* library = "libVkLayer_GLES_RenderDoc.so";
#else
    constexpr const char* library = "librenderdoc.so";
#endif
    if (!get) {
        void* mod = dlopen(library, RTLD_NOW | RTLD_NOLOAD);
        if (!mod && EmulatorSettings.IsRenderdocEnabled()) mod = dlopen(library, RTLD_NOW);
        if (mod) get = reinterpret_cast<pRENDERDOC_GetAPI>(dlsym(mod, "RENDERDOC_GetAPI"));
        // Retain a successful dlopen handle for process lifetime, like the driver namespace.
    }
#endif
    if (!get) return;
    RENDERDOC_API_1_6_0* api{};
    if (get(eRENDERDOC_API_Version_1_6_0, reinterpret_cast<void**>(&api)) != 1 || !api) {
        LOG_ERROR(Render, "RenderDoc API 1.6 unavailable");
        return;
    }
    api->SetCaptureKeys(nullptr, 0);
    api->UnloadCrashHandler();
    loaded_api.store(api, std::memory_order_release);
}
bool IsRenderDocLoaded() { return Api() != nullptr; }

namespace {
class RdocApiBackend final : public IRenderDocBackend {
public:
    bool IsLoaded() const override { return IsRenderDocLoaded(); }
    bool IsCapturing() override { const auto a = Api(); return a && a->IsFrameCapturing(); }
    u32 GetNumCaptures() override { const auto a = Api(); return a ? a->GetNumCaptures() : 0; }
    bool StartFrameCapture(const CaptureTarget& target, const CaptureReceipt& request) override {
        const auto a = Api();
        if (!a || !target.device || target.output_directory.empty()) return false;
        std::filesystem::create_directories(target.output_directory);
        const auto prefix = std::filesystem::path(target.output_directory) / FileStem(request.capture_uuid);
        a->SetCaptureFilePathTemplate(prefix.string().c_str());
        a->StartFrameCapture(target.device, target.window);
        if (!a->IsFrameCapturing()) return false;
        a->SetCaptureTitle(request.capture_uuid.c_str());
        return true;
    }
    bool EndFrameCapture(const CaptureTarget& target) override {
        const auto a = Api();
        return a && a->EndFrameCapture(target.device, target.window) == 1;
    }
    bool DiscardFrameCapture(const CaptureTarget& target) override {
        const auto a = Api();
        if (!a) return false;
        if (!a->IsFrameCapturing()) return true;
        const auto result = a->DiscardFrameCapture(target.device, target.window);
        return result == 1 && !a->IsFrameCapturing();
    }
    bool GetCapture(u32 index, std::string& path, u64& timestamp) override {
        const auto a = Api();
        u32 size{};
        if (!a || !a->GetCapture(index, nullptr, &size, nullptr) || !size || size > 65536) return false;
        std::string buffer(size, '\0');
        if (!a->GetCapture(index, buffer.data(), &size, &timestamp)) return false;
        if (!buffer.empty() && buffer.back() == '\0') buffer.pop_back();
        path = std::move(buffer);
        return !path.empty();
    }
    bool Finalize(const CaptureTarget& target, CaptureReceipt& r) override {
        const auto path = std::filesystem::canonical(r.file_path);
        const auto root = std::filesystem::canonical(target.output_directory);
        if (path.parent_path() != root || !path.filename().string().starts_with(FileStem(r.capture_uuid)))
            return false;
        const auto size = std::filesystem::file_size(path);
        if (!size || size > 4ull * 1024 * 1024 * 1024) return false;
        std::ifstream file(path, std::ios::binary);
        if (!file) return false;
        SHA256_CTX hash;
        SHA256_Init(&hash);
        char bytes[65536];
        u64 read{};
        while (file.read(bytes, sizeof(bytes)) || file.gcount()) {
            SHA256_Update(&hash, bytes, static_cast<size_t>(file.gcount()));
            read += file.gcount();
        }
        if (!file.eof() || read != size || std::filesystem::file_size(path) != size) return false;
        unsigned char digest[SHA256_DIGEST_LENGTH];
        SHA256_Final(digest, &hash);
        constexpr char hex[] = "0123456789abcdef";
        for (auto byte : digest) { r.file_sha256 += hex[byte >> 4]; r.file_sha256 += hex[byte & 15]; }
        r.file_size = size;
        r.file_path = path.string();
        r.sidecar_path = path.string() + ".json";
        const auto temp = r.sidecar_path + ".partial";
        std::ofstream sidecar(temp, std::ios::binary | std::ios::trunc);
        using Core::Diagnostics::JsonEscape;
        sidecar << "{\n\"schema\":\"shadps4.renderdoc.v1\",\n"
                << "\"run_uuid\":\"" << JsonEscape(r.run_uuid) << "\",\n"
                << "\"capture_uuid\":\"" << JsonEscape(r.capture_uuid) << "\",\n"
                << "\"generation\":\"" << r.generation << "\",\n"
                << "\"request_id\":\"" << r.request_id << "\",\n"
                << "\"first_present\":\"" << r.first_present << "\",\n"
                << "\"last_present\":\"" << r.last_present << "\",\n"
                << "\"coverage\":\"host_present_interval\",\n"
                << "\"guest_frame_equivalence\":\"unverified\",\n"
                << "\"driver\":\"" << JsonEscape(target.driver_identity) << "\",\n"
                << "\"size\":\"" << size << "\",\n"
                << "\"sha256\":\"" << r.file_sha256 << "\"\n}\n";
        sidecar.close();
        if (!sidecar) return false;
        std::filesystem::rename(temp, r.sidecar_path);
        return true;
    }
};
RdocApiBackend backend;
}
CaptureCoordinator& GetCaptureCoordinator() {
    static CaptureCoordinator coordinator{backend, Core::Diagnostics::DiagnosticNowNs};
    return coordinator;
}
void BindCaptureTarget(u64 generation, void* instance, void* window, std::string driver) {
    LoadRenderDoc(); // Also retry AFTER instance creation, when Android has loaded its layers.
    CaptureTarget target{generation, RENDERDOC_DEVICEPOINTER_FROM_VKINSTANCE(instance), window,
        Common::FS::GetUserPath(Common::FS::PathType::CapturesDir).string(), std::move(driver)};
    if (!GetCaptureCoordinator().Bind(std::move(target)))
        throw std::runtime_error("Capture target from the preceding session has not retired");
}
void StopCaptureTarget(u64 generation) { GetCaptureCoordinator().RequestStop(generation); }
void UnbindCaptureTarget(u64 generation) { GetCaptureCoordinator().Unbind(generation); }
void NotifyPresentBoundary(u64 generation, u64 present_id) {
    GetCaptureCoordinator().OnFrameBoundary(generation, present_id, Core::Diagnostics::DiagnosticNowNs());
}
// Desktop hotkeys and command requests share exactly one capture owner. The old
// Liverpool drain callbacks no longer drive an independent Start/End machine.
void StartCapture() {}
void EndCapture() {}
void TriggerCapture() {
    auto& c = GetCaptureCoordinator();
    (void)c.Arm(1, c.BoundGeneration(), Core::Diagnostics::ProcessRunUuid(), Core::Diagnostics::DiagnosticNowNs());
}
void SetOutputDir(const std::filesystem::path&, const std::string&) {
    // Output belongs to the bound Session target and each unique request.
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
