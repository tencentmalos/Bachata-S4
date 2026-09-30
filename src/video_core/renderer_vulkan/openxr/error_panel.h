// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once
#include <string>
#include <vector>
#include "video_core/renderer_vulkan/vk_common.h"
#include "video_core/renderer_vulkan/vk_driver.h"
#include "spatial/xr/XrImguiVulkanLayer.h"
struct ImGuiContext;
namespace Vulkan { class Instance; }
namespace Vulkan::OpenXr {
// Used only after the guest renderer has drained. All ImGui calls stay on the
// XR pump thread; the process-wide Vulkan dispatcher lease excludes the game.
class ErrorPanel {
public:
    ErrorPanel();
    ~ErrorPanel();
    spatial::xr::XrImguiVulkanLayer& Renderer() { return renderer; }
    const spatial::xr::XrImguiLayerConfig& Config() const { return config; }
    void SetDetail(std::string text) { detail = std::move(text); }
    // 1 retry, 2 library. No action while a button held during entry is unreleased.
    struct Pointer { float x{}, y{}, scroll{}; bool visible{}, down{}; };
    int Draw(bool left, bool right, bool up, bool down, bool accept, bool cancel, bool focused,
             const Pointer& pointer, float delta_time);
private:
    ImGuiContext* context{};
    spatial::xr::XrImguiVulkanLayer renderer;
    spatial::xr::XrImguiLayerConfig config;
    std::string detail;
    bool armed{};
    bool pointer_armed{};
};
} // namespace Vulkan::OpenXr
