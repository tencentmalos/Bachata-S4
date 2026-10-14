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
// The XR session as a capture producer, through Foundation XrLayerCapture
// (foundation/modules/capture): source xr is what the wearer sees without the
// runtime's lens distortion (cinema environment, game quad or PSVR eye
// projection, PSV status, ray-cast per output eye side by side); source canvas
// is the game's mailbox image. The layers are noted as the runtime frame
// builds them and handed to Foundation bottom to top (environment, game,
// status) when the frame is composed.
// All calls on the XR frame thread. Destroy only after the scenes it attached
// to, or after detaching them (Attach(scene, layer) while inactive).
class XrCapture {
public:
    enum class Scene { Environment, Status };
    explicit XrCapture(const Instance& vk);
    ~XrCapture();
    XrCapture(const XrCapture&) = delete;
    XrCapture& operator=(const XrCapture&) = delete;

    // Frame start: takes a screenshot request / encoder image. False = nothing
    // to capture this frame.
    bool Begin(uint32_t eye_width, uint32_t eye_height, int64_t display_period_ns);
    bool Active() const;
    // Before the scene renders: route its swapchain copy into our image, or
    // detach it when inactive.
    void Attach(Scene scene, spatial::xr::XrSceneVulkanLayer& layer);
    // The scene's layer went into this frame's xrEndFrame.
    void Submitted(Scene scene, spatial::xr::XrSceneVulkanLayer& layer,
                   const XrCompositionLayerProjection& submitted);
    // Inside the mailbox -> game swapchain copy, mailbox in TRANSFER_SRC; the
    // copied content area (both eyes side by side when `views` is 2).
    void CopyScreen(VkCommandBuffer cmd, VkImage mailbox, VkFormat format, uint32_t width,
                    uint32_t height, uint32_t views);
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
