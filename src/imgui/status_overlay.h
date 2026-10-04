// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once
#include <filesystem>
#include <optional>
#pragma push_macro("assert_invariant")
#include "spatial/imgui/overlay/OverlayComponents.hpp"
#include "spatial/imgui/overlay/PerfHud.hpp"
#undef assert_invariant
#pragma pop_macro("assert_invariant")

namespace ImGui {
// Host adapter for Foundation's shell. One instance per Presenter / ImGui context.
class StatusOverlay final : public spatial::imgui::overlay::IntentSink {
public:
    StatusOverlay();
    ~StatusOverlay() override;
    void Begin(unsigned width, unsigned height);
    void Prepare(spatial::imgui::overlay::StatusSnapshot status);
    void Draw();
    bool WantsMetrics() const;
    bool WantsDetail() const;
    spatial::imgui::overlay::StatusMode Mode() const {
        return shell.controller().statusMode();
    }
    // Rows, graphs and appearance of the performance HUD shown in Summary; saved with the shell.
    const spatial::perf::PerfHudSettings& HudSettings() const {
        return perf_hud;
    }
    void submit(const spatial::imgui::overlay::OverlayIntent& intent) override;
    void emit(const spatial::imgui::overlay::OverlayCommand& command) override;

private:
    void Request(const std::string& command);
    void Save();
    void ApplyCommands();
    void Controls();
    // Ends the touch that ImGui follows; for the desktop mouse only the button is released.
    void ReleasePointer();
    spatial::imgui::overlay::OverlayShell shell;
    spatial::imgui::overlay::OverlayTheme theme;
    spatial::imgui::overlay::OverlayMetrics metrics;
    spatial::imgui::overlay::OverlaySnapshot snapshot;
    spatial::imgui::overlay::OverlayFrame frame;
    spatial::imgui::overlay::ImeDialogState ime;
    spatial::imgui::overlay::OverlayTextSize text_size{
        spatial::imgui::overlay::OverlayTextSize::Medium};
    spatial::perf::PerfHudSettings perf_hud{spatial::perf::defaultPerfHudSettings()};
    // Output the shell lays out against; the horizontal Summary wraps to its safe width.
    spatial::imgui::overlay::PresentationEnvironment environment;
    // Corner of the FPS chip / Summary panel; the top left also keeps clear of Android's touch
    // controls in the bottom corners.
    spatial::imgui::overlay::StatusAnchor status_anchor{
        spatial::imgui::overlay::StatusAnchor::TopLeft};
    std::filesystem::path settings_path;
    unsigned width{}, height{};
    float pixel_density{};
    // Window menu bar and status bar (desktop windowed mode): the HUD stays between them.
    float inset_top{}, inset_bottom{};
    std::uint64_t owner{}, sequence{};
    std::optional<int> pointer;
    bool pad_captured{};
    bool ime_captured{};
    bool initialized{};
};
} // namespace ImGui
