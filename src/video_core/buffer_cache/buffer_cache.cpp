// SPDX-FileCopyrightText: Copyright 2024 shadPS4 Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#include <algorithm>
#include <cstring>
#include <limits>
#include <ostream>
#include <stdexcept>
#include <magic_enum/magic_enum.hpp>
#include "common/alignment.h"
#include "common/profiler.h"
#include "core/memory.h"
#include "video_core/amdgpu/liverpool.h"
#include "video_core/amdgpu/pm4_stats.h"
#include "video_core/buffer_cache/buffer.h"
#include "video_core/buffer_cache/buffer_cache.h"
#include "video_core/buffer_cache/memory_tracker.h"
#include "video_core/renderer_vulkan/vk_graphics_pipeline.h"
#include "video_core/renderer_vulkan/vk_instance.h"
#include "video_core/renderer_vulkan/vk_runtime.h"
#include "video_core/renderer_vulkan/vk_scheduler.h"
#include "video_core/texture_cache/texture_cache.h"

#include <vk_mem_alloc.h>
#include "video_core/vma_diagnostics.h"

namespace VideoCore {

static constexpr size_t GDS_BUFFER_SIZE = 64_KB;
static constexpr size_t STREAM_BUFFER_SIZE = 128_MB;

static constexpr auto ARENA_USAGE =
    vk::BufferUsageFlagBits::eTransferSrc | vk::BufferUsageFlagBits::eTransferDst |
    vk::BufferUsageFlagBits::eUniformBuffer | vk::BufferUsageFlagBits::eStorageBuffer |
    vk::BufferUsageFlagBits::eIndexBuffer | vk::BufferUsageFlagBits::eVertexBuffer |
    vk::BufferUsageFlagBits::eIndirectBuffer | vk::BufferUsageFlagBits::eShaderDeviceAddress;

std::optional<u32> FindMemoryType(const vk::PhysicalDeviceMemoryProperties& properties,
                                  vk::MemoryPropertyFlags wanted, u32 memory_type_bits) {
    for (u32 i = 0; i < properties.memoryTypeCount; ++i) {
        if (((memory_type_bits >> i) & 1) == 0) {
            continue;
        }
        const auto flags = properties.memoryTypes[i].propertyFlags;
        if ((flags & wanted) == wanted) {
            return i;
        }
    }
    return std::nullopt;
}

BufferCache::BufferCache(const Vulkan::Instance& instance_, Vulkan::Scheduler& scheduler_,
                         Vulkan::Runtime& runtime_, AmdGpu::Liverpool* liverpool_,
                         TextureCache& texture_cache_, PageManager& tracker)
    : instance{instance_}, scheduler{scheduler_}, runtime{runtime_},
      staging_pool{runtime_.GetStagingPool()}, liverpool{liverpool_},
      memory{Core::Memory::Instance()}, texture_cache{texture_cache_},
      memory_tracker{std::make_unique<MemoryTracker>(tracker)},
      stream_buffer{instance, scheduler, MemoryType::Stream, STREAM_BUFFER_SIZE},
      gds_buffer{instance, 0, GDS_BUFFER_SIZE, MemoryType::Stream, "GDS Buffer"},
      memory_semaphore{instance} {
    const vk::BufferCreateInfo probe_ci = {
        .flags =
            vk::BufferCreateFlagBits::eSparseBinding | vk::BufferCreateFlagBits::eSparseResidency,
        .size = ARENA_PAGE_SIZE,
        .usage = ARENA_USAGE,
        .sharingMode = vk::SharingMode::eExclusive,
    };
    const vk::DeviceBufferMemoryRequirements req_info = {
        .pCreateInfo = &probe_ci,
    };
    const auto device = instance.GetDevice();
    const auto reqs = device.getBufferMemoryRequirements(req_info).memoryRequirements;
    block_size = Common::AlignUp(std::max<u64>(reqs.alignment, MIN_BLOCK_SIZE), reqs.alignment);
    ASSERT_MSG(std::popcount(block_size) == 1, "Sparse block size {} is not a power of 2",
               block_size);
    block_shift = std::bit_width(block_size) - 1;
    blocks_per_arena_page = ARENA_PAGE_SIZE / block_size;
    blocks_per_arena_page_shift = ARENA_PAGE_BITS - block_shift;
    arena_memory_type_index =
        FindMemoryType(instance.GetMemoryProperties(), vk::MemoryPropertyFlagBits::eDeviceLocal,
                       reqs.memoryTypeBits)
            .value();

    const u64 bda_pagetable_size =
        (blocks_per_arena_page * NUM_ARENA_PAGES) * sizeof(vk::DeviceAddress);
    fault_manager = std::make_unique<FaultManager>(instance, scheduler, *this, block_shift,
                                                   blocks_per_arena_page * NUM_ARENA_PAGES);
    bda_pagetable_buffer = std::make_unique<Buffer>(
        instance, 0, bda_pagetable_size, MemoryType::DeviceLocal, "BDA Page Table Buffer");
    runtime.FillBuffer(bda_pagetable_buffer.get(), 0u, bda_pagetable_size, 0u);
    for (const auto& [buffer, category] :
         std::initializer_list<std::pair<const Buffer*, const char*>>{
             {&stream_buffer, "buffer/stream-pool"},
             {&gds_buffer, "buffer/gds"},
             {bda_pagetable_buffer.get(), "buffer/bda-page-table"}}) {
        VmaDiagnostics::Tag(instance.GetAllocator(), buffer->buffer.allocation, category);
    }
    VerifySparseResidency();
}

BufferCache::~BufferCache() = default;

void BufferCache::VerifySparseResidency() {
    // Some drivers report sparse residency and accept binds without making the bound memory
    // reachable (the R8 Turnip build on KGSL): every arena access would then read zeros and the
    // game renders nothing. One block is checked through the GPU so such a driver fails here.
    constexpr u64 PatternSize = 256;
    const auto device = instance.GetDevice();
    const auto memory = Vulkan::Check<"allocate sparse check memory">(
        device.allocateMemoryUnique(vk::MemoryAllocateInfo{
            .allocationSize = block_size,
            .memoryTypeIndex = arena_memory_type_index,
        }));
    const Buffer probe{instance, 0, block_size, MemoryType::Sparse, "Sparse residency check"};
    const vk::SparseMemoryBind bind = {
        .resourceOffset = 0,
        .size = block_size,
        .memory = *memory,
    };
    const vk::SparseBufferMemoryBindInfo buffer_bind = {
        .buffer = probe.Handle(),
        .bindCount = 1,
        .pBinds = &bind,
    };
    const auto fence = Vulkan::Check<"create sparse check fence">(device.createFenceUnique({}));
    {
        std::scoped_lock lock{instance.QueueMutex()};
        Vulkan::Check<"bind the sparse check block">(instance.GetGraphicsQueue().bindSparse(
            vk::BindSparseInfo{.bufferBindCount = 1, .pBufferBinds = &buffer_bind}, *fence));
    }
    Vulkan::Check<"wait for the sparse check bind">(
        device.waitForFences(*fence, vk::True, std::numeric_limits<u64>::max()));

    // Both ends of the block, written from a pattern and read back.
    const auto upload = staging_pool.Request(PatternSize, MemoryType::HostUncached);
    const auto download = staging_pool.Request(PatternSize * 2, MemoryType::HostCached);
    for (u64 i = 0; i < PatternSize; ++i) {
        upload.mapped[i] = static_cast<u8>(i * 7 + 3);
    }
    upload.Flush();
    std::memset(download.mapped, 0, PatternSize * 2);
    download.Flush();
    const std::array<vk::BufferCopy, 2> writes = {{
        {upload.offset, 0, PatternSize},
        {upload.offset, block_size - PatternSize, PatternSize},
    }};
    const std::array<vk::BufferCopy, 2> reads = {{
        {0, download.offset, PatternSize},
        {block_size - PatternSize, download.offset + PatternSize, PatternSize},
    }};
    runtime.CopyBuffer(upload.buffer, &probe, writes);
    runtime.CopyBuffer(&probe, download.buffer, reads);
    runtime.FlushBarriers();
    static constexpr vk::MemoryBarrier HostRead = {
        .srcAccessMask = vk::AccessFlagBits::eTransferWrite,
        .dstAccessMask = vk::AccessFlagBits::eHostRead,
    };
    scheduler.CommandBuffer().pipelineBarrier(vk::PipelineStageFlagBits::eTransfer,
                                              vk::PipelineStageFlagBits::eHost, {}, HostRead, {},
                                              {});
    scheduler.Finish();
    download.Invalidate();

    u64 mismatches = 0;
    for (u64 i = 0; i < PatternSize * 2; ++i) {
        mismatches += download.mapped[i] != upload.mapped[i % PatternSize];
    }
    if (mismatches != 0) {
        const auto message = fmt::format(
            "The Vulkan driver ({}) accepts sparse buffer binds but {} of {} bytes written through "
            "a bound {} KiB block did not read back: sparse residency does not work, and the "
            "buffer cache depends on it. On Android select the mainline Turnip driver.",
            instance.GetModelName(), mismatches, PatternSize * 2, block_size >> 10);
        LOG_CRITICAL(Render_Vulkan, "{}", message);
        throw std::runtime_error(message);
    }
    LOG_INFO(Render_Vulkan, "Sparse residency check passed ({} KiB blocks)", block_size >> 10);
}

void BufferCache::AppendMemoryDiagnostics(std::ostream& out) {
    const auto allocation_size = [&](const Buffer& buffer) -> u64 {
        if (!buffer.buffer.allocation) {
            return 0;
        }
        VmaAllocationInfo info{};
        vmaGetAllocationInfo(instance.GetAllocator(), buffer.buffer.allocation, &info);
        return info.size;
    };
    u64 arena_va_bytes{}, resident_bytes{}, resident_ranges_count{};
    for (const auto& arena : arenas) {
        arena_va_bytes += arena.size_bytes;
    }
    for (const auto& range : resident_ranges) {
        resident_bytes += (range.end - range.start) << block_shift;
        ++resident_ranges_count;
    }
    // Arena residency is allocated outside VMA, so it is reported here rather than in the VMA
    // categories.
    out << "guest_arena_count=" << arenas.size() << " guest_arena_va_bytes=" << arena_va_bytes
        << " guest_arena_resident_bytes=" << resident_bytes
        << " guest_arena_resident_ranges=" << resident_ranges_count
        << " sparse_block_bytes=" << block_size << "\n";
    out << "utility_buffer_allocation_bytes="
        << allocation_size(stream_buffer) + allocation_size(gds_buffer) +
               allocation_size(*bda_pagetable_buffer)
        << "\n";
    out << "stream_requested_bytes=" << stream_buffer.SizeBytes()
        << " bda_page_table_requested_bytes=" << bda_pagetable_buffer->SizeBytes() << "\n";
    const auto& watch = Core::gpu_watch_counters;
    constexpr auto o = std::memory_order_relaxed;
    out << "gpu_watch watch_calls=" << watch.watch_calls.load(o)
        << " watch_pages=" << watch.watch_pages.load(o)
        << " release_calls=" << watch.release_calls.load(o)
        << " release_pages=" << watch.release_pages.load(o)
        << " syscalls=" << watch.syscalls.load(o)
        << " predicted_pages=" << watch.predicted_pages.load(o) << "\n";
    out << "hle_guest_copy mirror_regions=" << watch.hle_mirror_regions.load(o)
        << " mirror_bytes=" << watch.hle_mirror_bytes.load(o)
        << " commit_regions=" << watch.hle_commit_regions.load(o)
        << " commit_bytes=" << watch.hle_commit_bytes.load(o) << "\n";
    const u64 overwrites = watch.gpu_data_overwrites.load(o);
    out << "gpu_data_overwrites=" << overwrites
        << " (CPU writes to GPU-written pages with readbacks off; first:";
    for (u64 i = 0; i < std::min<u64>(overwrites, watch.gpu_data_overwrite_addrs.size()); ++i) {
        out << fmt::format(" {:#x}", watch.gpu_data_overwrite_addrs[i].load(o));
    }
    out << ")\n";
}

void BufferCache::InvalidateMemory(VAddr device_addr, u64 size) {
    // A faulting CPU must not read renderer-owned structures. Published tracker regions are
    // stable; absent/unwatched pages are no-ops.
    memory_tracker->InvalidateRegion(
        device_addr, size, [this, device_addr, size] { ReadMemory(device_addr, size, true); });
}

void BufferCache::InvalidateMemoryFromWriteFault(VAddr device_addr, u64 size) {
    const size_t predicted = memory_tracker->InvalidateRegionFromWriteFault(
        device_addr, size, [this, device_addr, size] { ReadMemory(device_addr, size, true); });
    if (predicted) {
        Core::gpu_watch_counters.predicted_pages.fetch_add(predicted, std::memory_order_relaxed);
    }
}

void BufferCache::InvalidateMapping(VAddr device_addr, u64 size) {
    memory_tracker->InvalidateMapping(device_addr, size);
}

void BufferCache::ReadMemory(VAddr device_addr, u64 size, bool is_write) {
    liverpool->SendCommand<true>([this, device_addr, size, is_write] {
        const u32 first_block = device_addr >> block_shift;
        const u32 last_block = (device_addr + size - 1) >> block_shift;
        const auto* arena = GetArena(first_block, last_block);

        // GPU-modified ranges come as many small scattered islands,
        // so the download is widened to a window around the request
        constexpr u64 WindowSize = 512_KB;
        const VAddr arena_end = arena->cpu_addr + arena->size_bytes;
        const VAddr window_start =
            std::max<VAddr>(Common::AlignDown(device_addr, WindowSize), arena->cpu_addr);
        const VAddr window_end = std::min<VAddr>(
            std::max<VAddr>(window_start + WindowSize, device_addr + size), arena_end);
        DownloadMemory(arena, window_start, window_end - window_start);
        if (is_write) {
            memory_tracker->MarkRegionAsCpuModified(device_addr, size);
        }
    });
}

void BufferCache::DownloadMemory(const Buffer* arena, VAddr device_addr, u64 size) {
    boost::container::small_vector<vk::BufferCopy, 1> copies;
    u64 total_size_bytes = 0;
    const VAddr arena_base = arena->cpu_addr;
    memory_tracker->ForEachDownloadRange<false>(device_addr, size, [&](u64 address, u64 size) {
        const auto add_download = [&](VAddr start, VAddr end) {
            const u64 new_offset = start - arena_base;
            const u64 new_size = end - start;
            copies.push_back(vk::BufferCopy{
                .srcOffset = new_offset,
                .dstOffset = total_size_bytes,
                .size = new_size,
            });
            // Align up to avoid cache conflicts
            constexpr u64 align = 64ULL;
            constexpr u64 mask = ~(align - 1ULL);
            total_size_bytes += (new_size + align - 1) & mask;
        };
        gpu_modified_ranges.ForEachInRange(address, size, add_download);
        gpu_modified_ranges.Subtract(address, size);
    });
    if (total_size_bytes == 0) {
        return;
    }
    const auto download = staging_pool.Request(total_size_bytes, VideoCore::MemoryType::HostCached);
    for (auto& copy : copies) {
        copy.dstOffset += download.offset;
    }
    runtime.CopyBuffer(arena, download.buffer, copies);
    scheduler.Finish();

    download.buffer->Invalidate(download.offset, download.size);
    for (const auto& copy : copies) {
        auto* dst_addr = std::bit_cast<u8*>(arena_base + copy.srcOffset);
        memory->TryWriteBacking(dst_addr, download.mapped + (copy.dstOffset - download.offset),
                                copy.size);
    }
    memory_tracker->UnmarkRegionAsGpuModified(device_addr, size);
}

std::pair<const Buffer*, u64> BufferCache::ObtainBuffer(VAddr device_addr, u32 size,
                                                        bool is_written, bool is_texel_buffer) {
    // For read-only buffers use device local stream buffer to reduce renderpass breaks.
    if (!is_written && size <= STREAM_THRESHOLD && !IsRegionGpuModified(device_addr, size)) {
        // A CPU snapshot taken now: later GPU writes to this range cannot affect the draw, so it
        // is not a pass dependency.
        const auto [data, offset] = stream_buffer.Map(size, instance.UniformMinAlignment());
        memory->CopySparseMemory(device_addr, data, size);
        stream_buffer.Commit();
        if (AmdGpu::Pm4Stats::armed.load(std::memory_order_relaxed)) [[unlikely]] {
            AmdGpu::Pm4Stats::NoteStreamCopy(liverpool->diagnostic_guest_flip, device_addr, size,
                                             data);
        }
        return {&stream_buffer, offset};
    }
    // Pass dependency tracking: the draw being prepared reads/writes this GPU range.
    scheduler.StageAccess(device_addr, size, is_written);
    const u64 first_block = device_addr >> block_shift;
    const u64 last_block = (device_addr + size - 1) >> block_shift;
    const auto* arena = GetArena(first_block, last_block);
    EnsureResident(arena, first_block, last_block);
    SynchronizeMemory(arena, device_addr, size, is_written, is_texel_buffer);
    if (is_written) {
        gpu_modified_ranges.Add(device_addr, size);
    }
    return {arena, arena->Offset(device_addr)};
}

std::pair<const Buffer*, u64> BufferCache::ObtainBufferForImage(VAddr device_addr, u32 size) {
    if (IsRegionGpuModified(device_addr, size)) {
        return ObtainBuffer(device_addr, size, false);
    }
    const auto staging = staging_pool.Request(size, VideoCore::MemoryType::HostUncached,
                                              instance.StorageMinAlignment());
    memory->CopySparseMemory(device_addr, staging.mapped, staging.size);
    staging.Flush();
    return {staging.buffer, staging.offset};
}

bool BufferCache::IsRegionCpuModified(VAddr addr, size_t size) {
    return memory_tracker->IsRegionCpuModified(addr, size);
}

bool BufferCache::IsRegionGpuModified(VAddr addr, size_t size) {
    return memory_tracker->IsRegionGpuModified(addr, size);
}

void BufferCache::ProcessFaultBuffer() {
    fault_manager->ProcessFaultBuffer();
}

void BufferCache::SynchronizeDmaBuffers() {
    for (const auto& range : resident_ranges) {
        const u64 page = range.start >> (ARENA_PAGE_BITS - block_shift);
        const VAddr device_addr = range.start << block_shift;
        const u64 size = (range.end - range.start) << block_shift;
        SynchronizeMemory(address_space[page], device_addr, size, false, false);
    }
}

const Buffer* BufferCache::GetArena(u64 first_block, u64 last_block) {
    const u64 first_page = first_block >> blocks_per_arena_page_shift;
    const u64 last_page = last_block >> blocks_per_arena_page_shift;
    ASSERT_MSG(last_page - first_page <= 1,
               "Buffer request cannot span more than two VA arena pages");

    const auto* first_arena = address_space[first_page];
    const auto* last_arena = address_space[last_page];
    if (first_arena == last_arena) {
        if (!first_arena) {
            const u64 base_block = Common::AlignDownPow2<u64>(first_block, blocks_per_arena_page);
            const u64 num_pages = last_page - first_page + 1;
            const auto* new_arena =
                &arenas.emplace_back(instance, base_block << block_shift,
                                     num_pages << ARENA_PAGE_BITS, MemoryType::Sparse);
            address_space[first_page] = new_arena;
            address_space[last_page] = new_arena;
        }
        return address_space[first_page];
    }

    LOG_WARNING(Render, "Migrating arena");

    const u64 first_addr = first_arena ? first_arena->cpu_addr : (first_page << ARENA_PAGE_BITS);
    const u64 first_size = first_arena ? first_arena->size_bytes : ARENA_PAGE_SIZE;
    const u64 last_size = last_arena ? last_arena->size_bytes : ARENA_PAGE_SIZE;

    const u64 base_block = first_addr >> block_shift;
    const u64 total_size = first_size + last_size;
    const u64 end_block = (first_addr + total_size) >> block_shift;
    auto* new_arena = &arenas.emplace_back(instance, first_addr, total_size, MemoryType::Sparse);
    auto* bind = BindsForArena(new_arena);
    resident_ranges.ForEachInRange(base_block, end_block, [&](const Backing& backing) {
        const u64 start = std::max(base_block, backing.start);
        const u64 end = std::min(end_block, backing.end);
        bind->binds.push_back(vk::SparseMemoryBind{
            .resourceOffset = (start - base_block) << block_shift,
            .size = (end - start) << block_shift,
            .memory = backing.memory,
            .memoryOffset = backing.offset + ((start - backing.start) << block_shift),
        });
    });

    u64 base_page = first_addr >> ARENA_PAGE_BITS;
    for (u32 page = 0; page < (first_size >> ARENA_PAGE_BITS); ++page) {
        address_space[base_page + page] = new_arena;
    }
    base_page = last_page;
    for (u32 page = 0; page < (last_size >> ARENA_PAGE_BITS); ++page) {
        address_space[base_page + page] = new_arena;
    }
    return new_arena;
}

void BufferCache::EnsureResident(const Buffer* arena, u64 first_block, u64 last_block) {
    u32 resident_blocks{};
    IntervalList bind_ranges;
    resident_ranges.ForEachGap(first_block, last_block + 1, [&](u64 start, u64 end) {
        resident_blocks += end - start;
        bind_ranges.Add({start, end});
    });

    if (bind_ranges.Empty()) {
        return;
    }

    const vk::MemoryAllocateInfo alloc_info = {
        .allocationSize = resident_blocks << block_shift,
        .memoryTypeIndex = arena_memory_type_index,
    };
    const auto device_memory = Vulkan::Check(instance.GetDevice().allocateMemory(alloc_info));

    boost::container::small_vector<vk::BufferCopy, 8> copies;
    const auto staging =
        staging_pool.Request(resident_blocks * sizeof(vk::DeviceAddress), MemoryType::HostUncached);

    u64 memory_offset{};
    ArenaBinds* binds = BindsForArena(arena);
    auto* bda_addrs = reinterpret_cast<vk::DeviceAddress*>(staging.mapped);
    u64 offset = staging.offset;
    for (const auto& range : bind_ranges) {
        Backing backing;
        backing.start = range.start;
        backing.end = range.end;
        backing.memory = device_memory;
        backing.offset = memory_offset;
        resident_ranges.Add(backing);

        LOG_INFO(Render, "Making range start={}, end={} resident", backing.start, backing.end);

        const auto& bind = binds->binds.emplace_back(vk::SparseMemoryBind{
            .resourceOffset = (range.start << block_shift) - arena->cpu_addr,
            .size = (range.end - range.start) << block_shift,
            .memory = device_memory,
            .memoryOffset = memory_offset,
        });
        memory_offset += bind.size;

        for (u32 block = 0; block < bind.size; block += block_size) {
            *(bda_addrs++) = arena->BufferDeviceAddress() + bind.resourceOffset + block;
        }
        const u64 copy_size = (backing.end - backing.start) * sizeof(vk::DeviceAddress);
        copies.emplace_back(offset, backing.start * sizeof(vk::DeviceAddress), copy_size);
        offset += copy_size;
    }

    staging.Flush();
    runtime.CopyBuffer(staging.buffer, bda_pagetable_buffer.get(), copies);
}

bool BufferCache::SynchronizeMemory(const Buffer* arena, VAddr device_addr, u32 size,
                                    bool is_written, bool is_texel_buffer) {
    boost::container::small_vector<vk::BufferCopy, 4> copies;
    size_t total_size_bytes{};
    Vulkan::StagingBufferRef staging{};
    const auto uploaded_bytes = memory_tracker->SnapshotForUpload(
        device_addr, size, is_written,
        [&](u64 capacity) {
            // All tracking locks are released here, including on a capacity retry. Reserve the
            // copy metadata too: the snapshot callback must not allocate.
            copies.reserve(capacity / TRACKER_BYTES_PER_PAGE);
            staging = staging_pool.Request(capacity, MemoryType::HostUncached);
        },
        [&](u64 addr, u64 range_size) {
            memory->CopySparseMemory(addr, staging.mapped + total_size_bytes, range_size);
            copies.emplace_back(staging.offset + total_size_bytes, addr - arena->cpu_addr,
                                range_size);
            total_size_bytes += range_size;
        });

    // Vulkan publication/retirement never runs under a tracking lock.
    if (uploaded_bytes != 0) {
        Common::Profiler::Scope upload_scope{"Buffer.Upload"};
        staging.buffer->Flush(staging.offset, uploaded_bytes);
        runtime.CopyBuffer(staging.buffer, arena, copies);
    }
    if (is_texel_buffer && !is_written) {
        return SynchronizeMemoryFromImage(arena, device_addr, size);
    }
    return false;
}

bool BufferCache::SynchronizeMemoryFromImage(const Buffer* arena, VAddr device_addr, u32 size) {
    if (auto type = texture_cache.IsMeta(device_addr)) {
        if (*type == TextureCache::MetaType::HTile) {
            static constexpr u32 ZmaskUncompressed = 0xf;
            runtime.FillBuffer(arena, arena->Offset(device_addr), size, ZmaskUncompressed);
            return true;
        } else {
            LOG_WARNING(Render_Vulkan, "Unhandled metadata type {}", magic_enum::enum_name(*type));
        }
    }
    const ImageId image_id = texture_cache.FindImageFromRange(device_addr, size);
    if (!image_id) {
        return false;
    }
    Image& image = texture_cache.GetImage(image_id);
    ASSERT_MSG(device_addr == image.info.guest_address,
               "Texel buffer aliases image subresources {:x} : {:x}", device_addr,
               image.info.guest_address);
    // Bounded: which GPU image is being read through a formatted buffer view and how large the
    // guest-layout tiling it forces is. gpu_memory request re-arms the budget.
    if (scheduler.TakePassBreakLog()) {
        LOG_INFO(Render_Vulkan,
                 "Internal scale: texel buffer sync from image {}x{} {} L:{} M:{} {:#x} request={} "
                 "guest_size={} scaled={} tiled={}",
                 image.info.size.width, image.info.size.height,
                 vk::to_string(image.info.pixel_format), image.info.resources.layers,
                 image.info.resources.levels, image.info.guest_address, size,
                 image.info.guest_size, image.IsScaled(), bool(image.info.props.is_tiled));
    }
    texture_cache.RecordImageBufferSync(image.info.guest_size);
    const u64 arena_offset = arena->Offset(device_addr);
    boost::container::small_vector<vk::BufferImageCopy, 8> buffer_copies;
    u32 copy_size = 0;
    for (u32 mip = 0; mip < image.info.resources.levels; mip++) {
        const auto& mip_info = image.info.mips_layout[mip];
        const u32 width = std::max(image.info.size.width >> mip, 1u);
        const u32 height = std::max(image.info.size.height >> mip, 1u);
        const u32 depth = std::max(image.info.size.depth >> mip, 1u);
        if (arena_offset + mip_info.offset + mip_info.size > arena->size_bytes) {
            break;
        }
        buffer_copies.push_back(vk::BufferImageCopy{
            .bufferOffset = mip_info.offset,
            .bufferRowLength = mip_info.pitch,
            .bufferImageHeight = mip_info.height,
            .imageSubresource{
                .aspectMask = image.aspect_mask & ~vk::ImageAspectFlagBits::eStencil,
                .mipLevel = mip,
                .baseArrayLayer = 0,
                .layerCount = image.info.resources.layers,
            },
            .imageOffset = {0, 0, 0},
            .imageExtent = {width, height, depth},
        });
        copy_size += mip_info.size;
    }
    if (buffer_copies.empty()) {
        return false;
    }
    // The tiling writes the arena outside the barrier tracker; order it against earlier accesses
    // and register the write for later ones.
    if (runtime.IsBufferAccessed(arena, arena_offset, copy_size, true)) {
        runtime.FlushBarriers();
    }
    auto& tile_manager = texture_cache.GetTileManager();
    tile_manager.TileImage(image, buffer_copies, arena->Handle(), arena_offset, copy_size);
    runtime.AccessBuffer(arena, arena_offset, copy_size, vk::PipelineStageFlagBits2::eAllCommands,
                         vk::AccessFlagBits2::eShaderWrite | vk::AccessFlagBits2::eTransferWrite);
    return true;
}

void BufferCache::SubmitPendingArenaBinds(Vulkan::SubmitInfo& info) {
    if (pending_binds.empty()) {
        return;
    }

    std::vector<vk::SparseBufferMemoryBindInfo> buffer_binds;
    buffer_binds.reserve(pending_binds.size());

    for (const auto& binds : pending_binds) {
        buffer_binds.emplace_back(vk::SparseBufferMemoryBindInfo{
            .buffer = binds.arena->Handle(),
            .bindCount = static_cast<u32>(binds.binds.size()),
            .pBinds = binds.binds.data(),
        });
    }

    const u64 signal_tick = memory_semaphore.NextTick();
    const auto signal_sema = memory_semaphore.Handle();

    const vk::TimelineSemaphoreSubmitInfo timeline_si = {
        .signalSemaphoreValueCount = 1u,
        .pSignalSemaphoreValues = &signal_tick,
    };

    const vk::BindSparseInfo sparse_info = {
        .pNext = &timeline_si,
        .bufferBindCount = static_cast<u32>(buffer_binds.size()),
        .pBufferBinds = buffer_binds.data(),
        .signalSemaphoreCount = 1u,
        .pSignalSemaphores = &signal_sema,
    };

    info.AddWait(signal_sema, signal_tick);
    vk::Result submit_result;
    {
        // The submission worker uses the same queue from its own thread. Binds only add
        // residency, so queueing them ahead of command buffers still in the worker FIFO is safe.
        Common::Profiler::Scope scope{"Buffer.BindSparse"};
        std::scoped_lock lock{instance.QueueMutex()};
        submit_result = instance.GetGraphicsQueue().bindSparse(sparse_info);
    }
    ASSERT_MSG(submit_result == vk::Result::eSuccess, "vkQueueBindSparse failed: {}",
               vk::to_string(submit_result));
    memory_semaphore.Submitted(signal_tick);

    pending_binds.clear();
}

} // namespace VideoCore
