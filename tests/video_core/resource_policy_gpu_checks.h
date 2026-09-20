// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once
#include <functional>
#include <sstream>
#include <thread>
#include "video_core/buffer_cache/buffer.h"
#include "video_core/texture_cache/tile_manager.h"
#include "video_core/vma_diagnostics.h"

// Production Image/TileManager/VMA paths. GPU completion waits are test-only.
static void ResourcePolicyGpuChecks(const Vulkan::Instance& instance, Vulkan::Scheduler& scheduler,
                                    VideoCore::BlitHelper& blit,
                                    Common::SlotVector<VideoCore::ImageView>& views,
                                    const std::function<void(bool, const char*)>& check) {
    using namespace VideoCore;
    const auto allocator = instance.GetAllocator();
    const auto snapshot = [&] {
        std::ostringstream out;
        VmaDiagnostics::Append(allocator, instance.GetMemoryProperties(),
                               instance.CanReportMemoryUsage(), out);
        return out.str();
    };
    const auto value = [](const std::string& text, const char* key) -> int64_t {
        const auto offset = text.find(std::string(key) + '=');
        return offset == text.npos ? -1 : std::stoll(text.substr(offset + std::strlen(key) + 1));
    };
    // Separate real VMA allocators exercise block growth and whole-block return
    // under all three policies without changing the renderer's active allocator.
    std::array<u64, 3> policy_peak{};
    std::array<u64, 3> policy_idle{};
    for (const auto quality : {TextureQuality::High, TextureQuality::Medium, TextureQuality::Low}) {
        const auto policy = HostMemoryPolicy::For({4, quality, false});
        const VmaVulkanFunctions functions{
            .vkGetInstanceProcAddr = VULKAN_HPP_DEFAULT_DISPATCHER.vkGetInstanceProcAddr,
            .vkGetDeviceProcAddr = VULKAN_HPP_DEFAULT_DISPATCHER.vkGetDeviceProcAddr};
        VmaAllocatorCreateInfo ci{};
        ci.physicalDevice = instance.GetPhysicalDevice();
        ci.device = instance.GetDevice();
        ci.instance = instance.GetInstance();
        ci.pVulkanFunctions = &functions;
        ci.vulkanApiVersion = VK_API_VERSION_1_3;
        VmaAllocator isolated{};
        const auto result = VmaDiagnostics::CreateAllocator(ci, policy, &isolated);
        check(result == VK_SUCCESS, "memory policy allocator creation");
        if (result != VK_SUCCESS)
            continue;
        std::vector<std::pair<VkBuffer, VmaAllocation>> buffers;
        for (u32 i = 0; i < 80; ++i) {
            VkBufferCreateInfo buffer_ci{VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO};
            buffer_ci.size = 1ull << 20;
            buffer_ci.usage = VK_BUFFER_USAGE_TRANSFER_SRC_BIT;
            VmaAllocationCreateInfo ai{};
            ai.usage = VMA_MEMORY_USAGE_AUTO_PREFER_DEVICE;
            VkBuffer buffer{};
            VmaAllocation allocation{};
            const auto allocated = VmaDiagnostics::CreateBuffer(
                isolated, &buffer_ci, &ai, &buffer, &allocation, nullptr, "probe/policy");
            check(allocated == VK_SUCCESS, "memory policy allocation burst");
            if (allocated == VK_SUCCESS)
                buffers.emplace_back(buffer, allocation);
        }
        for (const auto& [buffer, allocation] : buffers)
            VmaDiagnostics::DestroyBuffer(isolated, buffer, allocation);
        std::ostringstream out;
        VmaDiagnostics::Append(isolated, instance.GetMemoryProperties(), false, out);
        const auto idle = out.str();
        policy_peak[u32(quality)] = value(idle, "vma_reserved_peak_bytes");
        policy_idle[u32(quality)] = value(idle, "vma_reserved_bytes");
        check(value(idle, "vma_live_allocation_bytes") == 0,
              "memory policy frees burst allocations");
        check(value(idle, "vma_reserved_bytes") <= policy.preferred_block_bytes,
              "idle retained block respects policy granularity");
        check(value(idle, "device_memory_freed_bytes") > 0, "VMA returns whole blocks to driver");
        check(value(idle, "vma_reconciliation_delta") == 0 &&
                  value(idle, "vma_reserved_reconciliation_delta") == 0,
              "memory policy balances VMA accounting");
        std::printf("MEMORY_POLICY %s peak=%llu idle_reserved=%lld returned=%lld live=%lld\n",
                    TextureQualityName(quality).data(),
                    (unsigned long long)policy_peak[u32(quality)],
                    (long long)value(idle, "vma_reserved_bytes"),
                    (long long)value(idle, "device_memory_freed_bytes"),
                    (long long)value(idle, "vma_live_allocation_bytes"));
        vmaDestroyAllocator(isolated);
        VmaDiagnostics::End(isolated);
    }
    // VMA's block growth can raise a particular burst peak (observed 96 ->
    // 120 MiB here). The policy promises less idle retention, not a peak cap.
    check(policy_idle[2] < policy_idle[0], "low block policy reduces idle retention after burst");
    scheduler.Finish();
    const auto baseline = snapshot();
    check(value(baseline, "vma_consistent") == 1, "VMA quiescent snapshot");
    check(value(baseline, "vma_reconciliation_delta") == 0, "VMA allocation accounting complete");
    check(value(baseline, "vma_reserved_reconciliation_delta") == 0,
          "VMA device block callbacks complete");
    // Repeated cross-thread allocate/free exercises immediate allocation-handle reuse.
    std::array<std::thread, 4> workers;
    for (auto& worker : workers)
        worker = std::thread([&] {
            for (u32 i = 0; i < 32; ++i) {
                VkBufferCreateInfo ci{VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO};
                ci.size = 1024 + i * 256;
                ci.usage = VK_BUFFER_USAGE_TRANSFER_SRC_BIT;
                VmaAllocationCreateInfo ai{};
                ai.usage = VMA_MEMORY_USAGE_AUTO;
                VkBuffer buffer{};
                VmaAllocation allocation{};
                if (VmaDiagnostics::CreateBuffer(allocator, &ci, &ai, &buffer, &allocation, nullptr,
                                                 "probe/concurrent") == VK_SUCCESS)
                    VmaDiagnostics::DestroyBuffer(allocator, buffer, allocation);
            }
        });
    for (auto& worker : workers)
        worker.join();
    const auto after_workers = snapshot();
    check(value(after_workers, "tracked_live_bytes") == value(baseline, "tracked_live_bytes"),
          "VMA concurrent free balance");
    check(value(after_workers, "tracked_created_count") -
                  value(baseline, "tracked_created_count") ==
              128,
          "VMA counts allocations");
    check(value(after_workers, "tracked_freed_count") - value(baseline, "tracked_freed_count") ==
              128,
          "VMA counts actual frees");
    check(value(after_workers, "vma_reconciliation_delta") == 0, "VMA concurrent reconciliation");
    check(value(after_workers, "vma_reserved_reconciliation_delta") == 0,
          "VMA concurrent blocks reconciliation");

    // Retirement is a subset of live allocation, not a free. Reclassification
    // and repeated tags must not count the same allocation twice.
    {
        Buffer buffer{instance, scheduler, MemoryUsage::Upload, 0, AllFlags, 65536};
        VmaAllocationInfo allocation{};
        vmaGetAllocationInfo(allocator, buffer.buffer.allocation, &allocation);
        const auto before = snapshot();
        VmaDiagnostics::Tag(allocator, buffer.buffer.allocation, "probe/retiring", true);
        VmaDiagnostics::Tag(allocator, buffer.buffer.allocation, "probe/retiring", true);
        const auto retired = snapshot();
        check(value(retired, "tracked_live_bytes") == value(before, "tracked_live_bytes"),
              "retiring remains live");
        check(value(retired, "tracked_retiring_bytes") - value(before, "tracked_retiring_bytes") ==
                  allocation.size,
              "retirement counted once");
        check(value(retired, "tracked_retiring_count") - value(before, "tracked_retiring_count") ==
                  1,
              "retirement count");
        VmaDiagnostics::Tag(allocator, buffer.buffer.allocation, "probe/reclassified", true);
        const auto reclassified = snapshot();
        check(value(reclassified, "tracked_retiring_bytes") ==
                  value(retired, "tracked_retiring_bytes"),
              "retirement survives reclassification");
        check(value(reclassified, "tracked_retiring_peak_bytes") ==
                  value(retired, "tracked_retiring_peak_bytes"),
              "reclassification does not inflate retirement peak");
    }
    const auto freed_retired = snapshot();
    check(value(freed_retired, "tracked_retiring_bytes") ==
              value(after_workers, "tracked_retiring_bytes"),
          "destroy removes retirement bytes");
    check(value(freed_retired, "tracked_live_bytes") == value(after_workers, "tracked_live_bytes"),
          "destroy removes allocation bytes");

    const auto info_for = [](u32 width, u32 height, u32 levels) {
        ImageInfo info{};
        info.type = AmdGpu::ImageType::Color2DArray;
        info.size = {width, height, 1};
        info.pitch = width;
        info.resources = {levels, 2};
        info.pixel_format = vk::Format::eR8G8B8A8Unorm;
        info.num_bits = 32;
        info.UpdateSize();
        return info;
    };
    const auto copies_for = [](const ImageInfo& info, u32 first) {
        std::vector<vk::BufferImageCopy> result;
        for (u32 mip = first; mip < info.resources.levels; ++mip) {
            const auto& part = info.mips_layout[mip];
            result.push_back({.bufferOffset = part.offset - info.mips_layout[first].offset,
                              .bufferRowLength = part.pitch,
                              .bufferImageHeight = part.height,
                              .imageSubresource = {vk::ImageAspectFlagBits::eColor, mip, 0,
                                                   info.resources.layers},
                              .imageExtent = {std::max(info.size.width >> mip, 1u),
                                              std::max(info.size.height >> mip, 1u), 1}});
        }
        return result;
    };
    const auto fill = [](Buffer& buffer, const ImageInfo& info, u32 first) {
        std::fill(buffer.mapped_data.begin(), buffer.mapped_data.end(), 0);
        for (u32 mip = first; mip < info.resources.levels; ++mip) {
            const auto& part = info.mips_layout[mip];
            for (u32 layer = 0; layer < info.resources.layers; ++layer)
                for (u32 y = 0; y < std::max(info.size.height >> mip, 1u); ++y)
                    for (u32 x = 0; x < std::max(info.size.width >> mip, 1u); ++x) {
                        auto* pixel = buffer.mapped_data.data() + part.offset -
                                      info.mips_layout[first].offset +
                                      ((layer * part.height + y) * part.pitch + x) * 4;
                        pixel[0] = 32 + mip * 24;
                        pixel[1] = 64 + layer * 64;
                        pixel[2] = 128;
                        pixel[3] = 255;
                    }
        }
    };
    const auto read_physical = [&](Image& image) {
        const auto& ci = image.backing->image.image_ci;
        std::vector<vk::BufferImageCopy> regions;
        u32 bytes = 0;
        for (u32 mip = 0; mip < ci.mipLevels; ++mip) {
            const auto extent = vk::Extent3D{std::max(ci.extent.width >> mip, 1u),
                                             std::max(ci.extent.height >> mip, 1u), 1};
            regions.push_back(
                {.bufferOffset = bytes,
                 .imageSubresource = {vk::ImageAspectFlagBits::eColor, mip, 0, ci.arrayLayers},
                 .imageExtent = extent});
            bytes += extent.width * extent.height * ci.arrayLayers * 4;
        }
        Buffer output(instance, scheduler, MemoryUsage::Download, 0,
                      vk::BufferUsageFlagBits::eTransferDst, bytes);
        image.Transit(vk::ImageLayout::eTransferSrcOptimal, vk::AccessFlagBits2::eTransferRead, {});
        scheduler.CommandBuffer().copyImageToBuffer(
            image.GetImage(), vk::ImageLayout::eTransferSrcOptimal, output.Handle(), regions);
        const vk::MemoryBarrier2 host_barrier{.srcStageMask = vk::PipelineStageFlagBits2::eTransfer,
                                              .srcAccessMask = vk::AccessFlagBits2::eTransferWrite,
                                              .dstStageMask = vk::PipelineStageFlagBits2::eHost,
                                              .dstAccessMask = vk::AccessFlagBits2::eHostRead};
        scheduler.CommandBuffer().pipelineBarrier2(
            vk::DependencyInfo{.memoryBarrierCount = 1, .pMemoryBarriers = &host_barrier});
        scheduler.Finish();
        vmaInvalidateAllocation(allocator, output.buffer.allocation, 0, VK_WHOLE_SIZE);
        for (u32 mip = 0; mip < ci.mipLevels; ++mip) {
            const auto& region = regions[mip];
            const auto area = region.imageExtent.width * region.imageExtent.height;
            for (u32 layer = 0; layer < ci.arrayLayers; ++layer)
                for (u32 i = 0; i < area; ++i) {
                    const auto* p =
                        output.mapped_data.data() + region.bufferOffset + (layer * area + i) * 4;
                    check(p[0] == 32 + (mip + image.DroppedMips()) * 24 &&
                              p[1] == 64 + layer * 64 && p[2] == 128 && p[3] == 255,
                          "policy GPU pixels/mips/layers");
                }
        }
    };
    const auto usage =
        vk::BufferUsageFlagBits::eTransferSrc | vk::BufferUsageFlagBits::eTransferDst;
    for (u32 render : {2u, 3u, 4u, 6u, 8u}) {
        for (auto quality : {TextureQuality::High, TextureQuality::Medium, TextureQuality::Low}) {
            const ScalePolicySnapshot policy{render, quality, false};
            check(policy.ShaderMapping() == (render != 8 || quality != TextureQuality::High),
                  "independent shader policy");
            for (const auto dims : {Extent2D{8, 8},
                                    {16, 16},
                                    {32, 32},
                                    {64, 64},
                                    {65, 49},
                                    {1024, 32},
                                    {128, 128},
                                    {256, 256},
                                    {512, 512}}) {
                const auto info = info_for(dims.width, dims.height, 3);
                Image image(instance, scheduler, blit, views, info, nullptr, ScaleUse::Texture,
                            policy);
                const u32 expected =
                    std::min(dims.width, dims.height) < 128 || quality == TextureQuality::High ? 8
                    : quality == TextureQuality::Medium                                        ? 6
                                                                                               : 4;
                check(image.ScaleEighths() == expected,
                      "small/long texture protection independent of render");
                check(image.DroppedMips() == (expected == 4 ? 1u : 0u), "low uses actual mip tail");
                check(image.ShaderScaleCode(1) != 1, "nonzero view base does not repeat drop");
                const auto retained = info.RetainedMipChain(image.DroppedMips());
                Buffer upload(instance, scheduler, MemoryUsage::Upload, 0, usage,
                              retained.guest_size);
                fill(upload, info, image.DroppedMips());
                auto regions = copies_for(info, image.DroppedMips());
                image.Upload(regions, upload.Handle(), 0, retained.guest_size);
                read_physical(image);
                check(image.ScalePlan().upload_bytes == retained.guest_size,
                      "upload bytes exclude dropped mip0");
            }
            for (const auto role : {ScaleUse::Texture, ScaleUse::Storage}) {
                const auto info = info_for(2048, 128, 1);
                Image image(instance, scheduler, blit, views, info, nullptr, role, policy);
                check(!image.IsScaled(), "single mip or storage stays native");
            }
        }
    }
    // The real detiler receives only the retained mip bytes, for both micro and
    // macro tiling (including a macro-to-micro tail). Compare every retained pixel.
    {
        StreamBuffer stream(instance, scheduler, MemoryUsage::Stream, 1024 * 1024);
        TileManager tiles(instance, scheduler, stream);
        for (auto mode : {AmdGpu::TileMode::Thin1DThin, AmdGpu::TileMode::Thin2DThin}) {
            auto info = info_for(512, 256, 5);
            info.props.is_tiled = true;
            info.tile_mode = mode;
            info.array_mode = mode == AmdGpu::TileMode::Thin1DThin
                                  ? AmdGpu::ArrayMode::Array1DTiledThin1
                                  : AmdGpu::ArrayMode::Array2DTiledThin1;
            info.UpdateSize();
            Image native(instance, scheduler, blit, views, info, nullptr, ScaleUse::Texture,
                         ScalePolicySnapshot{});
            Buffer source(instance, scheduler, MemoryUsage::Upload, 0, usage, info.guest_size);
            Buffer tiled(instance, scheduler, MemoryUsage::Download, 0,
                         usage | vk::BufferUsageFlagBits::eStorageBuffer, info.guest_size);
            fill(source, info, 0);
            auto full = copies_for(info, 0);
            native.Upload(full, source.Handle(), 0, info.guest_size);
            tiles.TileImage(native, full, tiled.Handle(), 0, info.guest_size);
            scheduler.Finish();
            vmaInvalidateAllocation(allocator, tiled.buffer.allocation, 0, VK_WHOLE_SIZE);
            const auto tail = info.RetainedMipChain(1);
            Buffer tail_source(instance, scheduler, MemoryUsage::Upload, 0,
                               usage | vk::BufferUsageFlagBits::eStorageBuffer, tail.guest_size);
            std::memcpy(tail_source.mapped_data.data(),
                        tiled.mapped_data.data() + info.mips_layout[1].offset, tail.guest_size);
            const auto [linear, offset] = tiles.DetileImage(tail_source.Handle(), 0, tail);
            Image low(instance, scheduler, blit, views, info, nullptr, ScaleUse::Texture,
                      ScalePolicySnapshot{8, TextureQuality::Low});
            auto retained = copies_for(info, 1);
            low.Upload(retained, linear, offset, tail.guest_size);
            read_physical(low);
            VmaAllocationInfo high_bytes{}, low_bytes{};
            vmaGetAllocationInfo(allocator, native.backing->image.allocation, &high_bytes);
            vmaGetAllocationInfo(allocator, low.backing->image.allocation, &low_bytes);
            check(low_bytes.size < high_bytes.size, "VMA confirms mip0 allocation absent");
            printf("POLICY_TAIL tile=%u guest=%u retained=%u native_allocation=%llu "
                   "low_allocation=%llu\n",
                   u32(mode), info.guest_size, tail.guest_size, (unsigned long long)high_bytes.size,
                   (unsigned long long)low_bytes.size);
        }
    }
    // Latest GPU contents survive first-attachment conversion and copy inheritance.
    {
        auto info = info_for(256, 256, 3);
        const ScalePolicySnapshot policy{4, TextureQuality::High};
        Image source(instance, scheduler, blit, views, info, nullptr, ScaleUse::Texture, policy);
        Buffer upload(instance, scheduler, MemoryUsage::Upload, 0, usage, info.guest_size);
        fill(upload, info, 0);
        auto regions = copies_for(info, 0);
        source.Upload(regions, upload.Handle(), 0, info.guest_size);
        source.ObserveUsage(ScaleUse::RenderTarget);
        check(source.ScaleEighths() == 4 && source.ScalePlan().render_transition,
              "first attachment replans high asset");
        check(!source.binding.needs_rebind, "backing replan preserves cache identity binding");
        read_physical(source);
        Image dest(instance, scheduler, blit, views, info, nullptr, ScaleUse::Unknown, policy);
        dest.CopyImage(source);
        dest.MarkSampled();
        check(dest.ScaleEighths() == 4 && source.ScaleEighths() == 4, "copy inherits render plan");
        read_physical(dest);
        dest.CopyImage(source); // A later frame must not replan or promote the sampled copy.
        check(dest.ScaleEighths() == 4 && source.ScaleEighths() == 4,
              "repeated copy keeps matching plan");
        dest.ForceNative("readback");
        check(!dest.IsScaled() && dest.ScalePlan().upscaled_readback,
              "readback marks lossy native promotion");
        check(!dest.binding.needs_rebind, "native promotion preserves cache identity binding");
        read_physical(dest);
        dest.ObserveUsage(ScaleUse::RenderTarget);
        check(!dest.IsScaled(), "native readback decision sticky");
    }
    scheduler.Finish();
    const auto final = snapshot();
    check(value(final, "vma_reconciliation_delta") == 0, "final VMA allocations reconcile");
    check(value(final, "vma_reserved_reconciliation_delta") == 0, "final VMA blocks reconcile");
    check(value(final, "tracked_peak_bytes") >= value(final, "tracked_live_bytes"),
          "allocation peak tracks lifetime");
    printf("VMA_POLICY_FINAL\n%s", final.c_str());
}
