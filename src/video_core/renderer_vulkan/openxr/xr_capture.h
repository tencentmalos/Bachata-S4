// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once
#include <array>
#include <memory>
#include <vulkan/vulkan_core.h>
#include <openxr/openxr.h>

namespace spatial::xr {
class XrSceneVulkanLayer;
}
namespace Vulkan {
class Instance;
}

namespace Vulkan::OpenXr {
// The "xr" capture source (DebugBus capture_source xr): what the wearer sees,
// without the runtime's lens distortion. The layers the runtime composites
// (cinema environment, game quad or PSVR eye projection, PSV status) are
// copied as they are submitted and ray-cast per output eye into one
// side-by-side image, which feeds the embedded screenshot (PNG) and the H.264
// recorder exactly like the canvas source does.
// All calls on the XR frame thread. Destroy only after the scenes it attached
// to, or after detaching them (Attach(scene, layer) while inactive).
class XrCapture {
public:
    enum class Scene { Environment, Status };
    explicit XrCapture(const Instance& vk);
    ~XrCapture();
    XrCapture(const XrCapture&) = delete;
    XrCapture& operator=(const XrCapture&) = delete;

    // Frame start: takes a screenshot request / encoder image while the XR
    // source is selected. False = nothing to capture this frame.
    bool Begin(uint32_t eye_width, uint32_t eye_height);
    bool Active() const;
    // Before the scene renders: route its swapchain copy into our image, or
    // detach it when inactive.
    void Attach(Scene scene, spatial::xr::XrSceneVulkanLayer& layer);
    // The scene's layer went into this frame's xrEndFrame.
    void Submitted(Scene scene, spatial::xr::XrSceneVulkanLayer& layer,
                   const XrCompositionLayerProjection& submitted);
    // Inside the mailbox -> game swapchain copy, mailbox in TRANSFER_SRC.
    void CopyScreen(VkCommandBuffer cmd, VkImage mailbox, VkFormat format, uint32_t width,
                    uint32_t height);
    // The game layer submitted this frame: cinema quad(s) (eye_mask bit 0 =
    // left, 1 = right) or the PSVR projection. Rects index the mailbox.
    void ScreenQuad(uint32_t eye_mask, const XrCompositionLayerQuad& quad);
    void ScreenProjection(const XrCompositionLayerProjection& projection);
    // After xrEndFrame: compose with the frame's eye views, then encode / read
    // back. Must follow every Begin that returned true.
    void Compose(const std::array<XrView, 2>& eyes);

private:
    struct Impl;
    std::unique_ptr<Impl> impl;
};
} // namespace Vulkan::OpenXr
