// SPDX-FileCopyrightText: Copyright 2024 shadPS4 Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#include <algorithm>
#include <array>
#include <numeric>
#include "shader_recompiler/info.h"
#include "video_core/amdgpu/liverpool.h"
#include "video_core/amdgpu/pm4_stats.h"
#include "video_core/renderer_vulkan/vk_rasterizer.h"
#include "video_core/renderer_vulkan/vk_scheduler.h"
#include "video_core/renderer_vulkan/vk_shader_hle.h"

extern std::unique_ptr<AmdGpu::Liverpool> liverpool;

namespace Vulkan {

static constexpr u64 COPY_SHADER_HASH = 0xfefebf9f;

static bool ExecuteCopyShaderHLE(const Shader::Info& info, const AmdGpu::ComputeProgram& cs_program,
                                 Rasterizer& rasterizer) {
    auto& scheduler = rasterizer.GetScheduler();
    auto& buffer_cache = rasterizer.GetBufferCache();

    // Copy shader defines three formatted buffers as inputs: control, source, and destination.
    const auto ctl_buf_sharp = info.buffers[0].GetSharp(info);
    const auto src_buf_sharp = info.buffers[1].GetSharp(info);
    const auto dst_buf_sharp = info.buffers[2].GetSharp(info);
    const auto buf_stride = src_buf_sharp.GetStride();
    ASSERT(buf_stride == dst_buf_sharp.GetStride());

    struct CopyShaderControl {
        u32 dst_idx;
        u32 src_idx;
        u32 end;
    };
    static_assert(sizeof(CopyShaderControl) == 12);
    ASSERT(ctl_buf_sharp.GetStride() == sizeof(CopyShaderControl));
    const auto ctl_buf = reinterpret_cast<const CopyShaderControl*>(ctl_buf_sharp.base_address);

    static std::vector<vk::BufferCopy> copies;
    copies.clear();
    copies.reserve(cs_program.dim_x);

    // Turnip sets up a blit per region; the guest lists mostly 64-256 byte pieces that
    // continue each other in both buffers, so extend the previous region instead.
    const bool merge = !AmdGpu::Pm4Stats::hle_merge_off.load(std::memory_order_relaxed);
    for (u32 i = 0; i < cs_program.dim_x; i++) {
        const auto& [dst_idx, src_idx, end] = ctl_buf[i];
        const u32 local_dst_offset = dst_idx * buf_stride;
        const u32 local_src_offset = src_idx * buf_stride;
        const u32 local_size = (end + 1) * buf_stride;
        if (merge && !copies.empty()) {
            auto& last = copies.back();
            if (last.srcOffset + last.size == local_src_offset &&
                last.dstOffset + last.size == local_dst_offset) {
                last.size += local_size;
                continue;
            }
        }
        copies.emplace_back(local_src_offset, local_dst_offset, local_size);
    }

    if (AmdGpu::Pm4Stats::armed.load(std::memory_order_relaxed)) [[unlikely]] {
        static std::vector<AmdGpu::Pm4Stats::HleRegion> regions;
        regions.clear();
        for (const auto& c : copies)
            regions.push_back({c.srcOffset, c.dstOffset, c.size});
        AmdGpu::Pm4Stats::NoteHleCopy(liverpool->diagnostic_guest_flip, src_buf_sharp.base_address,
                                      dst_buf_sharp.base_address, regions);
    }
    if (scheduler.TakePassBreakLog())
        LOG_INFO(Render_Vulkan, "Internal scale: hle copy src={:#x} dst={:#x} stride={} copies={} bytes={}",
                 src_buf_sharp.base_address, dst_buf_sharp.base_address, buf_stride, copies.size(),
                 std::accumulate(copies.begin(), copies.end(), u64{0},
                                 [](u64 acc, const vk::BufferCopy& c) { return acc + c.size; }));
    // Place the copy before the open pass instead of breaking it when nothing in the pass
    // so far depends on it (frame-graph style hoisting on the deferred command stream).
    bool hoisted = false;
    if (scheduler.IsRendering() && !copies.empty()) {
        // Checked per region: bounding boxes would cover unrelated data between them.
        static std::vector<Scheduler::AccessRange> reads, writes;
        reads.clear();
        writes.clear();
        bool src_upload = false;
        for (const auto& c : copies) {
            const VAddr src = src_buf_sharp.base_address + c.srcOffset;
            const VAddr dst = dst_buf_sharp.base_address + c.dstOffset;
            reads.emplace_back(src, src + c.size);
            writes.emplace_back(dst, dst + c.size);
            // A CPU-dirty source is uploaded first, which rewrites its GPU copy.
            src_upload = src_upload || buffer_cache.IsRegionCpuModified(src, c.size);
        }
        hoisted = scheduler.BeginHoist(reads, writes, src_upload);
    }
    if (!hoisted)
        scheduler.EndRendering(Vulkan::RenderBreak::Hle);

    static constexpr vk::MemoryBarrier READ_BARRIER{
        .srcAccessMask = vk::AccessFlagBits::eMemoryWrite,
        .dstAccessMask = vk::AccessFlagBits::eTransferRead | vk::AccessFlagBits::eTransferWrite,
    };
    static constexpr vk::MemoryBarrier WRITE_BARRIER{
        .srcAccessMask = vk::AccessFlagBits::eTransferWrite,
        .dstAccessMask = vk::AccessFlagBits::eMemoryRead | vk::AccessFlagBits::eMemoryWrite,
    };
    scheduler.CommandBuffer().pipelineBarrier(
        vk::PipelineStageFlagBits::eAllCommands, vk::PipelineStageFlagBits::eTransfer,
        vk::DependencyFlagBits::eByRegion, READ_BARRIER, {}, {});

    static constexpr vk::DeviceSize MaxDistanceForMerge = 64_MB;
    u32 batch_start = 0;
    u32 batch_end = 0;

    while (batch_end < copies.size()) {
        // Place first copy into the current batch
        const auto& copy = copies[batch_start];
        auto src_offset_min = copy.srcOffset;
        auto src_offset_max = copy.srcOffset + copy.size;
        auto dst_offset_min = copy.dstOffset;
        auto dst_offset_max = copy.dstOffset + copy.size;

        for (++batch_end; batch_end < copies.size(); batch_end++) {
            // Compute new src and dst bounds if we were to batch this copy
            const auto& [src_offset, dst_offset, size] = copies[batch_end];
            auto new_src_offset_min = std::min(src_offset_min, src_offset);
            auto new_src_offset_max = std::max(src_offset_max, src_offset + size);
            if (new_src_offset_max - new_src_offset_min > MaxDistanceForMerge) {
                break;
            }

            auto new_dst_offset_min = std::min(dst_offset_min, dst_offset);
            auto new_dst_offset_max = std::max(dst_offset_max, dst_offset + size);
            if (new_dst_offset_max - new_dst_offset_min > MaxDistanceForMerge) {
                break;
            }

            // We can batch this copy
            src_offset_min = new_src_offset_min;
            src_offset_max = new_src_offset_max;
            dst_offset_min = new_dst_offset_min;
            dst_offset_max = new_dst_offset_max;
        }

        // Obtain buffers for the total source and destination ranges.
        const auto [src_buf, src_buf_offset] = buffer_cache.ObtainBuffer(
            src_buf_sharp.base_address + src_offset_min, src_offset_max - src_offset_min, false);
        const auto [dst_buf, dst_buf_offset] = buffer_cache.ObtainBuffer(
            dst_buf_sharp.base_address + dst_offset_min, dst_offset_max - dst_offset_min, true);

        // Apply found buffer base.
        const auto vk_copies = std::span{copies}.subspan(batch_start, batch_end - batch_start);
        for (auto& copy : vk_copies) {
            copy.srcOffset = copy.srcOffset - src_offset_min + src_buf_offset;
            copy.dstOffset = copy.dstOffset - dst_offset_min + dst_buf_offset;
        }

        // Execute buffer copies.
        LOG_TRACE(Render_Vulkan, "HLE buffer copy: src_size = {}, dst_size = {}",
                  src_offset_max - src_offset_min, dst_offset_max - dst_offset_min);
        scheduler.CommandBuffer().copyBuffer(src_buf->Handle(), dst_buf->Handle(), vk_copies);
        batch_start = batch_end;
    }

    scheduler.CommandBuffer().pipelineBarrier(
        vk::PipelineStageFlagBits::eTransfer, vk::PipelineStageFlagBits::eAllCommands,
        vk::DependencyFlagBits::eByRegion, WRITE_BARRIER, {}, {});
    if (hoisted)
        scheduler.EndHoist();

    return true;
}

bool ExecuteShaderHLE(const Shader::Info& info, const AmdGpu::Regs& regs,
                      const AmdGpu::ComputeProgram& cs_program, Rasterizer& rasterizer) {
    switch (info.pgm_hash) {
    case COPY_SHADER_HASH:
        return ExecuteCopyShaderHLE(info, cs_program, rasterizer);
    default:
        return false;
    }
}

} // namespace Vulkan
