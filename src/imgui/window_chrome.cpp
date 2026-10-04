// SPDX-License-Identifier: GPL-2.0-or-later
// Windowed-mode menu bar and status bar, after citron's main window: File / Emulation / View /
// Tools / Help, and a status bar with clickable toggles on the left and Building / Scale /
// Game FPS / Frame time on the right, refreshed every 500 ms.
#include <algorithm>
#include <atomic>
#include <cmath>
#include <filesystem>
#include <fstream>
#include <string>
#include <SDL3/SDL_events.h>
#include <SDL3/SDL_misc.h>
#include <fmt/format.h>
#include <imgui.h>
#include <imgui_internal.h>
#include <nlohmann/json.hpp>
#include "common/logging/log.h"
#include "common/path_util.h"
#include "common/scm_rev.h"
#include "core/debug_state.h"
#include "core/diagnostics/overlay_control.h"
#include "core/emulator_settings.h"
#include "core/guest_patch_desktop.h"
#include "imgui/big_picture/settings_dialog_layer.h"
#include "imgui/window_chrome.h"
#include "input/input_handler.h"
#include "video_core/renderdoc.h"
#include "video_core/renderer_vulkan/vk_pipeline_stats.h"
#include "video_core/renderer_vulkan/vk_presenter.h"

extern std::unique_ptr<Vulkan::Presenter> presenter;

namespace ImGui::WindowChrome {
namespace {

std::atomic<bool> fullscreen{};
std::atomic<float> top_inset{}, bottom_inset{};

// View > Show Status Bar, kept in the user directory.
bool preferences_loaded{};
bool show_status_bar{true};
bool open_about{};

// Status bar texts, refreshed every 500 ms like citron's status bar timer.
StatusInfo status{};
double status_refreshed{-1.0};
std::string fps_text, frame_text, scale_text, building_text;

std::filesystem::path PreferencesPath() {
    return Common::FS::GetUserPath(Common::FS::PathType::UserDir) / "window-chrome.json";
}

void LoadPreferences() {
    preferences_loaded = true;
    try {
        if (std::ifstream in{PreferencesPath()}; in) {
            show_status_bar = nlohmann::json::parse(in).value("status_bar", true);
        }
    } catch (const std::exception& e) {
        LOG_WARNING(ImGui, "Ignoring invalid window preferences: {}", e.what());
    }
}

void SavePreferences() {
    try {
        const auto path = PreferencesPath();
        auto temp = path;
        temp += ".tmp";
        {
            std::ofstream out{temp, std::ios::trunc};
            out << nlohmann::json{{"status_bar", show_status_bar}}.dump(2) << '\n';
            if (!out.flush()) {
                throw std::runtime_error("write failed");
            }
        }
        std::filesystem::rename(temp, path);
    } catch (const std::exception& e) {
        LOG_WARNING(ImGui, "Cannot save window preferences: {}", e.what());
    }
}

// Actions shared with the hotkeys go through the same SDL events, handled on the main thread.
void Push(Uint32 type, Sint32 code = 0) {
    SDL_Event event{};
    event.type = type;
    event.user.code = code;
    SDL_PushEvent(&event);
}

void OverlayRequest(const char* command) {
    ::Core::Diagnostics::StatusOverlayMailbox().Request(command);
}

void OpenFolder(Common::FS::PathType type) {
    const auto path = Common::FS::GetUserPath(type);
    std::error_code ec;
    std::filesystem::create_directories(path, ec);
    auto url = Common::FS::PathToUTF8String(path);
    std::ranges::replace(url, '\\', '/');
    url = "file:///" + url;
    if (!SDL_OpenURL(url.c_str())) {
        LOG_WARNING(ImGui, "Cannot open {}: {}", url, SDL_GetError());
    }
}

// Asks the window for a client size whose game area, between the bars, is width x height.
void ResizeGameArea(int width, int height) {
    const int total = height + static_cast<int>(std::ceil(top_inset.load() + bottom_inset.load()));
    Push(SDL_EVENT_RESIZE_WINDOW, (std::clamp(width, 1, 0x7fff) << 16) | std::clamp(total, 1, 0xffff));
}

void DrawMenuBar() {
    if (!BeginMainMenuBar()) {
        return;
    }
    if (BeginMenu("File")) {
        if (MenuItem("Open User Folder")) {
            OpenFolder(Common::FS::PathType::UserDir);
        }
        if (MenuItem("Open Log Folder")) {
            OpenFolder(Common::FS::PathType::LogDir);
        }
        if (MenuItem("Open Screenshots Folder")) {
            OpenFolder(Common::FS::PathType::ScreenshotsDir);
        }
        Separator();
        if (MenuItem("Exit")) {
            Push(SDL_EVENT_QUIT_DIALOG);
        }
        EndMenu();
    }
    if (BeginMenu("Emulation")) {
        const bool paused = DebugState.IsGuestThreadsPaused();
        if (MenuItem(paused ? "Continue" : "Pause")) {
            Push(SDL_EVENT_TOGGLE_PAUSE);
        }
        if (MenuItem("Stop")) {
            Push(SDL_EVENT_QUIT_DIALOG); // asks before quitting, like the quit hotkey
        }
        Separator();
        if (MenuItem("Configure...")) {
            ImGuiEmuSettings::OpenInGameSettingsDialog();
        }
        EndMenu();
    }
    if (BeginMenu("View")) {
        if (MenuItem("Fullscreen", nullptr, false)) {
            Push(SDL_EVENT_TOGGLE_FULLSCREEN);
        }
        if (MenuItem("Show Status Bar", nullptr, show_status_bar)) {
            show_status_bar = !show_status_bar;
            SavePreferences();
        }
        Separator();
        if (BeginMenu("Status Overlay")) {
            static constexpr const char* kLabels[] = {"Off", "Only FPS", "Summary"};
            static constexpr const char* kRequests[] = {"hide", "simple", "summary"};
            for (int mode = 0; mode < 3; ++mode) {
                if (MenuItem(kLabels[mode], nullptr, status.overlay_mode == mode)) {
                    OverlayRequest(kRequests[mode]);
                }
            }
            Separator();
            if (MenuItem("Detail")) {
                OverlayRequest("detail");
            }
            if (MenuItem("Controls...", "F10")) {
                OverlayRequest("controls");
            }
            EndMenu();
        }
        if (BeginMenu("Reset Window Size")) {
            if (MenuItem("1280x720")) {
                ResizeGameArea(1280, 720);
            }
            if (MenuItem("1600x900")) {
                ResizeGameArea(1600, 900);
            }
            if (MenuItem("1920x1080")) {
                ResizeGameArea(1920, 1080);
            }
            EndMenu();
        }
        EndMenu();
    }
    if (BeginMenu("Tools")) {
        if (MenuItem("Capture Screenshot")) {
            VideoCore::RequestScreenshot(VideoCore::ScreenshotRequest::GameOnly);
        }
        if (MenuItem("Capture Screenshot with Overlays")) {
            VideoCore::RequestScreenshot(VideoCore::ScreenshotRequest::WithOverlays);
        }
        if (VideoCore::IsRenderDocLoaded() && MenuItem("RenderDoc Capture")) {
            Push(SDL_EVENT_RDOC_CAPTURE);
        }
        Separator();
        // Packages installed for this run; switching keeps their code resident.
        const auto patches = ::Core::GuestPatch::Desktop::Installed();
        if (BeginMenu("Guest Patches", !patches.empty())) {
            for (const auto& patch : patches) {
                if (MenuItem(patch.display_name.c_str(), nullptr, patch.enabled, patch.switchable)) {
                    ::Core::GuestPatch::Desktop::SetEnabled(patch.id, !patch.enabled);
                }
            }
            EndMenu();
        }
        if (BeginMenu("Mouse")) {
            if (MenuItem("Mouse to Joystick")) {
                Push(SDL_EVENT_MOUSE_TO_JOYSTICK);
            }
            if (MenuItem("Mouse to Gyro")) {
                Push(SDL_EVENT_MOUSE_TO_GYRO);
            }
            if (MenuItem("Mouse to Touchpad")) {
                Push(SDL_EVENT_MOUSE_TO_TOUCHPAD);
            }
            EndMenu();
        }
        if (MenuItem("Reload Input Config")) {
            Push(SDL_EVENT_RELOAD_INPUTS);
        }
        Separator();
        MenuItem("Developer Tools", "Ctrl+F10", &DebugState.IsShowingDebugMenuBar());
        EndMenu();
    }
    if (BeginMenu("Help")) {
        if (MenuItem("About shadPS4")) {
            open_about = true;
        }
        EndMenu();
    }
    top_inset.store(GetWindowSize().y, std::memory_order_relaxed);
    EndMainMenuBar();
}

void RefreshStatusTexts() {
    const double now = GetTime();
    if (status_refreshed >= 0.0 && now - status_refreshed < 0.5) {
        return;
    }
    status_refreshed = now;
    if (status.fps && *status.fps > 0.0) {
        fps_text = fmt::format("Game: {:.0f} FPS", *status.fps);
        frame_text = fmt::format("Frame: {:.2f} ms", 1000.0 / *status.fps);
    } else {
        fps_text = "Game: -- FPS";
        frame_text = "Frame: -- ms";
    }
    scale_text = fmt::format("Scale: {:g}x", status.render_scale);
    const auto building = Vulkan::PipelineStats::PendingBuilds();
    building_text = building ? fmt::format("Building: {} shader{}", building, building == 1 ? "" : "s")
                             : std::string{};
}

// Flat status bar button; `active` draws it in the header colour (citron's checked buttons).
bool StatusButton(const char* label, bool active) {
    PushStyleColor(ImGuiCol_Button, active ? GetStyleColorVec4(ImGuiCol_Header) : ImVec4{0, 0, 0, 0});
    const bool pressed = SmallButton(label);
    PopStyleColor();
    SameLine();
    return pressed;
}

void DrawStatusBar() {
    RefreshStatusTexts();
    const float height = GetFrameHeight();
    // NoNav: clicking the bar must not take the keyboard from the game.
    constexpr ImGuiWindowFlags kFlags = ImGuiWindowFlags_NoScrollbar | ImGuiWindowFlags_NoSavedSettings |
                                       ImGuiWindowFlags_MenuBar | ImGuiWindowFlags_NoNav;
    if (BeginViewportSideBar("##WindowStatusBar", GetMainViewport(), ImGuiDir_Down, height, kFlags)) {
        if (BeginMenuBar()) {
            static constexpr const char* kOverlay[] = {"Status: Off", "Status: FPS", "Status: Summary"};
            static constexpr const char* kNext[] = {"simple", "summary", "hide"};
            const int mode = std::clamp(status.overlay_mode, 0, 2);
            if (StatusButton(kOverlay[mode], mode != 0)) {
                OverlayRequest(kNext[mode]);
            }
            if (presenter) {
                auto& fsr = presenter->GetFsrSettingsRef();
                if (StatusButton(fsr.enable ? "FSR" : "BILINEAR", fsr.enable)) {
                    fsr.enable = !fsr.enable; // this run only; Launch Options keep the saved choice
                }
            }
            const int volume = EmulatorSettings.GetVolumeSlider();
            if (StatusButton(fmt::format("VOLUME: {}%##volume", volume).c_str(), volume != 0)) {
                OpenPopup("##VolumePopup");
            }
            if (BeginPopup("##VolumePopup")) {
                int value = volume;
                SetNextItemWidth(200.f * GetFontSize() / 16.f);
                if (SliderInt("##volume", &value, 0, 500, "%d%%")) {
                    EmulatorSettings.SetVolumeSlider(value);
                }
                EndPopup();
            }
            if (DebugState.IsGuestThreadsPaused() && StatusButton("PAUSED", true)) {
                Push(SDL_EVENT_TOGGLE_PAUSE);
            }
            // Right-aligned readings.
            const float spacing = GetStyle().ItemSpacing.x * 2.f;
            float width = CalcTextSize(scale_text.c_str()).x + CalcTextSize(fps_text.c_str()).x +
                          CalcTextSize(frame_text.c_str()).x + spacing * 3.f;
            if (!building_text.empty()) {
                width += CalcTextSize(building_text.c_str()).x + spacing;
            }
            SameLine(std::max(GetCursorPosX(), GetWindowWidth() - width));
            if (!building_text.empty()) {
                TextColored(ImVec4{1.f, .75f, .3f, 1.f}, "%s", building_text.c_str());
                SameLine(0.f, spacing);
            }
            TextUnformatted(scale_text.c_str());
            SameLine(0.f, spacing);
            TextUnformatted(fps_text.c_str());
            SameLine(0.f, spacing);
            TextUnformatted(frame_text.c_str());
            EndMenuBar();
        }
        bottom_inset.store(GetWindowSize().y, std::memory_order_relaxed);
    }
    End();
}

void DrawAbout() {
    if (open_about) {
        OpenPopup("About shadPS4");
        open_about = false;
    }
    SetNextWindowPos(GetMainViewport()->GetCenter(), ImGuiCond_Appearing, ImVec2{.5f, .5f});
    if (BeginPopupModal("About shadPS4", nullptr, ImGuiWindowFlags_AlwaysAutoResize)) {
        Text("shadPS4 %s", Common::g_version);
        TextDisabled("%s %s", Common::g_scm_branch, Common::g_scm_desc);
        TextDisabled("%s", Common::g_scm_date);
        Spacing();
        if (Button("OK", ImVec2{120.f * GetFontSize() / 16.f, 0.f}) ||
            IsKeyPressed(ImGuiKey_Escape) || IsKeyPressed(ImGuiKey_Enter)) {
            CloseCurrentPopup();
        }
        EndPopup();
    }
}

} // namespace

void SetFullscreen(bool value) {
    fullscreen.store(value, std::memory_order_relaxed);
}

void SetStatus(const StatusInfo& info) {
    status = info;
}

void Draw() {
    if (!preferences_loaded) {
        LoadPreferences();
    }
    if (fullscreen.load(std::memory_order_relaxed)) {
        top_inset.store(0.f, std::memory_order_relaxed);
        bottom_inset.store(0.f, std::memory_order_relaxed);
        return;
    }
    DrawMenuBar();
    if (show_status_bar) {
        DrawStatusBar();
    } else {
        bottom_inset.store(0.f, std::memory_order_relaxed);
    }
    DrawAbout();
    // A closed menu leaves the keyboard focus on the menu bar, where ImGui navigation would keep
    // the game's keys. Hand it back, as citron refocuses the render window after a menu action.
    const auto& g = *GetCurrentContext();
    if (!IsPopupOpen("", ImGuiPopupFlags_AnyPopupId) && g.NavWindow != nullptr &&
        (g.NavWindow->Flags & ImGuiWindowFlags_MenuBar) != 0 &&
        (g.NavWindow == FindWindowByName("##MainMenuBar") ||
         g.NavWindow == FindWindowByName("##WindowStatusBar"))) {
        FocusWindow(nullptr);
    }
}

float TopInset() {
    return top_inset.load(std::memory_order_relaxed);
}

float BottomInset() {
    return bottom_inset.load(std::memory_order_relaxed);
}

bool Contains(float x, float y, float window_height) {
    (void)x;
    if (fullscreen.load(std::memory_order_relaxed)) {
        return false;
    }
    return y < TopInset() || y >= window_height - BottomInset();
}

} // namespace ImGui::WindowChrome
