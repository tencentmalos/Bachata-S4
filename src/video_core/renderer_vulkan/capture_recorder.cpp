// SPDX-License-Identifier: GPL-2.0-or-later
#ifndef VK_USE_PLATFORM_ANDROID_KHR
#define VK_USE_PLATFORM_ANDROID_KHR
#endif
#include "video_core/renderer_vulkan/capture_recorder.h"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cctype>
#include <filesystem>
#include <limits>
#include <mutex>
#include <sstream>
#include <stdexcept>

#include "common/path_util.h"
#include "video_core/renderer_vulkan/vk_instance.h"
#include "video_core/renderer_vulkan/vk_presenter.h"
#include "video_core/renderer_vulkan/vk_scheduler.h"
#include "scrcpy_capture/encoder_session.h"

namespace Vulkan {
namespace {
struct ControlState {
    std::mutex mutex;
    std::atomic<u64> revision{};
    bool wanted{};
    bool live{};
    std::string live_token;
    std::string phase{"idle"};
    std::string path;
    std::string error;
    u64 submitted{};
    u64 skipped{};
    scrcpy::capture::EncoderStats encoder{};
};
ControlState& Control() {
    static ControlState state;
    return state;
}
std::string ResultString(vk::Result result) {
    return vk::to_string(result);
}
}  // namespace

std::string HandleEmbeddedCaptureCommand(const std::vector<std::string>& args) {
    auto& state = Control();
    std::scoped_lock lock(state.mutex);
    const std::string action = args.empty() ? "status" : args.front();
    const bool start_file = action == "start" && args.size() == 1;
    const bool start_live = action == "start_live" && args.size() == 2 &&
        args[1].size() == 32 &&
        std::all_of(args[1].begin(), args[1].end(), [](unsigned char c) {
            return std::isdigit(c) || (c >= 'a' && c <= 'f');
        });
    if (start_file || start_live) {
        if (!state.wanted) {
            state.wanted = true;
            state.live = start_live;
            state.live_token = start_live ? args[1] : std::string{};
            ++state.revision;
            state.phase = "pending";
            state.error.clear();
            state.path.clear();
            state.submitted = 0;
            state.skipped = 0;
            state.encoder = {};
        }
    } else if (action == "stop" && args.size() == 1) {
        if (state.wanted) {
            state.wanted = false;
            ++state.revision;
            state.phase = "stopping";
        }
    } else if (action != "status" || args.size() > 1) {
        return "status: bad-arguments\nusage: capture_video [start|start_live TOKEN|stop|status]\n";
    }
    std::ostringstream out;
    out << "status: " << state.phase << '\n'
        << "revision: " << state.revision.load(std::memory_order_relaxed) << '\n'
        << "source: shadps4.final_render_target\n"
        << "mode: " << (state.live ? "live" : "file") << '\n'
        << "view: canvas\n"
        << "submitted: " << state.submitted << '\n'
        << "skipped: " << state.skipped << '\n'
        << "encoded_samples: " << state.encoder.encoded_samples << '\n'
        << "encoded_bytes: " << state.encoder.encoded_bytes << '\n'
        << "first_codec_pts_us: " << state.encoder.first_pts_us << '\n'
        << "last_codec_pts_us: " << state.encoder.last_pts_us << '\n'
        << "metadata_association: codec_pts_only\n"
        << "eos: " << (state.encoder.eos ? "true" : "false") << '\n';
    if (!state.path.empty()) {
        out << "file: " << state.path << '\n'
            << "metadata_file: " << state.path << ".frames.csv\n";
    }
    if (!state.error.empty()) out << "error: " << state.error << '\n';
    return out.str();
}

class CaptureRecorder::Impl {
public:
    explicit Impl(const Instance& instance_) : instance(instance_) {}

    void SyncRequest(const Frame& frame, vk::Format source_format, u32 frame_pool_size) {
        if (Control().revision.load(std::memory_order_acquire) == seen_revision &&
            (!active || (frame.width == width && frame.height == height &&
                         source_format == format))) return;
        u64 new_revision{};
        bool new_wanted{}, new_live{};
        std::string new_token;
        {
            auto& state = Control();
            std::scoped_lock lock(state.mutex);
            new_revision = state.revision.load(std::memory_order_relaxed);
            new_wanted = state.wanted;
            new_live = state.live;
            new_token = state.live_token;
        }
        if (new_revision == seen_revision && (!active ||
            (frame.width == width && frame.height == height && source_format == format))) return;
        Close();
        seen_revision = new_revision;
        if (!new_wanted) {
            SetPhase(encoder.Stats().failed ? "failed" : "stopped", encoder.Error());
            return;
        }
        if (frame.width == 0 || frame.height == 0 ||
            source_format != vk::Format::eR8G8B8A8Unorm &&
            source_format != vk::Format::eB8G8R8A8Unorm &&
            source_format != vk::Format::eR8G8B8A8Srgb &&
            source_format != vk::Format::eB8G8R8A8Srgb) {
            SetPhase("failed", "unsupported final render target format or dimensions");
            return;
        }
        width = frame.width;
        height = frame.height;
        format = source_format;
        try {
            const auto path = new_live ? std::filesystem::path{} :
                Common::FS::GetUserPath(Common::FS::PathType::CapturesDir) /
                ("embedded-" + std::to_string(std::chrono::steady_clock::now()
                    .time_since_epoch().count()) + ".h264");
            scrcpy::capture::EncoderConfig config{.width = static_cast<int>(width),
                .height = static_cast<int>(height), .fps = 60, .bitrate = 8'000'000,
                .output_path = path.string(), .live_token = new_token};
            std::string error;
            if (!encoder.Start(config, &error)) throw std::runtime_error(error);
            const vk::AndroidSurfaceCreateInfoKHR surface_info{
                .window = encoder.InputWindow(),
            };
            auto [surface_result, new_surface] = instance.GetInstance().createAndroidSurfaceKHR(surface_info);
            if (surface_result != vk::Result::eSuccess)
                throw std::runtime_error("encoder VkSurface: " + ResultString(surface_result));
            surface = new_surface;
            auto [support_result, supported] = instance.GetPhysicalDevice().getSurfaceSupportKHR(
                instance.GetGraphicsQueueFamilyIndex(), surface);
            if (support_result != vk::Result::eSuccess || !supported)
                throw std::runtime_error("encoder Surface not presentable by graphics queue");
            auto [caps_result, caps] = instance.GetPhysicalDevice().getSurfaceCapabilitiesKHR(surface);
            if (caps_result != vk::Result::eSuccess ||
                !(caps.supportedUsageFlags & vk::ImageUsageFlagBits::eTransferDst))
                throw std::runtime_error("encoder Surface lacks transfer-dst usage");
            auto [formats_result, formats] = instance.GetPhysicalDevice().getSurfaceFormatsKHR(surface);
            if (formats_result != vk::Result::eSuccess || formats.empty())
                throw std::runtime_error("encoder Surface has no formats");
            vk::SurfaceFormatKHR selected = formats.front();
            for (auto candidate : formats) {
                if (candidate.format == vk::Format::eR8G8B8A8Unorm ||
                    candidate.format == vk::Format::eB8G8R8A8Unorm) {
                    selected = candidate;
                    break;
                }
            }
            if (selected.format != vk::Format::eR8G8B8A8Unorm &&
                selected.format != vk::Format::eB8G8R8A8Unorm)
                throw std::runtime_error("encoder Surface has no SDR 8-bit format");
            const vk::Extent2D requested{width, height};
            const auto extent = caps.currentExtent.width == std::numeric_limits<u32>::max()
                ? requested : caps.currentExtent;
            if (extent != requested) throw std::runtime_error("encoder Surface extent differs from RT");
            const u32 image_count = caps.maxImageCount == 0 ? caps.minImageCount + 1 :
                std::min(caps.minImageCount + 1, caps.maxImageCount);
            const auto alpha = (caps.supportedCompositeAlpha & vk::CompositeAlphaFlagBitsKHR::eOpaque)
                ? vk::CompositeAlphaFlagBitsKHR::eOpaque : vk::CompositeAlphaFlagBitsKHR::eInherit;
            const vk::SwapchainCreateInfoKHR create_info{
                .surface = surface,
                .minImageCount = image_count,
                .imageFormat = selected.format,
                .imageColorSpace = selected.colorSpace,
                .imageExtent = extent,
                .imageArrayLayers = 1,
                .imageUsage = vk::ImageUsageFlagBits::eTransferDst,
                .imageSharingMode = vk::SharingMode::eExclusive,
                .preTransform = caps.currentTransform,
                .compositeAlpha = alpha,
                .presentMode = vk::PresentModeKHR::eFifo,
                .clipped = true,
            };
            auto [chain_result, chain] = instance.GetDevice().createSwapchainKHR(create_info);
            if (chain_result != vk::Result::eSuccess)
                throw std::runtime_error("encoder swapchain: " + ResultString(chain_result));
            swapchain = chain;
            auto [images_result, chain_images] = instance.GetDevice().getSwapchainImagesKHR(swapchain);
            if (images_result != vk::Result::eSuccess || chain_images.empty())
                throw std::runtime_error("encoder swapchain has no images");
            images = std::move(chain_images);
            image_acquired.resize(frame_pool_size);
            image_ready.resize(images.size());
            for (auto& semaphore : image_acquired) {
                auto [result, value] = instance.GetDevice().createSemaphore({});
                if (result != vk::Result::eSuccess) throw std::runtime_error("encoder acquire semaphore");
                semaphore = value;
            }
            for (auto& semaphore : image_ready) {
                auto [result, value] = instance.GetDevice().createSemaphore({});
                if (result != vk::Result::eSuccess) throw std::runtime_error("encoder present semaphore");
                semaphore = value;
            }
            active = true;
            {
                auto& state = Control();
                std::scoped_lock lock(state.mutex);
                state.path = path.string();
                state.submitted = 0;
                state.skipped = 0;
                state.encoder = {};
            }
            SetPhase("recording", {});
        } catch (const std::exception& error) {
            Close();
            SetPhase("failed", error.what());
        }
    }

    bool Acquire(const Frame& frame) {
        if (!active || frame.id >= image_acquired.size()) return false;
        // Zero timeout keeps video backpressure out of the presentation path.
        auto [result, index] = instance.GetDevice().acquireNextImageKHR(
            swapchain, 0, image_acquired[frame.id], VK_NULL_HANDLE);
        if (result == vk::Result::eNotReady || result == vk::Result::eTimeout) {
            std::scoped_lock lock(Control().mutex);
            ++Control().skipped;
            return false;
        }
        if (result != vk::Result::eSuccess && result != vk::Result::eSuboptimalKHR) {
            SetPhase("failed", "encoder acquire: " + ResultString(result));
            return false;
        }
        image_index = index;
        frame_index = frame.id;
        acquired = true;
        return true;
    }

    void Record(vk::CommandBuffer cmd, const Frame& frame) {
        if (!acquired) return;
        const vk::ImageSubresourceRange range{vk::ImageAspectFlagBits::eColor, 0, 1, 0, 1};
        const std::array before{
            vk::ImageMemoryBarrier2{
                .srcStageMask = vk::PipelineStageFlagBits2::eFragmentShader,
                .srcAccessMask = vk::AccessFlagBits2::eShaderRead,
                .dstStageMask = vk::PipelineStageFlagBits2::eTransfer,
                .dstAccessMask = vk::AccessFlagBits2::eTransferRead,
                .oldLayout = vk::ImageLayout::eShaderReadOnlyOptimal,
                .newLayout = vk::ImageLayout::eTransferSrcOptimal,
                .image = frame.image,
                .subresourceRange = range,
            },
            vk::ImageMemoryBarrier2{
                .dstStageMask = vk::PipelineStageFlagBits2::eTransfer,
                .dstAccessMask = vk::AccessFlagBits2::eTransferWrite,
                .oldLayout = vk::ImageLayout::eUndefined,
                .newLayout = vk::ImageLayout::eTransferDstOptimal,
                .image = images[image_index],
                .subresourceRange = range,
            },
        };
        cmd.pipelineBarrier2(vk::DependencyInfo{.imageMemoryBarrierCount = before.size(),
            .pImageMemoryBarriers = before.data()});
        const vk::ImageBlit blit{
            .srcSubresource = {vk::ImageAspectFlagBits::eColor, 0, 0, 1},
            .srcOffsets = std::array{vk::Offset3D{0, 0, 0}, vk::Offset3D{static_cast<s32>(width), static_cast<s32>(height), 1}},
            .dstSubresource = {vk::ImageAspectFlagBits::eColor, 0, 0, 1},
            .dstOffsets = std::array{vk::Offset3D{0, 0, 0}, vk::Offset3D{static_cast<s32>(width), static_cast<s32>(height), 1}},
        };
        cmd.blitImage(frame.image, vk::ImageLayout::eTransferSrcOptimal,
            images[image_index], vk::ImageLayout::eTransferDstOptimal, blit, vk::Filter::eNearest);
        const std::array after{
            vk::ImageMemoryBarrier2{
                .srcStageMask = vk::PipelineStageFlagBits2::eTransfer,
                .srcAccessMask = vk::AccessFlagBits2::eTransferRead,
                .dstStageMask = vk::PipelineStageFlagBits2::eFragmentShader,
                .dstAccessMask = vk::AccessFlagBits2::eShaderRead,
                .oldLayout = vk::ImageLayout::eTransferSrcOptimal,
                .newLayout = vk::ImageLayout::eShaderReadOnlyOptimal,
                .image = frame.image,
                .subresourceRange = range,
            },
            vk::ImageMemoryBarrier2{
                .srcStageMask = vk::PipelineStageFlagBits2::eTransfer,
                .srcAccessMask = vk::AccessFlagBits2::eTransferWrite,
                .oldLayout = vk::ImageLayout::eTransferDstOptimal,
                .newLayout = vk::ImageLayout::ePresentSrcKHR,
                .image = images[image_index],
                .subresourceRange = range,
            },
        };
        cmd.pipelineBarrier2(vk::DependencyInfo{.imageMemoryBarrierCount = after.size(),
            .pImageMemoryBarriers = after.data()});
    }

    void AddSubmitSync(SubmitInfo& info) const {
        if (acquired) {
            info.AddWait(image_acquired[frame_index]);
            info.AddSignal(image_ready[image_index]);
        }
    }

    void Present() {
        if (!acquired) return;
        const vk::PresentInfoKHR info{
            .waitSemaphoreCount = 1,
            .pWaitSemaphores = &image_ready[image_index],
            .swapchainCount = 1,
            .pSwapchains = &swapchain,
            .pImageIndices = &image_index,
        };
        const auto result = instance.GetPresentQueue().presentKHR(info);
        acquired = false;
        if (result == vk::Result::eSuccess || result == vk::Result::eSuboptimalKHR) {
            const auto stats = encoder.Stats();
            auto& state = Control();
            {
                std::scoped_lock lock(state.mutex);
                ++state.submitted;
                state.encoder = stats;
            }
            if (stats.failed) SetPhase("failed", encoder.Error());
        } else {
            SetPhase("failed", "encoder present: " + ResultString(result));
        }
    }

    void Close() {
        if (!surface && !swapchain && !encoder.InputWindow()) {
            encoder.Stop();
            return;
        }
        // The caller is the presentation owner. Submitted command buffers and
        // binary present semaphores must retire before destroying the swapchain.
        instance.DrainSubmissions();
        {
            std::scoped_lock lock(instance.QueueMutex());
            const auto idle_result = instance.GetDevice().waitIdle();
            if (idle_result != vk::Result::eSuccess)
                SetPhase("failed", "encoder queue idle: " + ResultString(idle_result));
        }
        for (auto semaphore : image_acquired) if (semaphore) instance.GetDevice().destroySemaphore(semaphore);
        for (auto semaphore : image_ready) if (semaphore) instance.GetDevice().destroySemaphore(semaphore);
        image_acquired.clear();
        image_ready.clear();
        images.clear();
        if (swapchain) instance.GetDevice().destroySwapchainKHR(swapchain);
        swapchain = nullptr;
        if (surface) instance.GetInstance().destroySurfaceKHR(surface);
        surface = nullptr;
        encoder.Stop();
        {
            auto& state = Control();
            std::scoped_lock lock(state.mutex);
            state.encoder = encoder.Stats();
        }
        active = false;
        acquired = false;
    }

    void SetPhase(std::string phase, std::string error) {
        auto& state = Control();
        std::scoped_lock lock(state.mutex);
        state.phase = std::move(phase);
        state.error = std::move(error);
    }

    const Instance& instance;
    scrcpy::capture::EncoderSession encoder;
    vk::SurfaceKHR surface{};
    vk::SwapchainKHR swapchain{};
    std::vector<vk::Image> images;
    std::vector<vk::Semaphore> image_acquired;
    std::vector<vk::Semaphore> image_ready;
    u64 seen_revision{};
    u32 width{}, height{};
    vk::Format format{};
    u32 frame_index{}, image_index{};
    bool active{}, acquired{};
};

CaptureRecorder::CaptureRecorder(const Instance& instance) : impl_(std::make_unique<Impl>(instance)) {}
CaptureRecorder::~CaptureRecorder() = default;
void CaptureRecorder::SyncRequest(const Frame& frame, vk::Format format, u32 frame_pool_size) {
    impl_->SyncRequest(frame, format, frame_pool_size);
}
bool CaptureRecorder::Acquire(const Frame& frame) { return impl_->Acquire(frame); }
void CaptureRecorder::Record(vk::CommandBuffer cmd, const Frame& frame) { impl_->Record(cmd, frame); }
void CaptureRecorder::AddSubmitSync(SubmitInfo& info) const { impl_->AddSubmitSync(info); }
void CaptureRecorder::Present() { impl_->Present(); }
void CaptureRecorder::Close() { impl_->Close(); }
}  // namespace Vulkan
