// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once
#include <chrono>
#include "imgui/imgui_layer.h"
#include "spatial/imgui/overlay/OverlayModel.hpp"
#include "spatial/xr/XrImguiVulkanLayer.h"
#include "spatial/xr/SwapchainFunctions.h"

namespace Vulkan::OpenXr {
// An independent display-only ImGui layer. All ImGui calls run on PresentThread,
// between mirror frames, restoring its context. The XR pump only composites the
// last released image; it never calls ImGui or waits for a new status image.
class StatusPanel final : public ImGui::Layer {
public:
    explicit StatusPanel(int layout);
    ~StatusPanel() override;
    bool Create(XrSession session, const spatial::xr::SwapchainFunctions& functions,
                const spatial::xr::XrImguiVulkanBinding& binding);
    void Update(const spatial::imgui::overlay::StatusSnapshot& status,
                uint32_t width, uint32_t height, int theme);
    void Draw() override;
    bool ShouldKeepDrawing() override { return false; }
    bool Fill(XrSpace view, XrCompositionLayerQuad& layer) const;
private:
    ImGuiContext* context{};
    spatial::xr::XrImguiLayerConfig config;
    spatial::xr::XrImguiVulkanLayer renderer;
    spatial::imgui::overlay::StatusSnapshot snapshot;
    uint32_t output_width{}, output_height{};
    int theme{};
    bool vertical{};
    uint64_t updates{}, reused{};
    std::chrono::steady_clock::time_point last_update{};
};
} // namespace Vulkan::OpenXr
