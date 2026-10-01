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
#include "video_core/renderer_vulkan/vk_runtime.h"
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
    Common::Log::Setup("depth-copy-scratch-probe");
    Window window;
    Vulkan::Instance instance(window, 0, false, false, Vulkan::LoadAndroidTurnip(argv[1], argv[2]));
    if (instance.IsMaintenance8Supported())
        return 2;
    Vulkan::Scheduler scheduler(instance);
    Vulkan::Runtime runtime(instance, scheduler);
    VideoCore::BlitHelper blit(instance, scheduler);
    Common::SlotVector<VideoCore::ImageView> views;
    std::array<std::unique_ptr<VideoCore::Image>, 3> depth, color;
    for (u32 i = 0; i < 3; ++i) {
        VideoCore::ImageInfo info{};
        const u32 side = 64U << i;
        info.type = AmdGpu::ImageType::Color2DArray;
        info.pixel_format = vk::Format::eD32Sfloat;
        info.size = {side, side / 2, 1};
        info.resources = {3, 2};
        info.num_bits = 32;
        info.pitch = side;
        info.props.is_depth = true;
        info.UpdateSize();
        depth[i] = std::make_unique<VideoCore::Image>(instance, scheduler, blit, views, info);
        info.props.is_depth = false;
        info.pixel_format = vk::Format::eR32Sfloat;
        color[i] = std::make_unique<VideoCore::Image>(instance, scheduler, blit, views, info);
    }
    constexpr u32 N = 6, Slot = 512 * 1024;
    VideoCore::Buffer output(instance, 0, 2 * N * Slot, VideoCore::MemoryType::HostCached);
    VideoCore::Buffer upload(instance, 0, N * Slot, VideoCore::MemoryType::HostUncached);
    unsigned checks{}, failures{};
    auto check = [&](bool ok) {
        ++checks;
        if (!ok)
            ++failures;
    };
    for (u32 batch = 0; batch < 2; ++batch) {
        std::array<std::vector<std::pair<u64, u64>>, N> spans;
        for (u32 n = 0; n < N; ++n) {
            auto& d = *depth[n % 3];
            auto& c = *color[n % 3];
            const float value = float(n + 1 + batch * N) / 16.f;
            std::vector<vk::BufferImageCopy> uploads;
            u64 upload_end = n * Slot;
            for (u32 mip = 0; mip < 3; ++mip) {
                const auto extent = d.HostExtent(mip);
                const u64 bytes = u64(extent.width) * extent.height * 2 * 4;
                uploads.push_back({.bufferOffset = upload_end,
                                   .imageSubresource = {vk::ImageAspectFlagBits::eDepth, mip, 0, 2},
                                   .imageExtent = {extent.width, extent.height, 1}});
                auto* data = reinterpret_cast<float*>(upload.mapped_data.data() + upload_end);
                std::fill_n(data, bytes / 4, value);
                upload_end += bytes;
            }
            upload.Flush(n * Slot, upload_end - n * Slot);
            runtime.UploadImage(&d, &upload, uploads);
            runtime.CopyColorAndDepth(&d, &c);
            runtime.CopyColorAndDepth(&c, &d);
            for (u32 dir = 0; dir < 2; ++dir) {
                auto& src = dir ? d : c;
                std::vector<vk::BufferImageCopy> copies;
                u64 off = (2 * n + dir) * Slot;
                for (u32 mip = 0; mip < 3; ++mip) {
                    const auto extent = src.HostExtent(mip);
                    const u64 bytes = u64(extent.width) * extent.height * 2 * 4;
                    copies.push_back({.bufferOffset = off,
                                      .imageSubresource = {dir ? vk::ImageAspectFlagBits::eDepth
                                                               : vk::ImageAspectFlagBits::eColor,
                                                           mip, 0, 2},
                                      .imageExtent = {extent.width, extent.height, 1}});
                    spans[n].emplace_back(off, bytes);
                    off += bytes;
                }
                runtime.DownloadImage(&src, &output, copies);
            }
        }
        std::ostringstream ledger;
        VideoCore::VmaDiagnostics::Append(instance.GetAllocator(), instance.GetMemoryProperties(),
                                          false, ledger);
        std::smatch match;
        const auto text = ledger.str();
        u64 device_bytes{};
        const std::regex groups(
            R"(vma_group=(?:buffer/DeviceLocal|scratch/depth-color-copy)/[^ ]+ allocation_bytes=(\d+))");
        for (std::sregex_iterator i(text.begin(), text.end(), groups), end; i != end; ++i)
            device_bytes += std::stoull((*i)[1]);
        std::printf("batch=%u scratch_bytes=%llu\n", batch,
                    static_cast<unsigned long long>(device_bytes));
        check(device_bytes <= 2 * Slot);
        runtime.FlushBarriers();
        const vk::MemoryBarrier2 done{.srcStageMask = vk::PipelineStageFlagBits2::eAllCommands,
                                      .srcAccessMask = vk::AccessFlagBits2::eMemoryWrite,
                                      .dstStageMask = vk::PipelineStageFlagBits2::eHost,
                                      .dstAccessMask = vk::AccessFlagBits2::eHostRead};
        scheduler.CommandBuffer().pipelineBarrier2(
            {.memoryBarrierCount = 1, .pMemoryBarriers = &done});
        scheduler.Finish();
        scheduler.PopPendingOperations();
        output.Invalidate(0, output.SizeBytes());
        for (u32 n = 0; n < N; ++n)
            for (const auto [off, size] : spans[n]) {
                const u32 expected = std::bit_cast<u32>(float(n + 1 + batch * N) / 16.f);
                const auto* data = reinterpret_cast<const u32*>(output.mapped_data.data() + off);
                for (u32 j = 0; j < size / 4; ++j)
                    check(data[j] == expected);
            }
    }
    scheduler.SetSubmitCallback({});
    std::printf("depth copy scratch: %u checks, %u failures\n", checks, failures);
    return failures ? 1 : 0;
}
int main(int argc, char** argv) {
    const int result = Run(argc, argv);
    Common::Log::Shutdown();
    return result;
}
