// SPDX-License-Identifier: GPL-2.0-or-later
// Batched codec copies must match individually retired encodes without retaining
// one full block buffer per texture or mip until the submission ends.
#include <bit>
#include <cstdio>
#include <cstring>
#include <regex>
#include <sstream>
#include "common/logging/log.h"
#include "common/path_util.h"
#include "frontend/window.h"
#include "video_core/buffer_cache/buffer.h"
#include "video_core/renderer_vulkan/vk_instance.h"
#include "video_core/renderer_vulkan/vk_scheduler.h"
#include "video_core/texture_cache/blit_helper.h"
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
    Common::Log::Setup("block-scratch-probe");
    Window window;
    Vulkan::Instance instance(window, 0, false, false, Vulkan::LoadAndroidTurnip(argv[1], argv[2]));
    Vulkan::Scheduler scheduler(instance);
    VideoCore::BlitHelper blit(instance, scheduler);
    auto d = instance.GetDevice();
    struct Image {
        vk::UniqueDeviceMemory memory;
        vk::UniqueImage image;
    };
    auto make = [&](vk::Format format, u32 side, u32 layers, u32 mips) {
        Image image;
        image.image = Vulkan::Check(d.createImageUnique(
            {.imageType = vk::ImageType::e2D,
             .format = format,
             .extent = {side, side, 1},
             .mipLevels = mips,
             .arrayLayers = layers,
             .samples = vk::SampleCountFlagBits::e1,
             .tiling = vk::ImageTiling::eOptimal,
             .usage = vk::ImageUsageFlagBits::eSampled | vk::ImageUsageFlagBits::eTransferSrc |
                      vk::ImageUsageFlagBits::eTransferDst}));
        const auto req = d.getImageMemoryRequirements(*image.image);
        u32 type = 0;
        while (!(req.memoryTypeBits & (1U << type)))
            ++type;
        image.memory = Vulkan::Check(
            d.allocateMemoryUnique({.allocationSize = req.size, .memoryTypeIndex = type}));
        Vulkan::Check(d.bindImageMemory(*image.image, *image.memory, 0));
        return image;
    };
    auto transit = [&](vk::Image image, vk::ImageLayout from, vk::ImageLayout to, u32 layers,
                       u32 mips) {
        const vk::ImageMemoryBarrier2 b{
            .srcStageMask = vk::PipelineStageFlagBits2::eAllCommands,
            .srcAccessMask = vk::AccessFlagBits2::eMemoryRead | vk::AccessFlagBits2::eMemoryWrite,
            .dstStageMask = vk::PipelineStageFlagBits2::eAllCommands,
            .dstAccessMask = vk::AccessFlagBits2::eMemoryRead | vk::AccessFlagBits2::eMemoryWrite,
            .oldLayout = from,
            .newLayout = to,
            .srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
            .dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
            .image = image,
            .subresourceRange = {vk::ImageAspectFlagBits::eColor, 0, mips, 0, layers}};
        scheduler.CommandBuffer().pipelineBarrier2(
            vk::DependencyInfo{.imageMemoryBarrierCount = 1, .pImageMemoryBarriers = &b});
    };
    constexpr u32 N = 48, Slot = 131072;
    struct Case {
        Image src, dst;
        u32 side, layers, mip, block, bytes;
        bool srgb;
        VideoCore::BlockCodec codec;
    };
    std::vector<Case> cases;
    for (u32 n = 0; n < N; ++n) {
        const u32 side = 48U << (n % 3), layers = 1 + (n % 2), mip = (n / 3) % 2,
                  block = n % 3 == 2 ? 6 : 4;
        const bool srgb = n % 4 == 3, bc = n % 3 == 0;
        auto fmt = bc ? (srgb ? vk::Format::eBc7SrgbBlock : vk::Format::eBc7UnormBlock)
                   : block == 6
                       ? (srgb ? vk::Format::eAstc6x6SrgbBlock : vk::Format::eAstc6x6UnormBlock)
                       : (srgb ? vk::Format::eAstc4x4SrgbBlock : vk::Format::eAstc4x4UnormBlock);
        Case c{make(vk::Format::eR8G8B8A8Unorm, side << mip, layers, mip + 1),
               make(fmt, side, layers, 1),
               side,
               layers,
               mip,
               block,
               (side / block) * (side / block) * layers * 16,
               srgb,
               bc ? VideoCore::BlockCodec::Bc7 : VideoCore::BlockCodec::Astc};
        transit(*c.src.image, vk::ImageLayout::eUndefined, vk::ImageLayout::eTransferDstOptimal,
                layers, mip + 1);
        const std::array<float, 4> rgba{float((n * 43) & 255) / 255, float((n * 17) & 255) / 255,
                                        float((n * 71) & 255) / 255,
                                        float(64 + (n * 19) % 192) / 255};
        scheduler.CommandBuffer().clearColorImage(
            *c.src.image, vk::ImageLayout::eTransferDstOptimal, vk::ClearColorValue(rgba),
            vk::ImageSubresourceRange{vk::ImageAspectFlagBits::eColor, 0, mip + 1, 0, layers});
        transit(*c.src.image, vk::ImageLayout::eTransferDstOptimal,
                vk::ImageLayout::eShaderReadOnlyOptimal, layers, mip + 1);
        transit(*c.dst.image, vk::ImageLayout::eUndefined, vk::ImageLayout::eTransferDstOptimal,
                layers, 1);
        cases.push_back(std::move(c));
    }
    VideoCore::Buffer output(instance, 0, N * Slot, VideoCore::MemoryType::HostCached);
    auto snapshot = [&] {
        std::ostringstream out;
        VideoCore::VmaDiagnostics::Append(instance.GetAllocator(), instance.GetMemoryProperties(),
                                          false, out);
        return out.str();
    };
    auto bytes = [](const std::string& s) {
        std::smatch m;
        if (!std::regex_search(s, m, std::regex("tracked_live_bytes=([0-9]+)")))
            throw std::runtime_error("ledger");
        return std::stoull(m[1].str());
    };
    const u64 before = bytes(snapshot());
    auto encode = [&](u32 n) {
        auto& c = cases[n];
        blit.EncodeBlocks(c.codec, *c.src.image, vk::Format::eR8G8B8A8Unorm, c.mip, *c.dst.image, 0,
                          c.side, c.side, c.layers, c.srgb, c.block);
        transit(*c.dst.image, vk::ImageLayout::eTransferDstOptimal,
                vk::ImageLayout::eTransferSrcOptimal, c.layers, 1);
        const vk::BufferImageCopy copy{
            .bufferOffset = u64(n) * Slot,
            .imageSubresource = {vk::ImageAspectFlagBits::eColor, 0, 0, c.layers},
            .imageExtent = {c.side, c.side, 1}};
        scheduler.CommandBuffer().copyImageToBuffer(
            *c.dst.image, vk::ImageLayout::eTransferSrcOptimal, output.Handle(), copy);
        transit(*c.dst.image, vk::ImageLayout::eTransferSrcOptimal,
                vk::ImageLayout::eTransferDstOptimal, c.layers, 1);
    };
    auto finish = [&] {
        const vk::MemoryBarrier2 b{.srcStageMask = vk::PipelineStageFlagBits2::eTransfer,
                                   .srcAccessMask = vk::AccessFlagBits2::eTransferWrite,
                                   .dstStageMask = vk::PipelineStageFlagBits2::eHost,
                                   .dstAccessMask = vk::AccessFlagBits2::eHostRead};
        scheduler.CommandBuffer().pipelineBarrier2(
            vk::DependencyInfo{.memoryBarrierCount = 1, .pMemoryBarriers = &b});
        scheduler.Finish();
        scheduler.PopPendingOperations();
        output.Invalidate(0, output.SizeBytes());
    };
    unsigned checks{}, fails{};
    auto check = [&](bool ok) {
        ++checks;
        if (!ok)
            ++fails;
    };
    u64 largest{};
    for (u32 n = 0; n < N; ++n) {
        encode(n);
        largest = std::max<u64>(largest, cases[n].bytes);
    }
    const u64 live = bytes(snapshot()) - before;
    check(live <= 2 * std::bit_ceil(std::max<u64>(largest, 16384)));
    std::printf("pending codec bytes=%llu bound=%llu\n", (unsigned long long)live,
                (unsigned long long)(2 * std::bit_ceil(std::max<u64>(largest, 16384))));
    finish();
    std::vector<u8> batched(output.mapped_data.begin(), output.mapped_data.end());
    // Independently retired reference records cannot accidentally read a later
    // encode's data, even if scratch reuse is missing its execution dependency.
    for (u32 n = 0; n < N; ++n) {
        encode(n);
        finish();
        for (u32 b = 0; b < cases[n].bytes; ++b)
            check(batched[n * Slot + b] == output.mapped_data[n * Slot + b]);
    }
    std::printf("block scratch: %u checks, %u failures\n", checks, fails);
    return fails ? 1 : 0;
}
int main(int argc, char** argv) {
    std::setvbuf(stdout, nullptr, _IONBF, 0);
    int r = Run(argc, argv);
    Common::Log::Shutdown();
    return r;
}
