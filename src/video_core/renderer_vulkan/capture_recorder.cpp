// SPDX-License-Identifier: GPL-2.0-or-later
#include "video_core/renderer_vulkan/capture_recorder.h"

#include <mutex>

#include "common/path_util.h"
#include "spatial/capture/CaptureHub.h"
#include "video_core/renderer_vulkan/vk_instance.h"
#include "video_core/renderer_vulkan/vk_presenter.h"
#include "video_core/renderer_vulkan/vk_scheduler.h"

namespace Vulkan {

// shadPS4's names in answers (shadps4.final_render_target, shadps4.xr_composite), its A/B
// properties and its log tag.
void ConfigureEmbeddedCapture() {
    static std::once_flag once;
    std::call_once(once, [] {
        spatial::capture::Configure({.product = "shadps4",
                                     .canvasProducer = "final_render_target",
                                     .propertyPrefix = "debug.shadps4.capture",
                                     .logTag = "shadPS4"});
    });
}

std::string HandleEmbeddedCaptureCommand(const std::string& command, const std::vector<std::string>& args) {
    ConfigureEmbeddedCapture();
    const auto captures = Common::FS::GetUserPath(Common::FS::PathType::CapturesDir);
    return spatial::capture::HandleCommand(command, args, captures.string());
}

spatial::capture::VulkanBinding EmbeddedCaptureBinding(const Instance& instance, bool present_queue) {
    spatial::capture::VulkanBinding binding;
    binding.instance = static_cast<VkInstance>(instance.GetInstance());
    binding.physicalDevice = static_cast<VkPhysicalDevice>(instance.GetPhysicalDevice());
    binding.device = static_cast<VkDevice>(instance.GetDevice());
    binding.queue = static_cast<VkQueue>(present_queue ? instance.GetPresentQueue() : instance.GetGraphicsQueue());
    binding.queueFamily =
        present_queue ? instance.GetPresentQueueFamilyIndex() : instance.GetGraphicsQueueFamilyIndex();
    binding.getInstanceProcAddr = VULKAN_HPP_DEFAULT_DISPATCHER.vkGetInstanceProcAddr;
    binding.queueMutex = &instance.QueueMutex();
    // Android always enables VK_KHR_swapchain and VK_KHR_android_surface (vk_platform, vk_instance).
    binding.surfaces = true;
    // The presenter's work goes through the submission thread; the recorder closed it the same way.
    if (present_queue) binding.drain = [&instance] { instance.DrainSubmissions(); };
    return binding;
}

CaptureRecorder::CaptureRecorder(const Instance& instance) {
    ConfigureEmbeddedCapture();
    frames = std::make_unique<spatial::capture::FrameCapture>(EmbeddedCaptureBinding(instance, true));
}

CaptureRecorder::~CaptureRecorder() = default;

void CaptureRecorder::SyncRequest(const Frame& frame, vk::Format source_format, u32 frame_pool_size) {
    // The presenter runs at up to 60 fps; capture_config max_fps lowers the encoded rate.
    frames->sync(frame.width, frame.height, frame.xr_stereo ? 2 : 1, static_cast<VkFormat>(source_format),
                 frame_pool_size, 60);
}

bool CaptureRecorder::Acquire(const Frame& frame) {
    return frames->acquire(frame.id);
}

void CaptureRecorder::Record(vk::CommandBuffer cmd, const Frame& frame) {
    // The final render target is sampled by the presentation pass before and after.
    frames->record(static_cast<VkCommandBuffer>(cmd), static_cast<VkImage>(frame.image),
                   VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL, VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT,
                   VK_ACCESS_SHADER_READ_BIT);
}

void CaptureRecorder::AddSubmitSync(SubmitInfo& info) const {
    const auto [wait, signal] = SubmitSemaphores();
    if (wait) info.AddWait(wait);
    if (signal) info.AddSignal(signal);
}

std::pair<vk::Semaphore, vk::Semaphore> CaptureRecorder::SubmitSemaphores() const {
    return {vk::Semaphore{frames->waitSemaphore()}, vk::Semaphore{frames->signalSemaphore()}};
}

void CaptureRecorder::Present() {
    frames->present();
}

void CaptureRecorder::Close() {
    frames->close();
}

std::optional<spatial::capture::FrameStill> CaptureRecorder::TakeStill() {
    return frames->takeStill();
}

void CaptureRecorder::StillDone(const std::string& token, u32 width, u32 height, const std::string& error,
                                u32 views) {
    spatial::capture::FrameCapture::stillDone(token, width, height, error, views);
}

} // namespace Vulkan
