// SPDX-FileCopyrightText: Copyright 2024 shadPS4 Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#pragma once
#include "video_core/vr_frame.h"

#include <condition_variable>

#include "core/libraries/videoout/buffer.h"
#include "video_core/renderdoc.h"
#include "imgui/imgui_texture.h"
#include "imgui/status_layer.h"
#include "video_core/renderer_vulkan/host_passes/fsr_pass.h"
#include "video_core/renderer_vulkan/host_passes/pp_pass.h"
#include "video_core/renderer_vulkan/vk_instance.h"
#include "video_core/renderer_vulkan/vk_scheduler.h"
#include "video_core/renderer_vulkan/vk_swapchain.h"
#include "video_core/texture_cache/texture_cache.h"

namespace Frontend {
class Window;
}

namespace AmdGpu {
struct Liverpool;
}

namespace Vulkan {

#if defined(SHADPS4_HAS_SCRCPY_CAPTURE_SDK)
class CaptureRecorder;
#endif

struct Frame {
    u32 width;
    u32 height;
    VmaAllocation allocation;
    vk::Image image;
    vk::ImageView image_view;
    vk::Fence present_done;
    vk::Semaphore ready_semaphore;
    u64 ready_tick;
    bool is_hdr{false};
    u8 id{};

    ImTextureID imgui_texture;
};

enum SchedulerType {
    Draw,
    Present,
    CpuFlip,
};

class Rasterizer;

class Presenter {
public:
    Presenter(std::shared_ptr<Frontend::Window> window, AmdGpu::Liverpool* liverpool,
              DriverLease driver = {}, std::function<bool()> splash_visible = {});
    ~Presenter();

    // The session owner must call this before joining rendering workers.
    void RequestStop() {
        capture_binding.Stop();
        swapchain.RequestStop();
        free_cv.notify_all();
    }

    const auto& Diagnostics() const { return instance.Diagnostics(); }
    void CheckSubmissionHealth() const { instance.CheckSubmissionHealth(); }
    void DrainSubmissions() const { instance.DrainSubmissions(); }
    u64 CaptureGeneration() const { return capture_binding.Generation(); }

    HostPasses::PostProcessingPass::Settings& GetPPSettingsRef() {
        return pp_settings;
    }

    HostPasses::FsrPass::Settings& GetFsrSettingsRef() {
        return fsr_settings;
    }

    Frontend::Window& GetWindow() const {
        return *window;
    }

    std::shared_ptr<Frontend::Window> ShareWindow() const {
        return window;
    }

    Rasterizer& GetRasterizer() const {
        return *rasterizer.get();
    }

    bool IsHDRSupported() const {
        return swapchain.HasHDR();
    }

    void SetHDR(bool enable) {
        if (!IsHDRSupported()) {
            return;
        }
        swapchain.SetHDR(enable);
        pp_settings.hdr = enable ? 1 : 0;
    }

    VideoCore::Image& RegisterVideoOutSurface(
        const Libraries::VideoOut::BufferAttributeGroup& attribute, VAddr cpu_address) {
        vo_buffers_addr.emplace_back(cpu_address);
        auto desc = VideoCore::TextureCache::ImageDesc{attribute, cpu_address};
        const auto image_id = texture_cache.FindImage(desc);
        auto& image = texture_cache.GetImage(image_id);
        image.usage.vo_surface = 1u;
        return image;
    }

    bool IsVideoOutSurface(const AmdGpu::ColorBuffer& color_buffer) const;

    Frame* PrepareVrFrame(const VideoCore::VrFrameSource& source,
                          std::function<void(bool)> complete);
    Frame* PrepareFrame(const Libraries::VideoOut::BufferAttributeGroup& attribute,
                        VAddr cpu_address, u64 diagnostic_id = 0);

    Frame* PrepareBlankFrame(bool present_thread);

    bool Present(Frame* frame, bool is_reusing_frame = false);
    Frame* PrepareLastFrame();

private:
    Frame* GetRenderFrame();

    // Rebuilds the small FDM ring only when the output extent changes or the
    // user switches the global quality mode. The map itself is uniform: low
    // is 2x2 shading (1/4), medium is 2x1 (1/2), high is 1x1.
    void EnsureFdm(u32 width, u32 height);
    vk::ImageView RecordFdmUpload(Scheduler& scheduler, const Frame& frame);

    void RecreateFrame(Frame* frame, u32 width, u32 height);

    void SetExpectedGameSize(s32 width, s32 height);

private:
    float expected_ratio{1920.0 / 1080.0f};
    u32 expected_frame_width{1920};
    u32 expected_frame_height{1080};

    std::function<bool()> splash_visible;
    std::shared_ptr<Frontend::Window> window;
    Instance instance;
    spatial::foveation::vulkan::FragmentDensityImageRing fdm_ring;
    spatial::foveation::FragmentDensityMapDesc fdm_desc{};
    std::vector<u8> fdm_map;
    u64 fdm_map_hash{};
    u32 fdm_width{};
    u32 fdm_height{};
    u32 fdm_quality{0xffffffffU};
    bool fdm_ready{};
    VideoCore::CaptureBinding capture_binding;
#if defined(SHADPS4_HAS_SCRCPY_CAPTURE_SDK)
    std::unique_ptr<CaptureRecorder> embedded_capture;
#endif
    std::unique_ptr<ImGui::StatusLayer> status_layer;
    HostPasses::FsrPass fsr_pass;
    HostPasses::FsrPass::Settings fsr_settings{};
    HostPasses::PostProcessingPass::Settings pp_settings{};
    HostPasses::PostProcessingPass pp_pass;
    AmdGpu::Liverpool* liverpool;
    Scheduler draw_scheduler;
    Scheduler present_scheduler;
    Scheduler flip_scheduler;
    Swapchain swapchain;
    std::unique_ptr<Rasterizer> rasterizer;
    VideoCore::TextureCache& texture_cache;
    vk::UniqueCommandPool command_pool;
    std::vector<Frame> present_frames;
    std::queue<Frame*> free_queue;
    Frame* last_submit_frame{};
    std::mutex free_mutex;
    std::condition_variable free_cv;
    std::condition_variable_any frame_cv;
    std::optional<ImGui::RefCountedTexture> splash_img;
    std::vector<VAddr> vo_buffers_addr;
};

} // namespace Vulkan
