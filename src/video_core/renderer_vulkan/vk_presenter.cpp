// SPDX-FileCopyrightText: Copyright 2024-2026 shadPS4 Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#include <stdexcept>
#include "common/debug.h"
#include "core/diagnostics/pipeline_handoff.h"
#include "common/elf_info.h"
#include "common/io_file.h"
#include "common/path_util.h"
#include "common/singleton.h"
#include "core/debug_state.h"
#include "core/host_runtime/guest_vr_sensor.h"
#include "core/devtools/layer.h"
#include "core/emulator_settings.h"
#include "core/libraries/system/systemservice.h"
#include "frontend/window.h"
#include "imgui/friends_layer.h"
#include "imgui/invitation_prompt_layer.h"
#include "imgui/notifications_layer.h"
#include "imgui/renderer/imgui_core.h"
#include "imgui/renderer/imgui_impl_vulkan.h"
#include "imgui/shadnet_notifications_layer.h"
#include "video_core/buffer_cache/buffer.h"
#include "video_core/renderdoc.h"
#include "video_core/renderer_vulkan/vk_platform.h"
#include "video_core/renderer_vulkan/vk_presenter.h"
#if defined(SHADPS4_HAS_SCRCPY_CAPTURE_SDK)
#include "video_core/renderer_vulkan/capture_recorder.h"
#endif
#include "shader_recompiler/resource.h"
#include "video_core/buffer_cache/buffer_cache.h"
#include "video_core/renderer_vulkan/vk_rasterizer.h"
#include "video_core/texture_cache/image.h"

#include <algorithm>
#include <array>
#include <atomic>
#include <cctype>
#include <chrono>
#include <cmath>
#include <csetjmp>
#include <cstring>
#include <ctime>
#include <filesystem>
#include <fstream>
#include <iomanip>
#include <limits>
#include <memory>
#include <span>
#include <sstream>
#include <system_error>
#include <vector>
#include <imgui.h>
#include <stb_image_write.h>
#include <vk_mem_alloc.h>
#include "video_core/vma_diagnostics.h"

namespace Vulkan {

bool CanBlitToSwapchain(const vk::PhysicalDevice physical_device, vk::Format format) {
    const vk::FormatProperties props{physical_device.getFormatProperties(format)};
    return static_cast<bool>(props.optimalTilingFeatures & vk::FormatFeatureFlagBits::eBlitDst);
}

[[nodiscard]] vk::ImageSubresourceLayers MakeImageSubresourceLayers() {
    return vk::ImageSubresourceLayers{
        .aspectMask = vk::ImageAspectFlagBits::eColor,
        .mipLevel = 0,
        .baseArrayLayer = 0,
        .layerCount = 1,
    };
}

[[nodiscard]] vk::ImageBlit MakeImageBlit(s32 frame_width, s32 frame_height, s32 dst_width,
                                          s32 dst_height, s32 offset_x, s32 offset_y) {
    return vk::ImageBlit{
        .srcSubresource = MakeImageSubresourceLayers(),
        .srcOffsets =
            std::array{
                vk::Offset3D{
                    .x = 0,
                    .y = 0,
                    .z = 0,
                },
                vk::Offset3D{
                    .x = frame_width,
                    .y = frame_height,
                    .z = 1,
                },
            },
        .dstSubresource = MakeImageSubresourceLayers(),
        .dstOffsets =
            std::array{
                vk::Offset3D{
                    .x = offset_x,
                    .y = offset_y,
                    .z = 0,
                },
                vk::Offset3D{
                    .x = offset_x + dst_width,
                    .y = offset_y + dst_height,
                    .z = 1,
                },
            },
    };
}

[[nodiscard]] vk::ImageBlit MakeImageBlitStretch(s32 frame_width, s32 frame_height,
                                                 s32 swapchain_width, s32 swapchain_height) {
    return MakeImageBlit(frame_width, frame_height, swapchain_width, swapchain_height, 0, 0);
}

static vk::Rect2D FitImage(s32 frame_width, s32 frame_height, s32 swapchain_width,
                           s32 swapchain_height) {
    float frame_aspect = static_cast<float>(frame_width) / frame_height;
    float swapchain_aspect = static_cast<float>(swapchain_width) / swapchain_height;

    u32 dst_width = swapchain_width;
    u32 dst_height = swapchain_height;

    if (frame_aspect > swapchain_aspect) {
        dst_height = static_cast<s32>(swapchain_width / frame_aspect);
    } else {
        dst_width = static_cast<s32>(swapchain_height * frame_aspect);
    }

    const s32 offset_x = (swapchain_width - dst_width) / 2;
    const s32 offset_y = (swapchain_height - dst_height) / 2;

    return vk::Rect2D{{offset_x, offset_y}, {dst_width, dst_height}};
}

[[nodiscard]] vk::ImageBlit MakeImageBlitFit(s32 frame_width, s32 frame_height, s32 swapchain_width,
                                             s32 swapchain_height) {
    const auto& dst_rect = FitImage(frame_width, frame_height, swapchain_width, swapchain_height);

    return MakeImageBlit(frame_width, frame_height, dst_rect.extent.width, dst_rect.extent.height,
                         dst_rect.offset.x, dst_rect.offset.y);
}

enum class ScreenshotKind : u8 {
    GameOnly,
    WithOverlays,
};

struct ScreenshotReadback {
    ScreenshotKind kind{};
    std::vector<std::filesystem::path> paths{};
    VideoCore::Buffer buffer;
    u32 width{};
    u32 height{};
    vk::Format format{};
    bool hdr_encoded{};

    ScreenshotReadback(const Instance& instance, Scheduler& scheduler, ScreenshotKind kind_,
                       std::vector<std::filesystem::path> paths_, const u32 width_,
                       const u32 height_, const vk::Format format_, const bool hdr_encoded_)
        : kind{kind_}, paths{std::move(paths_)},
          buffer{instance,
                 scheduler,
                 VideoCore::MemoryUsage::Download,
                 0,
                 vk::BufferUsageFlagBits::eTransferDst,
                 static_cast<u64>(width_) * static_cast<u64>(height_) * 4},
          width{width_}, height{height_}, format{format_}, hdr_encoded{hdr_encoded_} {}
};

static std::string SanitizeFilenameComponent(std::string value) {
    for (char& c : value) {
        const unsigned char uc = static_cast<unsigned char>(c);
        if (!std::isalnum(uc) && c != '_' && c != '-') {
            c = '_';
        }
    }
    if (value.empty()) {
        return "UNKNOWN";
    }
    return value;
}

static std::vector<std::filesystem::path> BuildScreenshotPaths(const ScreenshotKind kind,
                                                               const u32 count) {
    static std::atomic<u64> screenshot_sequence{0};
    std::vector<std::filesystem::path> paths{};
    if (count == 0) {
        return paths;
    }

    const auto& screenshots_dir = Common::FS::GetUserPath(Common::FS::PathType::ScreenshotsDir);
    std::filesystem::create_directories(screenshots_dir);

    const auto game_id =
        SanitizeFilenameComponent(std::string(Common::ElfInfo::Instance().GameSerial()));
    const auto now = std::chrono::system_clock::now();
    const auto now_time = std::chrono::system_clock::to_time_t(now);
    const auto ms =
        std::chrono::duration_cast<std::chrono::milliseconds>(now.time_since_epoch()).count() %
        1000;

    std::tm local_tm{};
#ifdef _WIN32
    localtime_s(&local_tm, &now_time);
#else
    localtime_r(&now_time, &local_tm);
#endif

    std::ostringstream stamp;
    stamp << std::put_time(&local_tm, "%Y%m%d_%H%M%S") << '_' << std::setw(3) << std::setfill('0')
          << ms;

    const char* suffix = kind == ScreenshotKind::GameOnly ? "game" : "hud";
    const auto first_sequence = screenshot_sequence.fetch_add(count, std::memory_order_relaxed);

    paths.reserve(count);
    const auto stamp_str = stamp.str();
    for (u32 i = 0; i < count; ++i) {
        paths.emplace_back(screenshots_dir / fmt::format("{}_{}_{}_{:06}.png", game_id, stamp_str,
                                                         suffix, first_sequence + i));
    }

    return paths;
}

static float PqToNits(const float encoded) {
    // ST.2084 inverse EOTF
    constexpr float m1 = 2610.0f / 16384.0f;
    constexpr float m2 = 2523.0f / 32.0f;
    constexpr float c1 = 3424.0f / 4096.0f;
    constexpr float c2 = 2413.0f / 128.0f;
    constexpr float c3 = 2392.0f / 128.0f;

    const float v = std::clamp(encoded, 0.0f, 1.0f);
    const float vp = std::pow(v, 1.0f / m2);
    const float num = std::max(vp - c1, 0.0f);
    const float den = std::max(c2 - c3 * vp, 1e-6f);
    return 10000.0f * std::pow(num / den, 1.0f / m1);
}

static float ToneMapToSdrLinear(const float nits) {
    // Map absolute HDR luminance into SDR [0,1], preserving 100-nit white.
    constexpr float sdr_white_nits = 100.0f;
    const float x = std::max(nits, 0.0f) / sdr_white_nits;
    const float mapped = (2.0f * x) / (1.0f + x);
    return std::clamp(mapped, 0.0f, 1.0f);
}

static float LinearToSrgb(const float linear) {
    const float x = std::clamp(linear, 0.0f, 1.0f);
    if (x <= 0.0031308f) {
        return 12.92f * x;
    }
    return 1.055f * std::pow(x, 1.0f / 2.4f) - 0.055f;
}

static const std::array<float, 1024>& GetPqDecodeNitsLut() {
    static const std::array<float, 1024> lut = [] {
        std::array<float, 1024> values{};
        for (size_t i = 0; i < values.size(); ++i) {
            values[i] = PqToNits(static_cast<float>(i) / 1023.0f);
        }
        return values;
    }();
    return lut;
}

static const std::array<u8, 1024>& GetUnorm10ToU8Lut() {
    static const std::array<u8, 1024> lut = [] {
        std::array<u8, 1024> values{};
        for (size_t i = 0; i < values.size(); ++i) {
            values[i] = static_cast<u8>((i * 255u + 511u) / 1023u);
        }
        return values;
    }();
    return lut;
}

static void CopyImageToReadback(const vk::CommandBuffer& cmdbuf, const vk::Image image,
                                const vk::ImageLayout layout, ScreenshotReadback& readback) {
    const vk::BufferImageCopy copy_region = {
        .bufferOffset = 0,
        .bufferRowLength = 0,
        .bufferImageHeight = 0,
        .imageSubresource =
            {
                .aspectMask = vk::ImageAspectFlagBits::eColor,
                .mipLevel = 0,
                .baseArrayLayer = 0,
                .layerCount = 1,
            },
        .imageOffset = {0, 0, 0},
        .imageExtent = {readback.width, readback.height, 1},
    };
    cmdbuf.copyImageToBuffer(image, layout, readback.buffer.Handle(), copy_region);
}

static bool ConvertReadbackToRgba8(const ScreenshotReadback& readback, std::vector<u8>& out_rgba) {
    const u64 pixel_count = static_cast<u64>(readback.width) * static_cast<u64>(readback.height);
    const u64 byte_size = pixel_count * 4;
    if (readback.buffer.mapped_data.size() < byte_size) {
        LOG_ERROR(Render_Vulkan, "Screenshot readback buffer size mismatch (have {}, need {})",
                  readback.buffer.mapped_data.size(), byte_size);
        return false;
    }

    const auto src =
        std::span<const u8>{readback.buffer.mapped_data.data(), static_cast<size_t>(byte_size)};
    out_rgba.resize(static_cast<size_t>(byte_size));

    switch (readback.format) {
    case vk::Format::eR8G8B8A8Unorm:
    case vk::Format::eR8G8B8A8Srgb:
        std::memcpy(out_rgba.data(), src.data(), out_rgba.size());
        for (u64 i = 0; i < pixel_count; ++i) {
            out_rgba[static_cast<size_t>(i) * 4 + 3] = 255;
        }
        return true;
    case vk::Format::eB8G8R8A8Unorm:
    case vk::Format::eB8G8R8A8Srgb:
        for (u64 i = 0; i < pixel_count; ++i) {
            const size_t o = static_cast<size_t>(i) * 4;
            out_rgba[o + 0] = src[o + 2];
            out_rgba[o + 1] = src[o + 1];
            out_rgba[o + 2] = src[o + 0];
            out_rgba[o + 3] = 255;
        }
        return true;
    case vk::Format::eA2R10G10B10UnormPack32: {
        const auto& pq_decode_lut = GetPqDecodeNitsLut();
        const auto& unorm10_to_u8 = GetUnorm10ToU8Lut();

        for (u64 i = 0; i < pixel_count; ++i) {
            const size_t o = static_cast<size_t>(i) * 4;
            const u32 packed = static_cast<u32>(src[o + 0]) | (static_cast<u32>(src[o + 1]) << 8) |
                               (static_cast<u32>(src[o + 2]) << 16) |
                               (static_cast<u32>(src[o + 3]) << 24);
            const u32 b = (packed >> 0) & 0x3FF;
            const u32 g = (packed >> 10) & 0x3FF;
            const u32 r = (packed >> 20) & 0x3FF;

            if (readback.hdr_encoded) {
                // Rec.2020 + PQ. Convert to SDR Rec.709 for PNG output.
                const float r2020 = pq_decode_lut[r];
                const float g2020 = pq_decode_lut[g];
                const float b2020 = pq_decode_lut[b];

                const float r709_nits = 1.6605f * r2020 - 0.5876f * g2020 - 0.0728f * b2020;
                const float g709_nits = -0.1246f * r2020 + 1.1329f * g2020 - 0.0083f * b2020;
                const float b709_nits = -0.0182f * r2020 - 0.1006f * g2020 + 1.1187f * b2020;

                const float r_srgb = LinearToSrgb(ToneMapToSdrLinear(r709_nits));
                const float g_srgb = LinearToSrgb(ToneMapToSdrLinear(g709_nits));
                const float b_srgb = LinearToSrgb(ToneMapToSdrLinear(b709_nits));

                out_rgba[o + 0] = static_cast<u8>(std::clamp(r_srgb, 0.0f, 1.0f) * 255.0f + 0.5f);
                out_rgba[o + 1] = static_cast<u8>(std::clamp(g_srgb, 0.0f, 1.0f) * 255.0f + 0.5f);
                out_rgba[o + 2] = static_cast<u8>(std::clamp(b_srgb, 0.0f, 1.0f) * 255.0f + 0.5f);
            } else {
                out_rgba[o + 0] = unorm10_to_u8[r];
                out_rgba[o + 1] = unorm10_to_u8[g];
                out_rgba[o + 2] = unorm10_to_u8[b];
            }
            out_rgba[o + 3] = 255;
        }
        return true;
    }
    case vk::Format::eA2B10G10R10UnormPack32: {
        const auto& pq_decode_lut = GetPqDecodeNitsLut();
        const auto& unorm10_to_u8 = GetUnorm10ToU8Lut();

        for (u64 i = 0; i < pixel_count; ++i) {
            const size_t o = static_cast<size_t>(i) * 4;
            const u32 packed = static_cast<u32>(src[o + 0]) | (static_cast<u32>(src[o + 1]) << 8) |
                               (static_cast<u32>(src[o + 2]) << 16) |
                               (static_cast<u32>(src[o + 3]) << 24);
            const u32 r = (packed >> 0) & 0x3FF;
            const u32 g = (packed >> 10) & 0x3FF;
            const u32 b = (packed >> 20) & 0x3FF;

            if (readback.hdr_encoded) {
                // HDR swapchain path is Rec.2020 + PQ. Convert to SDR Rec.709 for PNG output.
                const float r2020 = pq_decode_lut[r];
                const float g2020 = pq_decode_lut[g];
                const float b2020 = pq_decode_lut[b];

                const float r709_nits = 1.6605f * r2020 - 0.5876f * g2020 - 0.0728f * b2020;
                const float g709_nits = -0.1246f * r2020 + 1.1329f * g2020 - 0.0083f * b2020;
                const float b709_nits = -0.0182f * r2020 - 0.1006f * g2020 + 1.1187f * b2020;

                const float r_srgb = LinearToSrgb(ToneMapToSdrLinear(r709_nits));
                const float g_srgb = LinearToSrgb(ToneMapToSdrLinear(g709_nits));
                const float b_srgb = LinearToSrgb(ToneMapToSdrLinear(b709_nits));

                out_rgba[o + 0] = static_cast<u8>(std::clamp(r_srgb, 0.0f, 1.0f) * 255.0f + 0.5f);
                out_rgba[o + 1] = static_cast<u8>(std::clamp(g_srgb, 0.0f, 1.0f) * 255.0f + 0.5f);
                out_rgba[o + 2] = static_cast<u8>(std::clamp(b_srgb, 0.0f, 1.0f) * 255.0f + 0.5f);
            } else {
                out_rgba[o + 0] = unorm10_to_u8[r];
                out_rgba[o + 1] = unorm10_to_u8[g];
                out_rgba[o + 2] = unorm10_to_u8[b];
            }
            out_rgba[o + 3] = 255;
        }
        return true;
    }
    default:
        LOG_WARNING(Render_Vulkan, "Unsupported screenshot format: {}",
                    vk::to_string(readback.format));
        return false;
    }
}

static bool WritePng(const std::filesystem::path& path, const std::span<const u8> rgba,
                     const u32 width, const u32 height) {
    Common::FS::IOFile file(path, Common::FS::FileAccessMode::Create);
    if (!file.IsOpen()) {
        return false;
    }

    auto callback = [](void* context, void* data, int size) {
        const auto* f = static_cast<Common::FS::IOFile*>(context);
        f->WriteRaw<u8>(data, size);
    };
    return stbi_write_png_to_func(callback, &file, width, height, 4, rgba.data(), 0);
}

static void SavePendingScreenshots(const std::vector<ScreenshotReadback>& readbacks) {
    for (const auto& readback : readbacks) {
        if (readback.paths.empty()) {
            continue;
        }

        std::vector<u8> rgba;
        if (!ConvertReadbackToRgba8(readback, rgba)) {
            continue;
        }

        const auto& primary_path = readback.paths.front();
        if (!WritePng(primary_path, rgba, readback.width, readback.height)) {
            LOG_ERROR(Render_Vulkan, "Failed saving screenshot to {}", primary_path.string());
            continue;
        }

        LOG_INFO(Render_Vulkan, "Saved screenshot: {}", primary_path.string());

        std::ifstream file(primary_path, std::ios::binary);
        std::vector<u8> imgdata;
        if (file) {
            imgdata = std::vector<u8>(std::istreambuf_iterator<char>(file),
                                      std::istreambuf_iterator<char>());
        }
        shadNotifications::QueueNotification("Saved screenshot:\n" + primary_path.string(), 3.0f,
                                             shadNotifications::position::BottomRight, imgdata);

        for (size_t i = 1; i < readback.paths.size(); ++i) {
            const auto& path = readback.paths[i];
            std::error_code ec{};
            std::filesystem::copy_file(primary_path, path, std::filesystem::copy_options::none, ec);
            if (ec) {
                // Fallback for platforms/filesystems where copy_file can fail for transient
                // reasons.
                if (!WritePng(path, rgba, readback.width, readback.height)) {
                    LOG_ERROR(Render_Vulkan, "Failed saving screenshot to {}", path.string());
                    continue;
                }
            }

            LOG_INFO(Render_Vulkan, "Saved screenshot: {}", path.string());
            std::ifstream file(path, std::ios::binary);
            std::vector<u8> imgdata;
            if (file) {
                imgdata = std::vector<u8>(std::istreambuf_iterator<char>(file),
                                          std::istreambuf_iterator<char>());
            }
            shadNotifications::QueueNotification("Saved screenshot:\n" + path.string(), 3.0f,
                                                 shadNotifications::position::BottomRight, imgdata);
        }
    }
}

Presenter::Presenter(std::shared_ptr<Frontend::Window> window_, AmdGpu::Liverpool* liverpool_,
                     DriverLease driver, std::function<bool()> splash_visible_)
    : splash_visible(splash_visible_ ? std::move(splash_visible_)
                                     : Libraries::SystemService::IsSplashVisible),
      window{window_ ? std::move(window_)
                     : throw std::invalid_argument("Presenter requires a window")},
      liverpool{liverpool_},
      instance{*window, EmulatorSettings.GetGpuId(), EmulatorSettings.IsVkValidationEnabled(),
               EmulatorSettings.IsVkCrashDiagnosticEnabled(), std::move(driver)},
      draw_scheduler{instance}, present_scheduler{instance, GpuProfiler::Stage::Present},
      flip_scheduler{instance, GpuProfiler::Stage::Flip},
      swapchain{instance, *window},
      rasterizer{std::make_unique<Rasterizer>(instance, draw_scheduler, liverpool)},
      texture_cache{rasterizer->GetTextureCache()} {
    const auto& diag = instance.Diagnostics();
    status_layer = std::make_unique<ImGui::StatusLayer>(diag, instance.GpuTiming(), instance.ScalePolicy(),
                                                        texture_cache.Coverage());
    const u64 generation = diag ? diag->Generation() : 1;
    capture_binding.Bind(generation, static_cast<VkInstance>(instance.GetInstance()),
                         window->GetWindowInfo().render_surface, instance.GetDriverVersionName());
#if defined(SHADPS4_HAS_SCRCPY_CAPTURE_SDK)
    embedded_capture = std::make_unique<CaptureRecorder>(instance);
#endif
    const u32 num_images = swapchain.GetImageCount();
    const vk::Device device = instance.GetDevice();

    // Keep one frame exclusively for overlay redraws, in addition to the
    // in-flight pool. It must not also be acquired by the GPU producer.
    present_frames.resize(num_images + 1);
    for (u32 i = 0; i < present_frames.size(); i++) {
        Frame& frame = present_frames[i];
        frame.id = i;
        auto fence = Check<"create present done fence">(
            device.createFence({.flags = vk::FenceCreateFlagBits::eSignaled}));
        frame.present_done = fence;
        free_queue.push(&frame);
    }

    fsr_settings.enable = EmulatorSettings.IsFsrEnabled();
#if defined(__ANDROID__)
    // Keep the first Android SBS path on the guest VideoOut image directly.
    // FSR owns a separate storage-image chain and is enabled independently by
    // desktop settings; bypass it until the SBS source/view path is verified.
    fsr_settings.enable = false;
#endif
    fsr_settings.use_rcas = EmulatorSettings.IsRcasEnabled();
    fsr_settings.rcas_attenuation =
        static_cast<float>(EmulatorSettings.GetRcasAttenuation() / 1000.f);

    fsr_pass.Create(instance, instance.GetAllocator(), num_images);
    pp_pass.Create(instance, swapchain.GetSurfaceFormat().format);

    ImGui::Layer::AddLayer(Common::Singleton<Core::Devtools::Layer>::Instance());
    ImGui::Friends::Register();
    ImGui::ShadNetNotify::Register();
    ImGui::InvitationPrompt::Register();
}

Presenter::~Presenter() {
    capture_binding.Close();
    // Release any acquire that is (or becomes) blocked so teardown never wedges on
    // a surface the platform may already have taken (HN4 bounded-acquire stop).
    swapchain.RequestStop();

    ImGui::InvitationPrompt::Unregister();
    ImGui::ShadNetNotify::Unregister();
    ImGui::Friends::Unregister();
    ImGui::Layer::RemoveLayer(Common::Singleton<Core::Devtools::Layer>::Instance());

    try {
        draw_scheduler.Finish();
        present_scheduler.Finish();
        flip_scheduler.Finish();
        Check(draw_scheduler.RawCommandBuffer().reset());
        Check(present_scheduler.RawCommandBuffer().reset());
        Check(flip_scheduler.RawCommandBuffer().reset());
    } catch (const std::exception& error) {
        // A poisoned submission worker is reported to the Session owner. Do not
        // throw again from its teardown. Drain any accepted host calls first;
        // on failure the worker has already discarded its remaining captures.
        LOG_ERROR(Render_Vulkan, "Renderer drain after submission failure: {}", error.what());
        try { instance.DrainSubmissions(); } catch (const std::exception&) {}
        std::scoped_lock queue_lock{instance.QueueMutex()};
        const auto result = instance.GetDevice().waitIdle();
        if (result != vk::Result::eSuccess)
            LOG_ERROR(Render_Vulkan, "Device drain during failed teardown: {}", vk::to_string(result));
    }

#if defined(SHADPS4_HAS_SCRCPY_CAPTURE_SDK)
    embedded_capture->Close();
#endif

    const vk::Device device = instance.GetDevice();
    for (auto& frame : present_frames) {
        VideoCore::VmaDiagnostics::DestroyImage(instance.GetAllocator(), frame.image, frame.allocation);
        device.destroyImageView(frame.image_view);
        device.destroyFence(frame.present_done);
    }
}

bool Presenter::IsVideoOutSurface(const AmdGpu::ColorBuffer& color_buffer) const {
    return std::ranges::find(vo_buffers_addr, color_buffer.Address()) != vo_buffers_addr.cend();
}

void Presenter::RecreateFrame(Frame* frame, u32 width, u32 height) {
    const vk::Device device = instance.GetDevice();
    if (frame->imgui_texture) {
        ImGui::Vulkan::RemoveTexture(frame->imgui_texture);
    }
    if (frame->image_view) {
        device.destroyImageView(frame->image_view);
    }
    if (frame->image) {
        VideoCore::VmaDiagnostics::DestroyImage(instance.GetAllocator(), frame->image, frame->allocation);
    }

    const vk::Format format = swapchain.GetSurfaceFormat().format;
    const vk::ImageCreateInfo image_info = {
        .flags = vk::ImageCreateFlagBits::eMutableFormat,
        .imageType = vk::ImageType::e2D,
        .format = format,
        .extent = {width, height, 1},
        .mipLevels = 1,
        .arrayLayers = 1,
        .samples = vk::SampleCountFlagBits::e1,
        .usage = vk::ImageUsageFlagBits::eColorAttachment | vk::ImageUsageFlagBits::eTransferDst |
                 vk::ImageUsageFlagBits::eTransferSrc | vk::ImageUsageFlagBits::eSampled,
    };

    const VmaAllocationCreateInfo alloc_info = {
        .flags = VMA_ALLOCATION_CREATE_WITHIN_BUDGET_BIT,
        .usage = VMA_MEMORY_USAGE_AUTO_PREFER_DEVICE,
        .requiredFlags = 0,
        .preferredFlags = 0,
        .pool = VK_NULL_HANDLE,
        .pUserData = nullptr,
    };

    VkImage unsafe_image{};
    VkImageCreateInfo unsafe_image_info = static_cast<VkImageCreateInfo>(image_info);

    VkResult result = VideoCore::VmaDiagnostics::CreateImage(instance.GetAllocator(), &unsafe_image_info, &alloc_info,
                                     &unsafe_image, &frame->allocation, nullptr, "image/present");
    if (result != VK_SUCCESS) [[unlikely]] {
        LOG_CRITICAL(Render_Vulkan, "Failed allocating texture with error {}",
                     vk::to_string(vk::Result{result}));
        UNREACHABLE();
    }
    frame->image = vk::Image{unsafe_image};
    SetObjectName(device, frame->image, "Frame image #{}", frame->id);

    const vk::ImageViewCreateInfo view_info = {
        .image = frame->image,
        .viewType = vk::ImageViewType::e2D,
        .format = format,
        .subresourceRange{
            .aspectMask = vk::ImageAspectFlagBits::eColor,
            .baseMipLevel = 0,
            .levelCount = 1,
            .baseArrayLayer = 0,
            .layerCount = 1,
        },
    };
    auto view = Check<"create frame image view">(device.createImageView(view_info));
    frame->image_view = view;
    frame->width = width;
    frame->height = height;

    frame->imgui_texture = ImGui::Vulkan::AddTexture(view, vk::ImageLayout::eShaderReadOnlyOptimal);
    frame->is_hdr = swapchain.GetHDR();
}

void Presenter::EnsureFdm(u32 width, u32 height) {
    if (!instance.IsFdmSupported() || width == 0 || height == 0) {
        fdm_ready = false;
        return;
    }

    const u32 quality = std::min(EmulatorSettings.GetFdmQuality(), 2U);
    if (fdm_ready && fdm_width == width && fdm_height == height &&
        fdm_quality == quality) {
        return;
    }

    const auto& caps = instance.FdmCapabilities();
    fdm_desc = spatial::foveation::ResolveFdmDesc(
        width, height, caps.MinTexel(), caps.MaxTexel(), 32);
    if (!fdm_desc.IsValid()) {
        LOG_WARNING(Render_Vulkan, "FDM map descriptor is invalid for {}x{}", width, height);
        return;
    }

    const auto binding = spatial::foveation::vulkan::VulkanBinding{
        .instance = static_cast<VkInstance>(instance.GetInstance()),
        .physical_device = static_cast<VkPhysicalDevice>(instance.GetPhysicalDevice()),
        .device = static_cast<VkDevice>(instance.GetDevice()),
        .vk_get_instance_proc_addr = VULKAN_HPP_DEFAULT_DISPATCHER.vkGetInstanceProcAddr,
        .vk_get_device_proc_addr = VULKAN_HPP_DEFAULT_DISPATCHER.vkGetDeviceProcAddr,
        .queue_submit_mutex = &instance.QueueMutex(),
    };
    const bool recreate_ring = !fdm_ring.IsCreated() || fdm_width != width ||
                               fdm_height != height;
    if (recreate_ring) {
        fdm_ring.Destroy();
        if (!fdm_ring.Create(binding, fdm_desc, static_cast<u32>(present_frames.size()))) {
            LOG_WARNING(Render_Vulkan, "FDM ring creation failed for {}x{}", width, height);
            return;
        }
    }

    const uint8_t rate = quality == 0 ? 4 : (quality == 1 ? 2 : 1);
    const spatial::foveation::Profile profile{
        .inner_x = 1.0f,
        .inner_y = 1.0f,
        .outer_x = 1.0f,
        .outer_y = 1.0f,
        .mid_rate = rate,
        .outer_rate = rate,
    };
    fdm_map.resize(spatial::foveation::FragmentDensityMapByteSize(fdm_desc));
    fdm_map_hash = spatial::foveation::BuildFragmentDensityMap(
        fdm_desc, profile, std::span<const spatial::foveation::EyeRegion>{}, fdm_map);
    if (fdm_map_hash == 0) {
        LOG_WARNING(Render_Vulkan, "FDM map generation failed for {}x{}", width, height);
        fdm_map.clear();
        fdm_ready = false;
        return;
    }

    fdm_width = width;
    fdm_height = height;
    fdm_quality = quality;
    fdm_ready = true;
    LOG_INFO(Render_Vulkan, "FDM global quality={} rate={} target={}x{} map={}x{} texel={}",
             quality, rate, width, height, fdm_desc.width, fdm_desc.height, fdm_desc.texel);
}

vk::ImageView Presenter::RecordFdmUpload(Scheduler& scheduler, const Frame& frame) {
    EnsureFdm(frame.width, frame.height);
    if (!fdm_ready || frame.id >= fdm_ring.ImageCount()) {
        return {};
    }
    const auto result = fdm_ring.RecordUpload(
        static_cast<VkCommandBuffer>(scheduler.RawCommandBuffer()), frame.id, fdm_map,
        fdm_map_hash);
    if (result == spatial::foveation::vulkan::UploadResult::Failed) {
        LOG_WARNING(Render_Vulkan, "FDM map upload failed for frame {}", frame.id);
        return {};
    }
    return fdm_ring.ImageView(frame.id);
}

Frame* Presenter::PrepareLastFrame() {
    Common::Profiler::Scope redraw_scope{"Present.PrepareLastFrame"};
    if (last_submit_frame == nullptr) {
        return nullptr;
    }

    Frame* frame = last_submit_frame;

    while (true) {
        if (swapchain.StopRequested())
            return nullptr;
        vk::Result result;
        {
            Common::Profiler::Scope scope{"Present.RedrawFenceWait"};
            result = instance.GetDevice().waitForFences(frame->present_done, false, 50'000'000);
        }
        if (result == vk::Result::eSuccess) {
            break;
        }
        if (result == vk::Result::eTimeout) {
            continue;
        }
        ASSERT_MSG(result != vk::Result::eErrorDeviceLost,
                   "Device lost during waiting for a frame");
    }

    auto& scheduler = flip_scheduler;
    scheduler.EndRendering(Vulkan::RenderBreak::Present);
    const auto cmdbuf = scheduler.RawCommandBuffer();

    const auto frame_subresources = vk::ImageSubresourceRange{
        .aspectMask = vk::ImageAspectFlagBits::eColor,
        .baseMipLevel = 0,
        .levelCount = 1,
        .baseArrayLayer = 0,
        .layerCount = VK_REMAINING_ARRAY_LAYERS,
    };

    const auto pre_barrier =
        vk::ImageMemoryBarrier2{.srcStageMask = vk::PipelineStageFlagBits2::eColorAttachmentOutput,
                                .srcAccessMask = vk::AccessFlagBits2::eColorAttachmentRead,
                                .dstStageMask = vk::PipelineStageFlagBits2::eColorAttachmentOutput,
                                .dstAccessMask = vk::AccessFlagBits2::eColorAttachmentWrite,
                                .oldLayout = vk::ImageLayout::eShaderReadOnlyOptimal,
                                .newLayout = vk::ImageLayout::eGeneral,
                                .image = frame->image,
                                .subresourceRange{frame_subresources}};

    cmdbuf.pipelineBarrier2(vk::DependencyInfo{
        .imageMemoryBarrierCount = 1,
        .pImageMemoryBarriers = &pre_barrier,
    });

    // Flush frame creation commands.
    frame->ready_semaphore = scheduler.GetMasterSemaphore()->Handle();
    frame->ready_tick = scheduler.CurrentTick();
    SubmitInfo info{};
    scheduler.Flush(info);
    return frame;
}

static vk::Format GetFrameViewFormat(const Libraries::VideoOut::PixelFormat format) {
    switch (format) {
    case Libraries::VideoOut::PixelFormat::A8B8G8R8Srgb:
        // Match the mutable image's native RGBA sRGB format.  A view with a
        // different channel order can be created on desktop but Turnip may
        // legally expose zero samples for that reinterpretation.
        return vk::Format::eR8G8B8A8Srgb;
    case Libraries::VideoOut::PixelFormat::A8R8G8B8Srgb:
        // VideoOut images are allocated as the internal RGBA-compatible
        // format in ImageInfo; avoid a B8 reinterpretation that Turnip may
        // legally create but samples as zero. The channel order is restored
        // by FrameSwapsRedBlue instead.
        return vk::Format::eR8G8B8A8Srgb;
    case Libraries::VideoOut::PixelFormat::A2R10G10B10:
    case Libraries::VideoOut::PixelFormat::A2R10G10B10Srgb:
    case Libraries::VideoOut::PixelFormat::A2R10G10B10Bt2020Pq:
        return vk::Format::eA2R10G10B10UnormPack32;
    default:
        break;
    }
    UNREACHABLE_MSG("Unknown format={}", static_cast<u32>(format));
    return {};
}

// A8R8G8B8 is B,G,R,A in memory, and ImageInfo stores it in an RGBA image, so the
// image's red channel holds blue. The frame view keeps the image's own format and
// swaps red and blue in its component mapping.
static bool FrameSwapsRedBlue(const Libraries::VideoOut::PixelFormat format) {
    return format == Libraries::VideoOut::PixelFormat::A8R8G8B8Srgb;
}

Frame* Presenter::PrepareVrFrame(const VideoCore::VrFrameSource& source,
                                  std::function<void(bool)> complete) {
    const auto& eyes = source.eyes;
    const auto image_count = source.image_count;
    std::array<vk::Sampler, 4> samplers{};
    // Called only by the normal GPU command owner, after guest render completion.
    // Texture-cache views preserve the guest array slice and swizzle. No warp
    // shaders, intermediate eye copy or synchronous GPU/CPU readback are needed.
    std::array<vk::ImageView, 4> views{};
    vk::Extent2D eye_size{};
    static std::atomic<u32> vr_samples{};
    const u32 vr_sample = vr_samples.fetch_add(1, std::memory_order_relaxed);
    if (vr_sample < 8) {
        LOG_INFO(Render_Vulkan,
                 "SBS VR eye sample={} count={} L={:#x} {}x{} pitch={} type={} fmt={}/{} R={:#x} {}x{} pitch={} type={} fmt={}/{}",
                 vr_sample, image_count, eyes[0].base_address, eyes[0].width + 1,
                 eyes[0].height + 1, eyes[0].Pitch(), static_cast<u32>(eyes[0].GetType()),
                 eyes[0].data_format, eyes[0].num_format, eyes[1].base_address,
                 eyes[1].width + 1, eyes[1].height + 1, eyes[1].Pitch(),
                 static_cast<u32>(eyes[1].GetType()), eyes[1].data_format, eyes[1].num_format);
    }
    draw_scheduler.EndRendering(Vulkan::RenderBreak::Present);
    draw_scheduler.GpuProfile().Prepare(draw_scheduler.RawCommandBuffer());
    for (u32 i = 0; i < image_count; ++i) {
        VideoCore::TextureCache::ImageDesc desc{eyes[i], Shader::ImageResource{}};
        const auto id = texture_cache.FindImage(desc);
        // HMD submissions reference render targets that the guest GPU has just
        // written. Do not call UpdateImage here: that path may re-upload the
        // stale CPU backing store and erase the live Unity frame before the
        // direct SBS sample. The normal VideoOut path keeps its own CPU-dirty
        // synchronization in PrepareFrame.
        auto& image = texture_cache.GetImage(id);
        if (vr_sample < 8) {
            LOG_INFO(Render_Vulkan,
                     "SBS VR texture i={} id={} guest={:#x}/{:#x} image={}x{} pitch={} flags={:#x} usage_rt={} usage_vo={}",
                     i, id.index, image.info.guest_address, image.info.guest_size,
                     image.info.size.width, image.info.size.height, image.info.pitch,
                     static_cast<u32>(image.flags), static_cast<u32>(image.usage.render_target),
                     static_cast<u32>(image.usage.vo_surface));
            LOG_INFO(Render_Vulkan, "SBS VR source i={} uv={},{},{},{} sampler={:#x}/{:#x}",
                     i, source.uv[i][0], source.uv[i][1], source.uv[i][2], source.uv[i][3],
                     source.samplers[i].raw0, source.samplers[i].raw1);
        }
        image.Transit(vk::ImageLayout::eShaderReadOnlyOptimal,
                      vk::AccessFlagBits2::eShaderRead, {}, draw_scheduler.RawCommandBuffer());
        views[i] = *texture_cache.FindTexture(id, desc).image_view;
        samplers[i] = texture_cache.GetSampler(source.samplers[i], {});
        if (!i) eye_size = {u32(eyes[i].width + 1), u32(eyes[i].height + 1)};
    }
    auto* frame = GetRenderFrame();
    if (!frame) {
        if (vr_sample < 8) LOG_ERROR(Render_Vulkan, "SBS VR no presentation frame");
        complete(false);
        return nullptr;
    }
    const vk::ImageMemoryBarrier2 barrier{
        .srcStageMask = vk::PipelineStageFlagBits2::eAllCommands,
        .srcAccessMask = vk::AccessFlagBits2::eMemoryRead,
        .dstStageMask = vk::PipelineStageFlagBits2::eColorAttachmentOutput,
        .dstAccessMask = vk::AccessFlagBits2::eColorAttachmentWrite,
        .oldLayout = vk::ImageLayout::eUndefined,
        .newLayout = vk::ImageLayout::eColorAttachmentOptimal,
        .image = frame->image,
        .subresourceRange = {vk::ImageAspectFlagBits::eColor, 0, 1, 0, 1}};
    const auto cmdbuf = draw_scheduler.RawCommandBuffer();
    cmdbuf.pipelineBarrier2(vk::DependencyInfo{
        .imageMemoryBarrierCount = 1, .pImageMemoryBarriers = &barrier});
    auto settings = pp_settings;
    // Explicit per-eye coordinates apply even when both descriptors resolve
    // to the same view. Unity/UE can submit cropped dynamic-resolution atlases.
    settings.sbs = 8u | 16u | (image_count == 4 ? 2u : 0u);
    settings.eye_uv = source.uv;
    // The guest transform already carries the source's Y orientation (Unity
    // submits a negative scale; UE submits a positive one). Do not flip twice.
    settings.flip_y = 0;
    settings.srgb_input = 0;
    // Keep the guest-to-host composition at the existing single post-process
    // draw. Coarse shading is injected in guest Scheduler::BeginRendering;
    // applying FDM here would only reduce host post-processing work.
    pp_pass.Render(draw_scheduler, views[0], eye_size, *frame, settings,
                   {views[1], views[2], views[3]}, {}, samplers);
    expected_ratio = 16.0f / 9.0f;
    DebugState.game_resolution = {u32(float(eye_size.width) * std::abs(source.uv[0][0]) * 2.f), eye_size.height};
    DebugState.output_resolution = {frame->width, frame->height};
    frame->ready_semaphore = draw_scheduler.GetMasterSemaphore()->Handle();
    frame->ready_tick = draw_scheduler.CurrentTick();
    draw_scheduler.DeferPriorityOperation([complete = std::move(complete)] { complete(true); });
    draw_scheduler.GpuProfile().FrameEnd();
    SubmitInfo submit_info{};
    draw_scheduler.Flush(submit_info);
    return frame;
}

Frame* Presenter::PrepareFrame(const Libraries::VideoOut::BufferAttributeGroup& attribute,
                               VAddr cpu_address, u64 diagnostic_id) {
    auto desc = VideoCore::TextureCache::ImageDesc{attribute, cpu_address};
    const auto image_id = [&] {
        Common::Profiler::Scope scope{"Prepare.FindImage"};
        return texture_cache.FindImage(desc);
    }();
    {
        Common::Profiler::Scope scope{"Prepare.UpdateImage"};
        texture_cache.UpdateImage(image_id);
    }

    // Bounded Android SBS bring-up diagnostic: sample the guest VideoOut
    // backing store after the CPU-to-image upload.  This distinguishes a
    // genuinely black guest frame from a presenter/compositor that dropped a
    // non-black eye image, without changing the render path or retaining a
    // guest pointer.
    Frame* frame = GetRenderFrame();
    if (!frame)
        return nullptr;

    const auto frame_subresources = vk::ImageSubresourceRange{
        .aspectMask = vk::ImageAspectFlagBits::eColor,
        .baseMipLevel = 0,
        .levelCount = 1,
        .baseArrayLayer = 0,
        .layerCount = VK_REMAINING_ARRAY_LAYERS,
    };

    const auto pre_barrier = vk::ImageMemoryBarrier2{
        .srcStageMask = vk::PipelineStageFlagBits2::eColorAttachmentOutput,
        .srcAccessMask = vk::AccessFlagBits2::eColorAttachmentRead,
        .dstStageMask = vk::PipelineStageFlagBits2::eColorAttachmentOutput,
        .dstAccessMask = vk::AccessFlagBits2::eColorAttachmentWrite,
        .oldLayout = vk::ImageLayout::eUndefined,
        .newLayout = vk::ImageLayout::eColorAttachmentOptimal,
        .image = frame->image,
        .subresourceRange{frame_subresources},
    };

    {
        Common::Profiler::Scope scope{"Prepare.EndRendering"};
        draw_scheduler.EndRendering(Vulkan::RenderBreak::Present);
    }
    draw_scheduler.GpuProfile().Prepare(draw_scheduler.RawCommandBuffer());
    const auto cmdbuf = draw_scheduler.RawCommandBuffer();
    cmdbuf.pipelineBarrier2(vk::DependencyInfo{
        .imageMemoryBarrierCount = 1,
        .pImageMemoryBarriers = &pre_barrier,
    });

    VideoCore::ImageViewInfo view_info{};
    view_info.format = GetFrameViewFormat(attribute.attrib.pixel_format);
    // Exclude alpha from output frame to avoid blending with UI.
    view_info.mapping.a = vk::ComponentSwizzle::eOne;
    const bool swap_red_blue = FrameSwapsRedBlue(attribute.attrib.pixel_format);
    if (swap_red_blue) {
        view_info.mapping.r = vk::ComponentSwizzle::eB;
        view_info.mapping.b = vk::ComponentSwizzle::eR;
    }

    auto& image = texture_cache.GetImage(image_id);
    auto image_view = [&] {
        Common::Profiler::Scope scope{"Prepare.FindView"};
        return *image.FindView(view_info).image_view;
    }();
    const vk::Extent2D image_size = {image.HostExtent().width, image.HostExtent().height};
    expected_ratio = static_cast<float>(image_size.width) / static_cast<float>(image_size.height);

    const u32 capture_game_only_count = VideoCore::ConsumeGameOnlyScreenshotRequests();
    std::vector<ScreenshotReadback> pending_screenshots;
    if (capture_game_only_count > 0) {
        pending_screenshots.reserve(1);
        const bool hdr_encoded =
            attribute.attrib.pixel_format == Libraries::VideoOut::PixelFormat::A2R10G10B10Bt2020Pq;
        pending_screenshots.emplace_back(
            instance, draw_scheduler, ScreenshotKind::GameOnly,
            BuildScreenshotPaths(ScreenshotKind::GameOnly, capture_game_only_count),
            image_size.width, image_size.height,
            // The readback copies the image bytes, which are in guest (B,G,R,A) order.
            swap_red_blue ? vk::Format::eB8G8R8A8Srgb : view_info.format, hdr_encoded);
        auto& readback = pending_screenshots.back();

        // Capture the guest output before any host-side scaling (FSR/PP) is applied.
        image.Transit(vk::ImageLayout::eTransferSrcOptimal, vk::AccessFlagBits2::eTransferRead, {},
                      cmdbuf);
        CopyImageToReadback(cmdbuf, image.GetImage(), vk::ImageLayout::eTransferSrcOptimal,
                            readback);
    }

    // The ordinary VideoOut path stays single-image. SBS eye composition is
    // owned by PrepareVrFrame; it must not read back or blit this image.
    {
        image.Transit(vk::ImageLayout::eShaderReadOnlyOptimal, vk::AccessFlagBits2::eShaderRead, {},
                      cmdbuf);

        {
            Common::Profiler::Scope scope{"Prepare.FSR"};
            const auto zone = draw_scheduler.GpuProfile().Begin(cmdbuf, GpuProfiler::Stage::Fsr);
            image_view = fsr_pass.Render(draw_scheduler, image_view, image_size,
                                         {frame->width, frame->height}, fsr_settings, frame->is_hdr);
            draw_scheduler.GpuProfile().End(cmdbuf, zone);
        }

        // Vulkan has no sRGB variant of the 10-bit format, so an A2R10G10B10Srgb buffer reaches
        // the post process pass still sRGB encoded and has to be decoded there instead.
        pp_settings.srgb_input =
            attribute.attrib.pixel_format == Libraries::VideoOut::PixelFormat::A2R10G10B10Srgb;
        pp_settings.sbs = 0;
        // FDM changes fragment density only. It must not depend on the VR
        // sensor state or apply another texture-origin conversion to ordinary
        // VideoOut frames.
        pp_settings.flip_y = 0;
        {
            Common::Profiler::Scope scope{"Prepare.PostProcess"};
            const auto zone = draw_scheduler.GpuProfile().Begin(cmdbuf, GpuProfiler::Stage::PostProcess);
            // Do not attach FDM to the final presenter pass. It cannot reduce
            // guest rendering cost and would add a map upload plus a separate
            // render-pass variant on devices without dynamic FDM.
            pp_pass.Render(draw_scheduler, image_view, image_size, *frame, pp_settings, {});
            draw_scheduler.GpuProfile().End(cmdbuf, zone);
        }
    }

    DebugState.game_resolution = {image_size.width, image_size.height};
    DebugState.output_resolution = {frame->width, frame->height};

    std::shared_ptr<std::vector<ScreenshotReadback>> deferred_screenshots{};
    if (!pending_screenshots.empty()) {
        deferred_screenshots =
            std::make_shared<std::vector<ScreenshotReadback>>(std::move(pending_screenshots));
        draw_scheduler.DeferPriorityOperation(
            [deferred_screenshots]() { SavePendingScreenshots(*deferred_screenshots); });
    }

    // Flush frame creation commands.
    frame->ready_semaphore = draw_scheduler.GetMasterSemaphore()->Handle();
    frame->ready_tick = draw_scheduler.CurrentTick();
    // Opt-in correlation only. Capture numeric identities, never a Frame or
    // VideoOutPort pointer: a delayed diagnostic must not extend guest lifetime.
    if (Core::Diagnostics::Handoff::Enabled(instance.DiagnosticGeneration())) {
        const auto generation = instance.DiagnosticGeneration();
        const auto tick = frame->ready_tick;
        draw_scheduler.DeferPriorityOperation([generation, diagnostic_id, tick] {
            SHAD_HANDOFF(generation, "vo_snapshot_ready", 0, diagnostic_id, tick);
        });
    }
    SubmitInfo info{};
    {
        Common::Profiler::Scope scope{"Prepare.Flush"};
        draw_scheduler.GpuProfile().FrameEnd();
        draw_scheduler.Flush(info);
    }
    return frame;
}

Frame* Presenter::PrepareBlankFrame(bool present_thread) {
    // Request a free presentation frame.
    Frame* frame = GetRenderFrame();
    if (!frame)
        return nullptr;

    auto& scheduler = present_thread ? present_scheduler : draw_scheduler;
    scheduler.EndRendering(Vulkan::RenderBreak::Present);

    const auto cmdbuf = scheduler.RawCommandBuffer();

    constexpr vk::ImageSubresourceRange simple_subresource = {
        .aspectMask = vk::ImageAspectFlagBits::eColor,
        .levelCount = 1,
        .layerCount = 1,
    };
    const auto pre_barrier = vk::ImageMemoryBarrier2{
        .srcStageMask = vk::PipelineStageFlagBits2::eColorAttachmentOutput,
        .srcAccessMask = vk::AccessFlagBits2::eColorAttachmentRead,
        .dstStageMask = vk::PipelineStageFlagBits2::eColorAttachmentOutput,
        .dstAccessMask = vk::AccessFlagBits2::eColorAttachmentWrite,
        .oldLayout = vk::ImageLayout::eUndefined,
        .newLayout = vk::ImageLayout::eColorAttachmentOptimal,
        .image = frame->image,
        .subresourceRange = simple_subresource,
    };

    const auto post_barrier = vk::ImageMemoryBarrier2{
        .srcStageMask = vk::PipelineStageFlagBits2::eColorAttachmentOutput,
        .srcAccessMask = vk::AccessFlagBits2::eColorAttachmentWrite,
        .dstStageMask = vk::PipelineStageFlagBits2::eFragmentShader,
        .dstAccessMask = vk::AccessFlagBits2::eShaderRead,
        .oldLayout = vk::ImageLayout::eColorAttachmentOptimal,
        .newLayout = vk::ImageLayout::eGeneral,
        .image = frame->image,
        .subresourceRange = simple_subresource,
    };

    const vk::RenderingAttachmentInfo attachment = {
        .imageView = frame->image_view,
        .imageLayout = vk::ImageLayout::eColorAttachmentOptimal,
        .loadOp = vk::AttachmentLoadOp::eClear,
        .storeOp = vk::AttachmentStoreOp::eStore,
    };
    const vk::RenderingInfo rendering_info = {
        .renderArea =
            {
                .extent = {frame->width, frame->height},
            },
        .layerCount = 1,
        .colorAttachmentCount = 1u,
        .pColorAttachments = &attachment,
    };

    cmdbuf.pipelineBarrier2(vk::DependencyInfo{
        .imageMemoryBarrierCount = 1,
        .pImageMemoryBarriers = &pre_barrier,
    });

    cmdbuf.beginRendering(rendering_info);
    cmdbuf.endRendering();

    cmdbuf.pipelineBarrier2(vk::DependencyInfo{
        .imageMemoryBarrierCount = 1,
        .pImageMemoryBarriers = &post_barrier,
    });

    // Flush frame creation commands.
    frame->ready_semaphore = scheduler.GetMasterSemaphore()->Handle();
    frame->ready_tick = scheduler.CurrentTick();
    SubmitInfo info{};
    scheduler.Flush(info);
    return frame;
}

bool Presenter::Present(Frame* frame, bool is_reusing_frame) {
    Core::Diagnostics::Handoff::Scope present_scope{"Present.Frame", instance.DiagnosticGeneration()};
    if (!frame)
        return false;
    // A displayed frame stays owned by this thread while DrawLastFrame can
    // reuse its image/fence. Only its replacement releases it to the producer.
    // Failed acquire/present returns the new frame and preserves the last image.
    const auto free_frame = [&](bool presented = false) {
        if (!is_reusing_frame) {
            std::scoped_lock fl{free_mutex};
            if (presented) {
                if (last_submit_frame)
                    free_queue.push(last_submit_frame);
                last_submit_frame = frame;
            } else {
                free_queue.push(frame);
            }
            free_cv.notify_one();
        }
    };

    if (swapchain.StopRequested()) {
        free_frame();
        return false;
    }

    // SurfaceView may withdraw its ANativeWindow without ending the Guest
    // session. Keep the frame pool moving until a replacement is published.
    if (!swapchain.CanPresent()) {
        free_frame();
        return false;
    }
    if (swapchain.SurfaceChanged())
        swapchain.Recreate(window->GetWidth(), window->GetHeight());

    // Recreate the swapchain if the window was resized.
    if (window->GetWidth() != swapchain.GetWidth() ||
        window->GetHeight() != swapchain.GetHeight()) {
        swapchain.Recreate(window->GetWidth(), window->GetHeight());
    }

    auto acquired = [&] {
        Core::Diagnostics::Handoff::Scope scope{"Present.AcquireImage", instance.DiagnosticGeneration(), 0, 0, true};
        return swapchain.AcquireNextImage();
    }();
    if (acquired == AcquireStatus::Recreate && !swapchain.StopRequested() &&
        swapchain.CanPresent()) {
        swapchain.Recreate(window->GetWidth(), window->GetHeight());
        acquired = swapchain.AcquireNextImage();
    }
    if (acquired != AcquireStatus::Acquired) {
        // Timeout/cancel does not retire or rebuild a working swapchain. Leave
        // this frame's fence signalled so it can be safely reused next time.
        free_frame();
        return false;
    }

#if defined(SHADPS4_HAS_SCRCPY_CAPTURE_SDK)
    if (!is_reusing_frame) {
        embedded_capture->SyncRequest(*frame, swapchain.GetSurfaceFormat().format,
                                      static_cast<u32>(present_frames.size()));
    }
#endif

    // Reset fence for queue submission. Do it here instead of GetRenderFrame() because we may
    // skip frame because of slow swapchain recreation. If a frame skip occurs, we skip signal
    // the frame's present fence and future GetRenderFrame() call will hang waiting for this frame.
    const auto reset_result = instance.GetDevice().resetFences(frame->present_done);
    ASSERT_MSG(reset_result == vk::Result::eSuccess,
               "Unexpected error resetting present done fence: {}", vk::to_string(reset_result));

    status_layer->Prepare(Core::Diagnostics::DiagnosticNowNs(), swapchain.GetWidth(), swapchain.GetHeight());
    ImGuiID dockId = ImGui::Core::NewFrame(is_reusing_frame);

    const vk::Image swapchain_image = swapchain.Image();
    const vk::ImageView swapchain_image_view = swapchain.ImageView();

    auto& scheduler = present_scheduler;
    scheduler.GpuProfile().PresentKind(is_reusing_frame);
    const auto cmdbuf = scheduler.RawCommandBuffer();
    const u32 capture_with_overlays_count = VideoCore::ConsumeWithOverlaysScreenshotRequests();
    std::vector<ScreenshotReadback> pending_screenshots;
    if (capture_with_overlays_count > 0) {
        pending_screenshots.reserve(1);
    }

    if (EmulatorSettings.IsVkHostMarkersEnabled()) {
        cmdbuf.beginDebugUtilsLabelEXT(vk::DebugUtilsLabelEXT{
            .pLabelName = "Present",
        });
    }

    {
        auto* profiler_ctx = instance.GetProfilerContext();
        TracyVkNamedZoneC(profiler_ctx, renderer_gpu_zone, cmdbuf, "Host frame",
                          MarkersPalette::GpuMarkerColor, profiler_ctx != nullptr);

        const vk::Extent2D extent = swapchain.GetExtent();
        const std::array pre_barriers{
            vk::ImageMemoryBarrier{
                .srcAccessMask = vk::AccessFlagBits::eNone,
                .dstAccessMask = vk::AccessFlagBits::eColorAttachmentWrite,
                .oldLayout = vk::ImageLayout::eUndefined,
                .newLayout = vk::ImageLayout::eColorAttachmentOptimal,
                .srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
                .dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
                .image = swapchain_image,
                .subresourceRange{
                    .aspectMask = vk::ImageAspectFlagBits::eColor,
                    .baseMipLevel = 0,
                    .levelCount = 1,
                    .baseArrayLayer = 0,
                    .layerCount = VK_REMAINING_ARRAY_LAYERS,
                },
            },
            vk::ImageMemoryBarrier{
                .srcAccessMask = vk::AccessFlagBits::eColorAttachmentWrite,
                .dstAccessMask = vk::AccessFlagBits::eShaderRead,
                .oldLayout = vk::ImageLayout::eGeneral,
                .newLayout = vk::ImageLayout::eShaderReadOnlyOptimal,
                .srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
                .dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
                .image = frame->image,
                .subresourceRange{
                    .aspectMask = vk::ImageAspectFlagBits::eColor,
                    .baseMipLevel = 0,
                    .levelCount = 1,
                    .baseArrayLayer = 0,
                    .layerCount = VK_REMAINING_ARRAY_LAYERS,
                },
            },
        };

        bool swapchain_copied_for_screenshot = false;

        cmdbuf.pipelineBarrier(vk::PipelineStageFlagBits::eColorAttachmentOutput,
                               vk::PipelineStageFlagBits::eColorAttachmentOutput |
                                   vk::PipelineStageFlagBits::eFragmentShader,
                               vk::DependencyFlagBits::eByRegion, {}, {}, pre_barriers);

        { // Draw the game
            ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2{0.0f});
            ImGui::PushStyleVar(ImGuiStyleVar_WindowRounding, 0.0f);
            ImGui::PushStyleVar(ImGuiStyleVar_WindowBorderSize, 0.0f);
            ImGui::PushStyleColor(ImGuiCol_WindowBg, ImVec4(0.0f, 0.0f, 0.0f, 1.0f));
            ImGui::SetNextWindowDockID(dockId, ImGuiCond_Once);
            if (ImGui::Begin("Display##game_display", nullptr, ImGuiWindowFlags_NoNav)) {
                auto game_texture = frame->imgui_texture;
                auto game_width = frame->width;
                auto game_height = frame->height;

#if defined(__ANDROID__)
                // The first SBS bring-up must expose the actual VideoOut
                // frame.  Beat Saber keeps the generic shell splash flag set
                // while its Unity scene is already submitting frames, which
                // otherwise masks the SBS frame with an empty shell image.
                const bool show_shell_splash =
                    splash_visible() && !Core::HostRuntime::GuestVrSensor::Instance().Read().enabled;
#else
                const bool show_shell_splash = splash_visible();
#endif
                if (show_shell_splash) { // draw splash
                    if (!splash_img.has_value()) {
                        splash_img.emplace();
                        const auto& splash_data = Common::ElfInfo::Instance().GetSplashData();
                        if (!splash_data.empty()) {
                            splash_img = ImGui::RefCountedTexture::DecodePngTexture(splash_data);
                        }
                    }
                    if (auto& splash_image = this->splash_img.value()) {
                        auto [im_id, width, height] = splash_image.GetTexture();
                        game_texture = im_id;
                        game_width = width;
                        game_height = height;
                    }
                }

                ImVec2 contentArea = ImGui::GetContentRegionAvail();
                SetExpectedGameSize((s32)contentArea.x, (s32)contentArea.y);

                const auto imgRect =
                    FitImage(game_width, game_height, (s32)contentArea.x, (s32)contentArea.y);
                ImVec2 offset{
                    static_cast<float>(imgRect.offset.x),
                    static_cast<float>(imgRect.offset.y),
                };
                ImVec2 size{
                    static_cast<float>(imgRect.extent.width),
                    static_cast<float>(imgRect.extent.height),
                };

                ImGui::SetCursorPos(ImGui::GetCursorStartPos() + offset);
                ImGui::Image(game_texture, size);

                if (EmulatorSettings.IsNullGPU()) {
                    Core::Devtools::Layer::DrawNullGpuNotice();
                }
            }
            ImGui::End();
            ImGui::PopStyleVar(3);
            ImGui::PopStyleColor();
        }
        status_layer->Draw();
        ImGui::Core::Render(cmdbuf, swapchain_image_view, swapchain.GetExtent());

#if defined(SHADPS4_HAS_SCRCPY_CAPTURE_SDK)
        // Frame::image contains the complete application render target. Host
        // ImGui was drawn to swapchain_image and does not enter this copy.
        if (!is_reusing_frame && embedded_capture->Acquire(*frame)) {
            embedded_capture->Record(cmdbuf, *frame);
        }
#endif

        if (capture_with_overlays_count > 0) {
            pending_screenshots.emplace_back(
                instance, scheduler, ScreenshotKind::WithOverlays,
                BuildScreenshotPaths(ScreenshotKind::WithOverlays, capture_with_overlays_count),
                extent.width, extent.height,
                swapchain.GetHDR() ? vk::Format::eA2B10G10R10UnormPack32
                                   : swapchain.GetSurfaceFormat().format,
                swapchain.GetHDR());
            auto& readback = pending_screenshots.back();

            const vk::ImageMemoryBarrier to_transfer{
                .srcAccessMask = vk::AccessFlagBits::eColorAttachmentWrite,
                .dstAccessMask = vk::AccessFlagBits::eTransferRead,
                .oldLayout = vk::ImageLayout::eColorAttachmentOptimal,
                .newLayout = vk::ImageLayout::eTransferSrcOptimal,
                .srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
                .dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
                .image = swapchain_image,
                .subresourceRange{
                    .aspectMask = vk::ImageAspectFlagBits::eColor,
                    .baseMipLevel = 0,
                    .levelCount = 1,
                    .baseArrayLayer = 0,
                    .layerCount = VK_REMAINING_ARRAY_LAYERS,
                },
            };

            cmdbuf.pipelineBarrier(vk::PipelineStageFlagBits::eColorAttachmentOutput,
                                   vk::PipelineStageFlagBits::eTransfer,
                                   vk::DependencyFlagBits::eByRegion, {}, {}, to_transfer);
            CopyImageToReadback(cmdbuf, swapchain_image, vk::ImageLayout::eTransferSrcOptimal,
                                readback);
            swapchain_copied_for_screenshot = true;
        }

        const vk::AccessFlags post_src_access_mask =
            swapchain_copied_for_screenshot ? vk::AccessFlagBits::eTransferRead
                                            : vk::AccessFlagBits::eColorAttachmentWrite;
        const vk::ImageLayout post_old_layout = swapchain_copied_for_screenshot
                                                    ? vk::ImageLayout::eTransferSrcOptimal
                                                    : vk::ImageLayout::eColorAttachmentOptimal;
        const vk::ImageMemoryBarrier post_barrier{
            .srcAccessMask = post_src_access_mask,
            .dstAccessMask = vk::AccessFlagBits::eNone,
            .oldLayout = post_old_layout,
            .newLayout = vk::ImageLayout::ePresentSrcKHR,
            .srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
            .dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
            .image = swapchain_image,
            .subresourceRange{
                .aspectMask = vk::ImageAspectFlagBits::eColor,
                .baseMipLevel = 0,
                .levelCount = 1,
                .baseArrayLayer = 0,
                .layerCount = VK_REMAINING_ARRAY_LAYERS,
            },
        };
        cmdbuf.pipelineBarrier(vk::PipelineStageFlagBits::eAllCommands,
                               vk::PipelineStageFlagBits::eAllCommands,
                               vk::DependencyFlagBits::eByRegion, {}, {}, post_barrier);

        if (profiler_ctx) {
            TracyVkCollect(profiler_ctx, cmdbuf);
        }
    }
    if (EmulatorSettings.IsVkHostMarkersEnabled()) {
        cmdbuf.endDebugUtilsLabelEXT();
    }

    // Flush vulkan commands.
    std::shared_ptr<std::vector<ScreenshotReadback>> deferred_screenshots{};
    if (!pending_screenshots.empty()) {
        deferred_screenshots =
            std::make_shared<std::vector<ScreenshotReadback>>(std::move(pending_screenshots));
        scheduler.DeferPriorityOperation(
            [deferred_screenshots]() { SavePendingScreenshots(*deferred_screenshots); });
    }

    SubmitInfo info{};
    info.AddWait(swapchain.GetImageAcquiredSemaphore());
    // The frame is consumed by transfer/blit or fragment sampling before color
    // output. Waiting only at COLOR_ATTACHMENT_OUTPUT does not protect those reads.
    info.AddWait(frame->ready_semaphore, frame->ready_tick);
    info.AddSignal(swapchain.GetPresentReadySemaphore());
    info.AddSignal(frame->present_done);
#if defined(SHADPS4_HAS_SCRCPY_CAPTURE_SDK)
    embedded_capture->AddSubmitSync(info);
#endif
    scheduler.Flush(info);
    // FIFO acceptance is not enough for a binary present wait: its signal and
    // timeline dependencies must already have reached vkQueueSubmit. Wait here,
    // outside the queue lock, never on the PM4/guest owner.
    scheduler.WaitSubmitted();

    bool presented{};
    // Present to swapchain.
    bool reusable;
    {
        std::unique_lock submit_lock{instance.QueueMutex(), std::defer_lock};
        {
            Core::Diagnostics::Handoff::Scope scope{"Present.QueueLock", instance.DiagnosticGeneration(), 0, 0, true};
            submit_lock.lock();
        }
        Core::Diagnostics::Handoff::Scope scope{"Present.DriverCall", instance.DiagnosticGeneration()};
        const auto previous = swapchain.SuccessfulPresents();
        reusable = swapchain.Present();
#if defined(SHADPS4_HAS_SCRCPY_CAPTURE_SDK)
        embedded_capture->Present();
#endif
        presented = swapchain.SuccessfulPresents() > previous;
        if (presented)
            status_layer->Presented(Core::Diagnostics::DiagnosticNowNs(), is_reusing_frame);
    }
    // Recreate may wait for GPU resources. Do not prevent their producer from submitting.
    if (!reusable && !swapchain.StopRequested() && swapchain.CanPresent()) {
        swapchain.Recreate(window->GetWidth(), window->GetHeight());
    }

    free_frame(presented);
    if (!is_reusing_frame) {
        DebugState.IncFlipFrameNum();
    }
    return presented;
}

Frame* Presenter::GetRenderFrame() {
    // Wait for free presentation frames
    Frame* frame;
    {
        Core::Diagnostics::Handoff::Scope scope{"Present.FreeFrameWait", instance.DiagnosticGeneration(), 0, 0, true};
        std::unique_lock lock{free_mutex};
        free_cv.wait(lock, [this] { return swapchain.StopRequested() || !free_queue.empty(); });
        if (swapchain.StopRequested())
            return nullptr;
        LOG_DEBUG(Render_Vulkan, "Got render frame, remaining {}", free_queue.size() - 1);

        // Take the frame from the queue
        frame = free_queue.front();
        free_queue.pop();
        ASSERT_MSG(frame != last_submit_frame, "Presentation redraw frame acquired by producer");
    }

    const vk::Device device = instance.GetDevice();
    vk::Result result{};

    const auto wait = [&]() {
        Core::Diagnostics::Handoff::Scope scope{"Present.FrameFenceWait", instance.DiagnosticGeneration(), 0, 0, true};
        result = device.waitForFences(frame->present_done, false, 50'000'000);
        return result;
    };

    // Wait for the presentation to be finished so all frame resources are free
    while (wait() != vk::Result::eSuccess) {
        if (swapchain.StopRequested()) {
            std::scoped_lock lock(free_mutex);
            free_queue.push(frame);
            return nullptr;
        }
        ASSERT_MSG(result != vk::Result::eErrorDeviceLost,
                   "Device lost during waiting for a frame");
        // Retry if the waiting times out
        if (result == vk::Result::eTimeout) {
            continue;
        }
    }

    if (frame->width != expected_frame_width || frame->height != expected_frame_height ||
        frame->is_hdr != swapchain.GetHDR()) {
        RecreateFrame(frame, expected_frame_width, expected_frame_height);
    }

    return frame;
}

void Presenter::SetExpectedGameSize(s32 width, s32 height) {
    const float ratio = (float)width / (float)height;

    expected_frame_height = height;
    expected_frame_width = width;
    if (ratio > expected_ratio) {
        expected_frame_width = static_cast<s32>(height * expected_ratio);
    } else {
        expected_frame_height = static_cast<s32>(width / expected_ratio);
    }
}

} // namespace Vulkan
