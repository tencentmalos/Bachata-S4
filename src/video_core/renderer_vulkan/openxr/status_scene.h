// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once
#include <memory>
#include <optional>
#include <span>
#include <string>
#include <vector>
#include <openxr/openxr.h>
#include "spatial/xr/SwapchainFunctions.h"
#include "spatial/xr/XrImguiVulkanLayer.h"
#include "spatial/xr/XrSceneVulkanLayer.h"
#include "spatial/imgui/overlay/OverlayModel.hpp"
#include "spatial/perf/PerfMetrics.hpp"

namespace Vulkan::OpenXr {
// Commands publish settings / copy diagnostics only. GPU and scene objects are
// confined to the XR frame thread, including destruction.
std::string StatusSceneCommand(const std::vector<std::string>& args);
struct StatusSettings { bool visible{true}, psvr_title{}; int theme{}, layout{}; };
StatusSettings ReadStatusSettings();
void ReportStatusLayer(std::string text);
class StatusScene {
public:
    StatusScene();
    ~StatusScene();
    bool Create(XrSession session, const spatial::xr::SwapchainFunctions& functions,
                const spatial::xr::XrImguiVulkanBinding& binding,
                uint32_t eye_width, uint32_t eye_height, std::string cache_dir);
    void Recenter();
    // Inside a cinema environment the PSV sits on the World's dock: the
    // authored origin (floor under the seated eye) in LOCAL space; none = the
    // standalone head-anchored placement.
    void Place(const std::optional<XrPosef>& authored_origin);
    bool Render(std::span<const XrView> views, XrSpace space,
                const spatial::imgui::overlay::StatusSnapshot& status,
                const std::optional<spatial::perf::DeviceMetrics>& device, bool active,
                bool psvr, uint32_t output_width, uint32_t output_height);
    const XrCompositionLayerProjection* Layer() const;
    // The Lite layer behind Layer(), for the undistorted XR capture.
    spatial::xr::XrSceneVulkanLayer& SceneLayer();
private:
    struct Impl;
    std::unique_ptr<Impl> impl;
};
} // namespace Vulkan::OpenXr
