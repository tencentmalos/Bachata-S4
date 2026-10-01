// SPDX-License-Identifier: GPL-2.0-or-later
// Consecutive production image readbacks must preserve every dispatch's pixels
// while keeping linear scratch bounded by the largest image, not the batch size.
#include <cstdio>
#include <regex>
#include <sstream>
#include "common/logging/log.h"
#include "common/path_util.h"
#include "frontend/window.h"
#include "video_core/renderer_vulkan/vk_instance.h"
#include "video_core/renderer_vulkan/vk_scheduler.h"
#include "video_core/renderer_vulkan/vk_staging_buffer_pool.h"
#include "video_core/texture_cache/blit_helper.h"
#include "video_core/texture_cache/image.h"
#include "video_core/texture_cache/tile_manager.h"
#include "video_core/vma_diagnostics.h"

struct Window : Frontend::Window {
    s32 GetWidth() const override {
        return 64;
    }
    s32 GetHeight() const override {
        return 64;
    }
    Frontend::WindowSystemInfo GetWindowInfo() const override {
        return {};
    }
    bool RequestKeyboard() override {
        return false;
    }
    void ReleaseKeyboard() override {}
};
int Run(int argc, char** argv) {
    if (argc != 3)
        return 2;
    Common::FS::InitializeAndroidUserPaths(argv[2]);
    Common::Log::Setup("tiling-scratch-probe");
    Window window;
    Vulkan::Instance instance(window, 0, false, false, Vulkan::LoadAndroidTurnip(argv[1], argv[2]));
    Vulkan::Scheduler scheduler(instance);
    VideoCore::StreamBuffer stream(instance, scheduler, VideoCore::MemoryType::Stream, 1 << 20);
    Vulkan::StagingBufferPool pool(instance, scheduler);
    VideoCore::BlitHelper blit(instance, scheduler);
    Common::SlotVector<VideoCore::ImageView> views;
    VideoCore::TileManager tiler(instance, scheduler, stream, pool);
    std::array<std::unique_ptr<VideoCore::Image>, 3> images;
    for (u32 i = 0; i < images.size(); ++i) {
        VideoCore::ImageInfo info{};
        const u32 side = 64U << i;
        info.type = AmdGpu::ImageType::Color2D;
        info.pixel_format = vk::Format::eR8G8B8A8Unorm;
        info.size = {side, side, 1};
        info.resources = {1, 1};
        info.num_bits = 32;
        info.pitch = side;
        info.props.is_tiled = true;
        info.tile_mode = AmdGpu::TileMode::Thin1DThin;
        info.array_mode = AmdGpu::ArrayMode::Array1DTiledThin1;
        info.UpdateSize();
        images[i] = std::make_unique<VideoCore::Image>(instance, scheduler, blit, views, info);
    }
    constexpr u32 N = 48, Slot = 256 * 256 * 4;
    VideoCore::Buffer output(instance, 0, N * Slot, VideoCore::MemoryType::HostCached);
    unsigned checks{}, failures{};
    const auto check = [&](bool ok) {
        ++checks;
        if (!ok)
            ++failures;
    };
    for (u32 batch = 0; batch < 2; ++batch) {
        std::array<u32, N> expected{}, sizes{};
        for (u32 n = 0; n < N; ++n) {
            auto& image = *images[n < 3 ? n : (n % 5 == 0 ? 0 : 2)];
            const auto& info = image.info;
            const u32 r = (n * 37 + batch * 53) & 255;
            const u32 g = (n * 71 + batch * 11) & 255;
            const u32 b = (n * 19 + batch * 97) & 255;
            expected[n] = r | (g << 8) | (b << 16) | 0xff000000U;
            sizes[n] = info.guest_size;
            image.Transit(vk::ImageLayout::eTransferDstOptimal, vk::AccessFlagBits2::eTransferWrite,
                          {});
            scheduler.CommandBuffer().clearColorImage(
                image.GetImage(), vk::ImageLayout::eTransferDstOptimal,
                vk::ClearColorValue(std::array<float, 4>{r / 255.f, g / 255.f, b / 255.f, 1.f}),
                vk::ImageSubresourceRange{vk::ImageAspectFlagBits::eColor, 0, 1, 0, 1});
            vk::BufferImageCopy copy{.bufferRowLength = info.pitch,
                                     .bufferImageHeight = info.size.height,
                                     .imageSubresource = {vk::ImageAspectFlagBits::eColor, 0, 0, 1},
                                     .imageExtent = {info.size.width, info.size.height, 1}};
            tiler.TileImage(image, {&copy, 1}, output.Handle(), u64(n) * Slot, info.guest_size);
        }
        // Before submit: old implementation retains all 48 full-image allocations.
        std::ostringstream ledger;
        VideoCore::VmaDiagnostics::Append(instance.GetAllocator(), instance.GetMemoryProperties(),
                                          false, ledger);
        const auto text = ledger.str();
        std::smatch match;
        const bool found = std::regex_search(
            text, match,
            std::regex(
                R"(vma_group=scratch/detile-tile/[^ ]+ allocation_bytes=(\d+) buffer_requested_bytes=\d+ count=(\d+))"));
        check(found);
        if (found) {
            printf("batch=%u scratch_bytes=%s count=%s\n", batch, match[1].str().c_str(),
                   match[2].str().c_str());
            check(std::stoull(match[1]) <= 2 * Slot);
            check(std::stoul(match[2]) <= 3);
        }
        const vk::MemoryBarrier2 barrier{.srcStageMask = vk::PipelineStageFlagBits2::eComputeShader,
                                         .srcAccessMask = vk::AccessFlagBits2::eShaderWrite,
                                         .dstStageMask = vk::PipelineStageFlagBits2::eHost,
                                         .dstAccessMask = vk::AccessFlagBits2::eHostRead};
        scheduler.CommandBuffer().pipelineBarrier2(
            {.memoryBarrierCount = 1, .pMemoryBarriers = &barrier});
        scheduler.Finish();
        output.Invalidate(0, N * Slot);
        for (u32 n = 0; n < N; ++n) {
            const auto* words = reinterpret_cast<const u32*>(output.mapped_data.data() + n * Slot);
            for (u32 i = 0; i < sizes[n] / 4; ++i)
                check(words[i] == expected[n]);
        }
    }
    printf("tiling_scratch_probe: %u checks / %u failures\n", checks, failures);
    return failures ? 1 : 0;
}
int main(int argc, char** argv) {
    const auto result = Run(argc, argv);
    Common::Log::Shutdown();
    return result;
}
