// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once
#include <filesystem>
#include <optional>
#pragma push_macro("assert_invariant")
#include "spatial/imgui/overlay/OverlayComponents.hpp"
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
    void submit(const spatial::imgui::overlay::OverlayIntent& intent) override;
    void emit(const spatial::imgui::overlay::OverlayCommand& command) override;

private:
    void Request(const std::string& command);
    void Save();
    void ApplyCommands();
    void Controls();
    spatial::imgui::overlay::OverlayShell shell;
    spatial::imgui::overlay::OverlayTheme theme;
    spatial::imgui::overlay::OverlayMetrics metrics;
    spatial::imgui::overlay::OverlaySnapshot snapshot;
    spatial::imgui::overlay::OverlayFrame frame;
    spatial::imgui::overlay::ImeDialogState ime;
    spatial::imgui::overlay::OverlayTextSize text_size{
        spatial::imgui::overlay::OverlayTextSize::Medium};
    std::filesystem::path settings_path;
    unsigned width{}, height{};
    float pixel_density{};
    std::uint64_t owner{}, sequence{};
    std::optional<int> pointer;
    bool pad_captured{};
    bool ime_captured{};
    bool initialized{};
};
} // namespace ImGui
