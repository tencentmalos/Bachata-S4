// SPDX-FileCopyrightText: Copyright 2024 shadPS4 Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#pragma once

#include <deque>
#include <iosfwd>
#include <boost/container/small_vector.hpp>

#include "common/interval_set.h"
#include "common/types.h"
#include "core/guest_read_cache.h"
#include "video_core/buffer_cache/buffer.h"
#include "video_core/buffer_cache/fault_manager.h"
#include "video_core/buffer_cache/range_set.h"
#include "video_core/texture_cache/upload_diagnostics.h"
#include "video_core/renderer_vulkan/vk_semaphore.h"

namespace AmdGpu {
struct Liverpool;
}

namespace Core {
class MemoryManager;
}

namespace Vulkan {
class GraphicsPipeline;
struct SubmitInfo;
class Runtime;
class StagingBufferPool;
} // namespace Vulkan

namespace VideoCore {

struct Image;
class TextureCache;
class MemoryTracker;
class PageManager;

class BufferCache {
    static constexpr u64 ADDRESS_SPACE_BITS = 40;
    // Arena pages are 4 GiB unless the device's buffer size limit is smaller: a page is at most
    // half the limit, so an access straddling two pages always fits in one arena.
    static constexpr u64 MAX_ARENA_PAGE_BITS = 32;
    static constexpr u64 MIN_ARENA_PAGE_BITS = 28;
    static constexpr u64 MAX_ARENA_PAGES = u64{1} << (ADDRESS_SPACE_BITS - MIN_ARENA_PAGE_BITS);
    static constexpr u64 MIN_BLOCK_SIZE = 16_KB;
    static constexpr u64 STREAM_THRESHOLD = 16_KB;

public:
    explicit BufferCache(const Vulkan::Instance& instance, Vulkan::Scheduler& scheduler,
                         Vulkan::Runtime& runtime, AmdGpu::Liverpool* liverpool,
                         TextureCache& texture_cache, PageManager& tracker);
    ~BufferCache();

    // Renderer thread only; no queue synchronization or cross-thread cache reads.
    void AppendMemoryDiagnostics(std::ostream& out);

    /// Returns a pointer to GDS device local buffer.
    [[nodiscard]] const Buffer* GetGdsBuffer() const noexcept {
        return &gds_buffer;
    }

    /// Retrieves the device local BDA page table buffer for uploads and barriers.
    [[nodiscard]] Buffer* GetBdaPageTableBuffer() {
        EnsurePageTable();
        return bda_pagetable_buffer.get();
    }

    /// Shader descriptor containing the table's device address. The full table can
    /// exceed maxStorageBufferRange and must not be bound as a single SSBO.
    [[nodiscard]] Buffer* GetBdaPageTableRootBuffer() {
        EnsurePageTable();
        return bda_pagetable_root.get();
    }

    /// Retrieves the fault buffer.
    [[nodiscard]] Buffer* GetFaultBuffer() noexcept {
        return fault_manager->GetFaultBuffer();
    }

    /// Retrieves the stream buffer.
    StreamBuffer& GetStreamBuffer() noexcept {
        if (staged_stream_buffer && UploadDiagnostics::stream_dma.load(std::memory_order_relaxed)) {
            return *staged_stream_buffer;
        }
        if (host_stream_buffer && UploadDiagnostics::stream_host.load(std::memory_order_relaxed)) {
            return *host_stream_buffer;
        }
        return stream_buffer;
    }

    /// For small parameters of internal passes held by their owner for the whole session: a
    /// stream buffer the device reads where it is, so they never wait for a staged copy.
    StreamBuffer& GetParameterStreamBuffer() noexcept {
        return host_stream_buffer ? *host_stream_buffer : stream_buffer;
    }

    /// Streamed buffers hold CPU snapshots (ObtainBuffer's small read-only ranges, per-draw
    /// constants) that the GPU only reads.
    [[nodiscard]] bool IsStreamBuffer(const Buffer* buffer) const noexcept {
        return buffer == &stream_buffer || (host_stream_buffer && buffer == &*host_stream_buffer) ||
               (staged_stream_buffer && buffer == &*staged_stream_buffer);
    }

    /// Copies the staged stream buffer's writes of this submission into device memory on the
    /// transfer queue and makes the submission wait for it. Called on every submission.
    void SubmitStagedStream(Vulkan::SubmitInfo& info);

    /// Returns minimum granularity of a sparse memory bind.
    u32 GetSparsePageShift() const noexcept {
        return block_shift;
    }

    /// Invalidates any buffer in the logical page range.
    void InvalidateMemory(VAddr device_addr, u64 size);

    /// A guest write faulted on a watched page: invalidate it and release the pages the CPU is
    /// predicted to rewrite (MemoryTracker::InvalidateRegionFromWriteFault).
    void InvalidateMemoryFromWriteFault(VAddr device_addr, u64 size);

    // New backing replaces cached sparse zeros and discards writes to old holes.
    // No readback into the newly mapped allocation is permitted.
    void InvalidateMapping(VAddr device_addr, u64 size);

    /// Memory unmapped: forget which of its bytes the GPU wrote.
    void ForgetGpuWrites(VAddr device_addr, u64 size);

    /// Flushes any GPU modified buffer in the logical page range back to CPU memory.
    void ReadMemory(VAddr device_addr, u64 size, bool is_write = false);

    /// GPU replay: writes every GPU-modified range back to guest memory, after folding in the
    /// bytes the CPU wrote over GPU data since (readbacks off). Command processor thread.
    void WriteBackGpuModified();

    /// GPU replay: the GDS contents once the GPU finished writing them.
    std::vector<u8> ReadGds();
    /// GPU replay: restores the GDS contents.
    void WriteGds(std::span<const u8> data);

    /// Streamed copies in this scope share one VM read lock (see MemoryManager::LockReads).
    /// The scope must not reach the VM writer lock; nested scopes are counted.
    void BeginStreamReads();
    void EndStreamReads();

    /// Finds a buffer for the specified region.
    [[nodiscard]] std::pair<const Buffer*, u64> ObtainBuffer(VAddr device_addr, u32 size,
                                                             bool is_written,
                                                             bool is_texel_buffer = false);

    /// Establish page-table residency even for small read-only ranges. DMA faults
    /// cannot be serviced by a temporary stream copy that is absent from the table.
    [[nodiscard]] std::pair<const Buffer*, u64> ObtainResidentBuffer(VAddr device_addr, u32 size,
                                                                     bool is_written,
                                                                     bool is_texel_buffer = false);

    /// Attempts to obtain a buffer without modifying the cache contents.
    [[nodiscard]] std::pair<const Buffer*, u64> ObtainBufferForImage(VAddr device_addr, u32 size);

    /// Return true when a region is modified from the CPU
    [[nodiscard]] bool IsRegionCpuModified(VAddr addr, size_t size);

    /// Return true when a region is modified from the GPU
    [[nodiscard]] bool IsRegionGpuModified(VAddr addr, size_t size);

    /// Processes the fault buffer.
    void ProcessFaultBuffer();

    /// Synchronizes all buffers needed for DMA.
    // Empty means unbounded. Bounds are conservative half-open guest ranges.
    static constexpr u64 DmaAddressSpaceSize = u64{1} << ADDRESS_SPACE_BITS;
    using DmaRange = std::pair<VAddr, VAddr>;
    struct DmaBufferRead {
        const Buffer* buffer;
        u64 offset;
        u32 size;
    };
    // Returned reads must join the draw/dispatch's ordinary buffer access tracking,
    // after all uploads have been recorded. A BDA descriptor alone does not track
    // the buffers reached through its page table.
    boost::container::small_vector<DmaBufferRead, 16> SynchronizeDmaBuffers(
        std::span<const DmaRange> ranges = {});

    /// Commits pending sparse buffer memory binds. Must be called before every scheduler submit.
    void SubmitPendingArenaBinds(Vulkan::SubmitInfo& info);

private:
    struct ArenaBinds {
        const Buffer* arena;
        boost::container::small_vector<vk::SparseMemoryBind, 32> binds;
    };

    ArenaBinds* BindsForArena(const Buffer* arena) {
        auto it = std::ranges::find(pending_binds, arena, &ArenaBinds::arena);
        if (it != pending_binds.end()) {
            return std::addressof(*it);
        }
        return &pending_binds.emplace_back(arena);
    }

    const Buffer* GetArena(u64 first_block, u64 last_block);

    /// Creates the BDA page table (2^(40 - block_shift) addresses: 512 MiB with 16 KiB blocks)
    /// with entries for the blocks already resident. Only shaders reading guest memory directly
    /// (the direct memory access setting) use it, so without that setting it is made when first
    /// asked for, and normally never.
    void EnsurePageTable();

    void EnsureResident(const Buffer* arena, u64 first_block, u64 last_block);

    /// Binds one block of a probe buffer and copies a pattern through it; throws when the
    /// driver accepts the bind without backing the range with the bound memory.
    void VerifySparseResidency();

    void DownloadMemory(const Buffer* arena, VAddr device_addr, u64 size);

    bool SynchronizeMemory(const Buffer* arena, VAddr device_addr, u32 size, bool is_written,
                           bool is_texel_buffer);

    bool SynchronizeMemoryFromImage(const Buffer* arena, VAddr device_addr, u32 size);

    /// Raw (unformatted) read of a whole GPU-written image, e.g. a compute shader copying a
    /// render target with buffer_load: tile the image's newer contents into the arena first.
    bool SynchronizeMemoryFromGpuImage(const Buffer* arena, VAddr device_addr, u32 size);

    /// Tiles `image` (which starts at device_addr) back into the arena in guest layout.
    bool TileImageIntoArena(const Buffer* arena, VAddr device_addr, u32 size, Image& image);

    const Vulkan::Instance& instance;
    Vulkan::Scheduler& scheduler;
    Vulkan::Runtime& runtime;
    Vulkan::StagingBufferPool& staging_pool;
    AmdGpu::Liverpool* liverpool;
    Core::MemoryManager* memory;
    TextureCache& texture_cache;
    std::unique_ptr<MemoryTracker> memory_tracker;

    StreamBuffer stream_buffer;
    // Discrete GPUs only: CPU writes through the PCIe BAR run at a few hundred MB/s on some
    // systems. Shaders reading the data from host memory instead pay the PCIe latency.
    std::optional<StreamBuffer> host_stream_buffer;
    // Discrete GPUs with a copy-only queue: the CPU writes host memory, and that queue copies
    // each submission's data into VRAM before the submission runs (staged_copier).
    std::optional<StreamBuffer> staged_stream_buffer;
    class StagedStreamCopier;
    std::unique_ptr<StagedStreamCopier> staged_copier;
    Buffer gds_buffer;
    RangeSet gpu_modified_ranges;
    // Mapping lookups saved for streamed buffer copies (GPU command thread only).
    Core::GuestReadCache stream_read_cache;
    u32 stream_read_depth{};
    // Streamed copies: count, bytes and sizes (<=256 B, <=1 KiB, <=4 KiB, larger).
    u64 stream_copies{};
    u64 stream_bytes{};
    std::array<u64, 4> stream_sizes{};
    // CPU-dirty data copied into arenas for a binding: calls, bytes, copy regions, calls for
    // written bindings, and calls by binding size (<=16 KiB, <=64 KiB, <=256 KiB, larger).
    struct {
        u64 calls{}, bytes{}, regions{}, written{};
        std::array<u64, 4> binding_sizes{};
    } arena_uploads;

    std::unique_ptr<FaultManager> fault_manager;
    std::unique_ptr<Buffer> bda_pagetable_buffer;
    std::unique_ptr<Buffer> bda_pagetable_root;

    std::array<const Buffer*, MAX_ARENA_PAGES> address_space{};
    std::deque<Buffer> arenas;
    std::vector<ArenaBinds> pending_binds;
    Vulkan::Semaphore memory_semaphore;

    struct Backing : public Interval {
        vk::DeviceMemory memory;
        u64 offset;
        constexpr bool CanMergeWith(const Backing& other) const noexcept {
            return memory == other.memory && offset + (end - start) == other.offset;
        }
        constexpr Backing SubRange(u64 a, u64 b) const noexcept {
            return {{a, b}, memory, offset + (a - start)};
        }
    };
    IntervalList<Backing> resident_ranges;

    u32 arena_memory_type_index{};
    u32 block_size{};
    u32 block_shift{};
    u32 blocks_per_arena_page{};
    u32 blocks_per_arena_page_shift{};
    u32 arena_page_bits{};
    u64 max_arena_size{};
    u64 arena_migrations{};
    u32 raw_image_sync_logs{};
};

} // namespace VideoCore
