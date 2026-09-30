// SPDX-License-Identifier: GPL-2.0-or-later
// Establish the host Vulkan-Hpp configuration before Foundation headers.
#include "video_core/renderer_vulkan/vk_common.h"

#include <mutex>
#include "spatial/upscale/VulkanUpscaler.h"
#include "video_core/renderer_vulkan/host_passes/spatial_upscale.h"
#include "video_core/renderer_vulkan/vk_instance.h"
#include "video_core/renderer_vulkan/vk_scheduler.h"
namespace Vulkan::HostPasses {
namespace {
std::mutex config_mutex;
SpatialOptions config;
spatial::upscale::Statistics stats;
bool fdm_enabled{}, gaze_supported{}, gaze_fresh{};
uint64_t gaze_valid{}, gaze_invalid{};
vk::Extent2D output_recommended{}, output_limit{}, output_actual{};
} // namespace
void SetSpatialOptions(SpatialOptions value) {
    if (u32(value.filter) > 2 || u32(value.foveation) > 2 || u32(value.level) > 2 ||
        value.sharpness > 100 || u32(value.output_resolution) > 2)
        return;
    std::scoped_lock lock(config_mutex);
    config = value;
}
SpatialOptions GetSpatialOptions() {
    std::scoped_lock lock(config_mutex);
    return config;
}
void PublishGazeStatus(bool supported, bool fresh, uint64_t valid, uint64_t invalid) {
    std::scoped_lock lock(config_mutex);
    gaze_supported = supported;
    gaze_fresh = fresh;
    gaze_valid = valid;
    gaze_invalid = invalid;
}
void PublishXrOutputExtent(vk::Extent2D recommended, vk::Extent2D limit, vk::Extent2D actual) {
    std::scoped_lock lock(config_mutex);
    output_recommended = recommended;
    output_limit = limit;
    output_actual = actual;
}
std::string SpatialStatus() {
    std::scoped_lock lock(config_mutex);
    return fmt::format(
        "upscaler={} foveation={} level={} sharpness={} draws={} fdm_draws={} "
        "tracked_draws={} uploads={} unchanged={} fdm_enabled={} gaze_supported={} gaze_fresh={} "
        "gaze_valid={} gaze_invalid={} output_resolution={} eye_output={}x{} "
        "eye_recommended={}x{} eye_limit={}x{} scope=host-presentation\n",
        u32(config.filter), u32(config.foveation), u32(config.level), config.sharpness, stats.draws,
        stats.fdm_draws, stats.tracked_draws, stats.uploads, stats.unchanged, fdm_enabled,
        gaze_supported, gaze_fresh, gaze_valid, gaze_invalid, u32(config.output_resolution),
        output_actual.width, output_actual.height, output_recommended.width, output_recommended.height,
        output_limit.width, output_limit.height);
}
struct SpatialUpscalePass::Impl {
    spatial::upscale::VulkanUpscaler pass;
    explicit Impl(const Instance& i)
        : pass({.vulkan = {.instance = i.GetInstance(),
                           .physical_device = i.GetPhysicalDevice(),
                           .device = i.GetDevice(),
                           .vk_get_instance_proc_addr =
                               VULKAN_HPP_DEFAULT_DISPATCHER.vkGetInstanceProcAddr,
                           .vk_get_device_proc_addr =
                               VULKAN_HPP_DEFAULT_DISPATCHER.vkGetDeviceProcAddr,
                           .queue_submit_mutex = &i.QueueMutex()},
                .enabled_fdm = i.IsFdmSupported() ? i.FdmCapabilities()
                                                  : spatial::foveation::vulkan::Capabilities{}}) {}
};
SpatialUpscalePass::SpatialUpscalePass(const Instance& i) : impl(std::make_unique<Impl>(i)) {
    std::scoped_lock lock(config_mutex);
    stats = {};
    fdm_enabled = i.IsFdmSupported();
}
SpatialUpscalePass::~SpatialUpscalePass() = default;
vk::ImageView SpatialUpscalePass::Render(Scheduler& scheduler, u32 slot, vk::ImageView input,
                                         vk::Extent2D input_size, std::array<float, 4> uv,
                                         vk::Extent2D output_size, SpatialOptions options,
                                         FoveatedEye eye, bool encoded_input) {
    scheduler.EndRendering(RenderBreak::Present);
    const auto output = impl->pass.Render(
        scheduler.RawCommandBuffer(), slot, input, input_size, uv, output_size,
        {static_cast<spatial::upscale::Filter>(options.filter), options.sharpness, options.debug},
        {eye.center, eye.profile, eye.enabled, eye.tracked}, encoded_input);
    {
        std::scoped_lock lock(config_mutex);
        stats = impl->pass.Stats();
    }
    return output.view;
}
} // namespace Vulkan::HostPasses
