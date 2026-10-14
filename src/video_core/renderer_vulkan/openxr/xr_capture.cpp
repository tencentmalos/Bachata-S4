// SPDX-License-Identifier: GPL-2.0-or-later
#include "video_core/renderer_vulkan/openxr/xr_capture.h"

#include <optional>
#include <vector>

#include "spatial/capture/XrLayerCapture.h"
#include "spatial/xr/XrSceneVulkanLayer.h"
#include "video_core/renderer_vulkan/capture_recorder.h"
#include "video_core/renderer_vulkan/vk_instance.h"

namespace Vulkan::OpenXr {
namespace {

using spatial::capture::OverlayHook;
using spatial::capture::Source;

Source SceneSource(XrCapture::Scene scene) {
    return scene == XrCapture::Scene::Environment ? Source::Environment : Source::Hud;
}

// A scene renders every frame: it copies only on captured frames.
OverlayHook SceneHook(spatial::xr::XrSceneVulkanLayer& layer) {
    OverlayHook hook;
    hook.identity = &layer;
    hook.ready = [&layer] { return layer.IsCreated() && layer.Format() != VK_FORMAT_UNDEFINED; };
    hook.format = [&layer] { return layer.Format(); };
    hook.extent = [&layer] { return VkExtent2D{layer.SwapchainWidth(), layer.EyeHeight()}; };
    hook.layers = [&layer] { return layer.SwapchainLayers(); };
    hook.setTarget = [&layer](VkImage image) { layer.SetCaptureTarget(image); };
    hook.serial = [&layer] { return layer.CaptureSerial(); };
    hook.everyFrame = true;
    return hook;
}

// A projection layer as submitted, with its own copy of the views.
struct Projection {
    XrCompositionLayerProjection layer{XR_TYPE_COMPOSITION_LAYER_PROJECTION};
    std::array<XrCompositionLayerProjectionView, 2> views{};

    explicit Projection(const XrCompositionLayerProjection& submitted) : layer(submitted) {
        views = {submitted.views[0], submitted.views[1]};
        layer.viewCount = 2;
        layer.views = views.data();
        layer.next = nullptr;
    }
    const XrCompositionLayerBaseHeader* Header() const {
        return reinterpret_cast<const XrCompositionLayerBaseHeader*>(&layer);
    }
};

} // namespace

struct XrCapture::Impl {
    explicit Impl(const Instance& vk)
        : capture(EmbeddedCaptureBinding(vk, false), spatial::capture::XrFunctions{}) {}

    void Reset() {
        environment.reset();
        status.reset();
        projection.reset();
        quads.clear();
    }

    spatial::capture::XrLayerCapture capture;
    // This frame's layers, handed over bottom to top at Compose.
    std::optional<Projection> environment;
    std::optional<Projection> status;
    std::optional<Projection> projection;
    std::vector<XrCompositionLayerQuad> quads;
};

XrCapture::XrCapture(const Instance& vk) {
    ConfigureEmbeddedCapture();
    impl = std::make_unique<Impl>(vk);
}

XrCapture::~XrCapture() = default;

bool XrCapture::Active() const {
    return impl->capture.active();
}

bool XrCapture::Begin(uint32_t eye_width, uint32_t eye_height, int64_t display_period_ns) {
    impl->Reset();
    return impl->capture.begin(eye_width, eye_height, display_period_ns);
}

void XrCapture::Attach(Scene scene, spatial::xr::XrSceneVulkanLayer& layer) {
    impl->capture.attach(SceneSource(scene), SceneHook(layer));
}

void XrCapture::Submitted(Scene scene, spatial::xr::XrSceneVulkanLayer&,
                          const XrCompositionLayerProjection& submitted) {
    if (!impl->capture.active() || submitted.viewCount < 2) return;
    (scene == Scene::Environment ? impl->environment : impl->status).emplace(submitted);
}

void XrCapture::CopyScreen(VkCommandBuffer cmd, VkImage mailbox, VkFormat format, uint32_t width,
                           uint32_t height, uint32_t views) {
    impl->capture.copyScreenImage(cmd, mailbox, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, format, width,
                                  height, views);
}

void XrCapture::ScreenQuad(uint32_t eye_mask, const XrCompositionLayerQuad& quad) {
    if (!impl->capture.active()) return;
    XrCompositionLayerQuad copy = quad;
    copy.next = nullptr;
    copy.eyeVisibility = eye_mask == 1u   ? XR_EYE_VISIBILITY_LEFT
                         : eye_mask == 2u ? XR_EYE_VISIBILITY_RIGHT
                                          : XR_EYE_VISIBILITY_BOTH;
    impl->quads.push_back(copy);
}

void XrCapture::ScreenProjection(const XrCompositionLayerProjection& projection) {
    if (!impl->capture.active() || projection.viewCount < 2) return;
    impl->projection.emplace(projection);
}

void XrCapture::Compose(const std::array<XrView, 2>& eyes) {
    auto& p = *impl;
    if (!p.capture.active()) return;
    // Bottom to top, as the runtime composites them.
    if (p.environment) p.capture.note(p.environment->Header(), Source::Environment);
    if (p.projection) {
        p.capture.note(p.projection->Header(), Source::Screen);
    } else {
        for (const auto& quad : p.quads)
            p.capture.note(reinterpret_cast<const XrCompositionLayerBaseHeader*>(&quad), Source::Screen);
    }
    if (p.status) p.capture.note(p.status->Header(), Source::Hud);
    p.capture.compose(eyes);
    p.Reset();
}

} // namespace Vulkan::OpenXr
