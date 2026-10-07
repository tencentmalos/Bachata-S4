// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once
#include <chrono>
#include "core/host_runtime/guest_vr_sensor.h"
#include "spatial/xr/SwapchainFunctions.h"
#include "spatial/xr/XrImguiVulkanLayer.h"

struct ImGuiContext;

namespace Vulkan::OpenXr {
// The ImGui::Layer UI (system dialogs, IME, notifications, devtools) while the
// headset is the output. The same layers that draw over the Android or desktop
// window draw here, into an independent context, and appear as a quad in front
// of the wearer while they show anything. Dialogs read the virtual pad as on
// every output (XR controllers feed it); a controller aim ray moves the pointer.
// All ImGui calls run on PresentThread; the XR pump only composites the last
// released image (Fill, under the runtime's panel mutex).
class LayerPanel {
public:
    LayerPanel();
    ~LayerPanel();
    bool Create(XrSession session, const spatial::xr::SwapchainFunctions& functions,
                const spatial::xr::XrImguiVulkanBinding& binding);
    // Builds one ImGui frame of every layer and renders it when it is not empty.
    void Update(const Core::HostRuntime::GuestVrSensor::HardwareFrame& input);
    // The quad for this frame, in `local`; false while nothing shows.
    bool Fill(XrSpace local, XrCompositionLayerQuad& layer) const;

private:
    ImGuiContext* context{};
    spatial::xr::XrImguiLayerConfig config;
    spatial::xr::XrImguiVulkanLayer renderer;
    XrPosef anchor{};
    bool anchored{}, visible{}, pointer_armed{};
    std::chrono::steady_clock::time_point last_update{};
};
} // namespace Vulkan::OpenXr
