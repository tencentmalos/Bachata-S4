// SPDX-FileCopyrightText: Copyright 2024 shadPS4 Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#include "common/profiler.h"
#include <algorithm>
#include <array>
#include <numeric>
#include <tuple>
#include "core/memory.h"
#include "shader_recompiler/info.h"
#include "video_core/amdgpu/liverpool.h"
#include "video_core/amdgpu/pm4_stats.h"
#include "video_core/renderer_vulkan/vk_rasterizer.h"
#include "video_core/renderer_vulkan/vk_runtime.h"
#include "video_core/renderer_vulkan/vk_shader_hle.h"
#include "video_core/amdgpu/pm4_trace.h"

extern std::unique_ptr<AmdGpu::Liverpool> liverpool;

namespace Vulkan {

static constexpr u64 COPY_SHADER_HASH = 0xfefebf9f;

static std::atomic<bool> guest_copy_enabled{true};

std::string HleGuestCopyCommand(const std::vector<std::string>& args) {
    if (args.size() == 1 && (args[0] == "on" || args[0] == "off")) {
        guest_copy_enabled.store(args[0] == "on", std::memory_order_relaxed);
    } else if (!args.empty() && !(args.size() == 1 && args[0] == "status")) {
        return "usage: hle_guest_copy status | on | off\n";
    }
    const auto& watch = Core::gpu_watch_counters;
    constexpr auto o = std::memory_order_relaxed;
    return fmt::format("hle_guest_copy={} mirror_regions={} mirror_bytes={} commit_regions={} "
                       "commit_bytes={}\n",
                       guest_copy_enabled.load(o) ? "on" : "off", watch.hle_mirror_regions.load(o),
                       watch.hle_mirror_bytes.load(o), watch.hle_commit_regions.load(o),
                       watch.hle_commit_bytes.load(o));
}

static bool ExecuteCopyShaderHLE(const Shader::Info& info, const AmdGpu::ComputeProgram& cs_program,
                                 Rasterizer& rasterizer) {
    Common::Profiler::Scope profile_scope{"HLE.CopyShader"};
    auto& scheduler = rasterizer.GetScheduler();
    auto& runtime = rasterizer.GetRuntime();
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
    // Destination regions to download for the guest-memory commit (see below), per batch.
    static std::vector<vk::BufferCopy> commits;
    commits.clear();

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
    if (AmdGpu::Pm4Trace::Active()) {
        u64 bytes = 0;
        for (const auto& c : copies)
            bytes += c.size;
        AmdGpu::Pm4Trace::NoteHost(AmdGpu::Pm4Trace::HostEvent::HleCopy,
                                   src_buf_sharp.base_address, dst_buf_sharp.base_address,
                                   copies.size(), bytes,
                                   hoisted                    ? "hoisted before pass"
                                   : scheduler.IsRendering() ? "breaks pass"
                                                              : "outside pass");
    }
    if (!hoisted) {
        if (Scheduler::PassLogActive() || AmdGpu::Pm4Trace::Active()) {
            u64 bytes = 0;
            for (const auto& c : copies)
                bytes += c.size;
            scheduler.NoteBreak(fmt::format("[hle copy src={:#x} dst={:#x} regions={} bytes={}]",
                                            src_buf_sharp.base_address, dst_buf_sharp.base_address,
                                            copies.size(), bytes));
        }
        scheduler.EndRendering(Vulkan::RenderBreak::Hle);
        scheduler.ClearBreakDetail();
    }
    if (Scheduler::LabelsEnabled()) {
        u64 bytes = 0;
        for (const auto& c : copies)
            bytes += c.size;
        scheduler.Label(fmt::format("shadps4.hle.copy src={:#x} dst={:#x} regions={} bytes={} {}",
                                    src_buf_sharp.base_address, dst_buf_sharp.base_address,
                                    copies.size(), bytes, hoisted ? "hoisted-before-pass" : "pass-break"));
    }

    // The copy lands in the GPU's buffers only. Guest memory keeps the old bytes, and the next
    // CPU write to any other byte of those pages makes the buffer cache re-upload the pages from
    // guest memory, putting the old bytes back over the copy: Bloodborne's skinning matrices are
    // copied here into pages its CPU also writes, and a lost matrix stretches a vertex across the
    // screen. So guest memory gets exactly the copied regions as well (never whole pages, which
    // may hold newer CPU data): at record time when every source is CPU-owned, otherwise from a
    // download of the destination regions once the GPU copy completes.
    const bool guest_copy = guest_copy_enabled.load(std::memory_order_relaxed);
    bool mirror = guest_copy;
    if (guest_copy) {
        for (const auto& c : copies) {
            if (buffer_cache.IsRegionGpuModified(src_buf_sharp.base_address + c.srcOffset,
                                                 c.size)) {
                mirror = false;
                break;
            }
        }
    }
    if (mirror) {
        Common::Profiler::Scope mirror_scope{"HLE.GuestMirror"};
        // One lock and cached backing segments for the whole list; the regions are many and small.
        auto* memory = Core::Memory::Instance();
        static std::vector<Core::MemoryManager::GuestCopy> guest_copies;
        static std::vector<size_t> skipped;
        guest_copies.clear();
        skipped.clear();
        for (const auto& c : copies) {
            guest_copies.push_back({src_buf_sharp.base_address + c.srcOffset,
                                    dst_buf_sharp.base_address + c.dstOffset, c.size});
        }
        u64 bytes = memory->CopyGuestRegions(guest_copies, skipped);
        // Regions spanning backing segments: the general path.
        static std::vector<u8> staging;
        for (const size_t index : skipped) {
            const auto& c = guest_copies[index];
            staging.resize(c.size);
            if (memory->TryCopySparseMemory(c.source, staging.data(), c.size) &&
                memory->TryWriteBacking(std::bit_cast<void*>(c.destination), staging.data(),
                                        c.size)) {
                bytes += c.size;
            }
        }
        auto& watch = Core::gpu_watch_counters;
        watch.hle_mirror_regions.fetch_add(guest_copies.size(), std::memory_order_relaxed);
        watch.hle_mirror_bytes.fetch_add(bytes, std::memory_order_relaxed);
    }
    const bool commit = guest_copy && !mirror;
    static std::vector<std::pair<VAddr, u32>> commit_targets;
    commit_targets.clear();
    u64 commit_bytes = 0;
    if (commit) {
        for (const auto& c : copies) {
            commit_targets.emplace_back(dst_buf_sharp.base_address + c.dstOffset,
                                        static_cast<u32>(c.size));
            commit_bytes += c.size;
        }
    }
    // Held until the deferred write-back below has read it.
    StagingBufferRef download{};
    if (commit && commit_bytes) {
        download = runtime.GetStagingPool().Request(commit_bytes, VideoCore::MemoryType::HostCached,
                                                    0, /*deferred=*/true);
    }
    u8* const download_data = download.mapped;
    u64 download_used = 0;

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
        runtime.CopyBuffer(src_buf, dst_buf, vk_copies);
        if (download_data) {
            // Read back what this batch wrote, in the order of commit_targets. Recorded with its
            // own transfer barrier so a hoisted copy stays ahead of the held pass.
            commits.clear();
            for (const auto& copy : vk_copies) {
                commits.push_back({copy.dstOffset, download.offset + download_used, copy.size});
                download_used += copy.size;
            }
            static constexpr vk::MemoryBarrier COPY_TO_DOWNLOAD{
                .srcAccessMask = vk::AccessFlagBits::eTransferWrite,
                .dstAccessMask = vk::AccessFlagBits::eTransferRead,
            };
            scheduler.CommandBuffer().pipelineBarrier(
                vk::PipelineStageFlagBits::eTransfer, vk::PipelineStageFlagBits::eTransfer,
                vk::DependencyFlagBits::eByRegion, COPY_TO_DOWNLOAD, {}, {});
            scheduler.CommandBuffer().copyBuffer(dst_buf->Handle(), download.buffer->Handle(),
                                                 commits);
            for (const auto& copy : commits) {
                runtime.AccessBuffer(dst_buf, copy.srcOffset, copy.size,
                                     vk::PipelineStageFlagBits2::eCopy,
                                     vk::AccessFlagBits2::eTransferRead);
            }
        }
        batch_start = batch_end;
    }

    if (download_data) {
        static constexpr vk::MemoryBarrier HOST_BARRIER{
            .srcAccessMask = vk::AccessFlagBits::eTransferWrite,
            .dstAccessMask = vk::AccessFlagBits::eHostRead,
        };
        scheduler.CommandBuffer().pipelineBarrier(vk::PipelineStageFlagBits::eTransfer,
                                                  vk::PipelineStageFlagBits::eHost, {},
                                                  HOST_BARRIER, {}, {});
        // Written in submission order once the copy has completed, like image downloads.
        scheduler.DeferPriorityOperation(
            [&pool = runtime.GetStagingPool(), download,
             targets = std::vector<std::pair<VAddr, u32>>(commit_targets)] {
                download.Invalidate();
                auto* memory = Core::Memory::Instance();
                auto& watch = Core::gpu_watch_counters;
                u64 at = 0;
                for (const auto& [address, bytes] : targets) {
                    if (memory->TryWriteBacking(std::bit_cast<void*>(address), download.mapped + at,
                                                bytes)) {
                        watch.hle_commit_regions.fetch_add(1, std::memory_order_relaxed);
                        watch.hle_commit_bytes.fetch_add(bytes, std::memory_order_relaxed);
                    }
                    at += bytes;
                }
                pool.FreeDeferred(download);
            });
    }
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
