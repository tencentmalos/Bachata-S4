// SPDX-FileCopyrightText: Copyright 2024-2026 shadPS4 Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#pragma once

#include <algorithm>
#include <atomic>
#include <bit>
#include <deque>
#include <memory>
#include <mutex>
#include <type_traits>
#include <vector>
#include <boost/container/small_vector.hpp>

#include "common/alignment.h"
#include "common/debug.h"
#include "common/scope_exit.h"
#include "common/types.h"
#include "core/address_space.h"
#include "core/emulator_settings.h"
#include "video_core/buffer_cache/gpu_byte_keeper.h"
#include "video_core/buffer_cache/region_manager.h"

namespace VideoCore {

class MemoryTracker {
public:
    static constexpr size_t MAX_CPU_PAGE_BITS = 40;
    static constexpr size_t NUM_HIGH_PAGES = 1ULL << (MAX_CPU_PAGE_BITS - TRACKER_HIGHER_PAGE_BITS);
    static constexpr size_t MANAGER_POOL_SIZE = 32;
    // Keep the top directory at 2 MiB even with finer Android locking. Leaf
    // directories are allocated only by the renderer and live until teardown.
    static constexpr size_t DIRECTORY_PAGE_BITS = 22;
    static constexpr size_t REGIONS_PER_DIRECTORY =
        1ULL << (DIRECTORY_PAGE_BITS - TRACKER_HIGHER_PAGE_BITS);
    static constexpr size_t NUM_DIRECTORIES = 1ULL << (MAX_CPU_PAGE_BITS - DIRECTORY_PAGE_BITS);

public:
    explicit MemoryTracker(PageManager& tracker_) : tracker{&tracker_} {}
    ~MemoryTracker() = default;

    /// Bytes written by the GPU that CPU writes beside them must not replace (readbacks off).
    GpuByteKeeper& Keeper() {
        return keeper;
    }

    /// Returns true if a region has been modified from the CPU
    bool IsRegionCpuModified(VAddr query_cpu_addr, u64 query_size) noexcept {
        return IteratePages<true>(
            query_cpu_addr, query_size, [](RegionManager* manager, u64 offset, size_t size) {
                std::scoped_lock lk{manager->lock};
                return manager->template IsRegionModified<Type::CPU>(offset, size);
            });
    }

    /// Returns true if a region has been modified from the GPU
    bool IsRegionGpuModified(VAddr query_cpu_addr, u64 query_size) noexcept {
        const bool use_flag = region_gpu_flag.load(std::memory_order_relaxed);
        return IteratePages<false>(
            query_cpu_addr, query_size,
            [use_flag](RegionManager* manager, u64 offset, size_t size) {
                if (use_flag && !manager->MayHaveGpuPages()) {
                    return false;
                }
                std::scoped_lock lk{manager->lock};
                return manager->template IsRegionModified<Type::GPU>(offset, size);
            });
    }

    /// Mark region as CPU modified, notifying the device_tracker about this change
    void MarkRegionAsCpuModified(VAddr dirty_cpu_addr, u64 query_size) {
        IteratePages<false>(dirty_cpu_addr, query_size,
                            [](RegionManager* manager, u64 offset, size_t size) {
                                std::scoped_lock lk{manager->lock};
                                manager->template ChangeRegionState<Type::CPU, true>(
                                    manager->GetCpuAddr() + offset, size);
                            });
    }

    /// Unmark region as modified from the host GPU
    void UnmarkRegionAsGpuModified(VAddr dirty_cpu_addr, u64 query_size) noexcept {
        IteratePages<false>(dirty_cpu_addr, query_size,
                            [](RegionManager* manager, u64 offset, size_t size) {
                                std::scoped_lock lk{manager->lock};
                                manager->template ChangeRegionState<Type::GPU, false>(
                                    manager->GetCpuAddr() + offset, size);
                            });
    }

    /// Replace backing without downloading the old GPU contents into the new allocation.
    void InvalidateMapping(VAddr cpu_addr, u64 size) {
        IteratePages<false>(cpu_addr, size, [](RegionManager* manager, u64 offset, size_t bytes) {
            std::scoped_lock lock{manager->lock};
            manager->template ChangeRegionState<Type::GPU, false>(manager->GetCpuAddr() + offset,
                                                                 bytes);
            manager->template ChangeRegionState<Type::CPU, true>(manager->GetCpuAddr() + offset,
                                                                bytes);
        });
    }

    /// Removes all protection from a page and ensures GPU data has been flushed if requested
    void InvalidateRegion(VAddr cpu_addr, u64 size, auto&& on_flush) noexcept {
        IteratePages<false>(
            cpu_addr, size, [this, &on_flush](RegionManager* manager, u64 offset, size_t size) {
                const bool should_flush = [&] {
                    // Perform both the GPU modification check and CPU state change with the lock
                    // in case we are racing with GPU thread trying to mark the page as GPU
                    // modified. If we need to flush the flush function is going to perform CPU
                    // state change.
                    std::scoped_lock lk{manager->lock};
                    if (manager->template IsRegionModified<Type::GPU>(offset, size)) {
                        if (EmulatorSettings.GetReadbacksMode() != GpuReadbacksMode::Disabled) {
                            return true;
                        }
                        NoteCpuWriteOverGpuPages(manager, offset, size);
                    }
                    manager->template ChangeRegionState<Type::CPU, true>(
                        manager->GetCpuAddr() + offset, size);
                    return false;
                }();
                if (should_flush) {
                    on_flush();
                }
            });
    }

    /// A guest CPU write faulted on a watched page: InvalidateRegion plus write-fault prediction
    /// (see RegionManager::MarkWriteFault). Returns the number of pages released ahead.
    size_t InvalidateRegionFromWriteFault(VAddr cpu_addr, u64 size, auto&& on_flush) noexcept {
        const bool predict = predict_write_faults.load(std::memory_order_relaxed);
        size_t predicted = 0;
        bool reached_end = false;
        VAddr last_region = 0;
        IteratePages<false>(
            cpu_addr, size, [&](RegionManager* manager, u64 offset, size_t bytes) {
                const bool should_flush = [&] {
                    std::scoped_lock lk{manager->lock};
                    if (manager->template IsRegionModified<Type::GPU>(offset, bytes)) {
                        if (EmulatorSettings.GetReadbacksMode() != GpuReadbacksMode::Disabled) {
                            return true;
                        }
                        NoteCpuWriteOverGpuPages(manager, offset, bytes);
                    }
                    predicted += manager->MarkWriteFault(manager->GetCpuAddr() + offset, bytes,
                                                         predict, &reached_end);
                    last_region = manager->GetCpuAddr();
                    return false;
                }();
                if (should_flush) {
                    on_flush();
                }
            });
        // The release ran to the end of its region: go on in the next one, under its own lock
        // (the previous region's lock is already released).
        if (reached_end) {
            if (RegionManager* next = FindRegion((last_region >> TRACKER_HIGHER_PAGE_BITS) + 1)) {
                std::scoped_lock lk{next->lock};
                const size_t more = next->ContinueReleaseAhead();
                predicted += more;
                content_prediction_counters.cross_released.fetch_add(more,
                                                                     std::memory_order_relaxed);
            }
        }
        return predicted;
    }

    // Reserve staging outside ALL tracking locks, then recheck CPU dirtiness.
    // copy_range may only copy to that reservation; allocation, GPU commands,
    // waits and retirement callbacks belong in prepare or after this returns.
    // A racing CPU writer can require one larger reservation. Reserve the full
    // page-aligned query on that retry, bounding preparation to two calls.
    // hash_pages(address, count, hashes) hashes uploaded pages for write prediction
    // (RegionManager::CleanForUpload); it runs under the same locks as copy_range.
    u64 SnapshotForUpload(VAddr query_cpu_range, u64 query_size, bool is_written, auto&& prepare,
                          auto&& copy_range, auto&& hash_pages) {
        struct Slice {
            RegionManager* manager;
            u64 offset;
            size_t size;
            RegionBits original_cpu;
        };
        boost::container::small_vector<Slice, 4> slices;
        boost::container::small_vector<RegionManager*, 16> clean_regions;
        boost::container::small_vector<std::pair<RegionDirectory*, u64>, 4> clean_directories;
        // Hashes only decide whether a released-ahead page decays (RegionManager::EndWriteCycle).
        const bool hash_contents = predict_from_contents.load(std::memory_order_relaxed) &&
                                   decay_unchanged.load(std::memory_order_relaxed);
        // Streaming pages: checked by every upload; only small read-only bindings promote.
        const StreamUpload stream_mode = is_written                        ? StreamUpload::Demote
                                         : query_size <= StreamMaxBinding ? StreamUpload::Promote
                                                                          : StreamUpload::Keep;
        // Allocation/publication precedes locking; preparation can re-enter the
        // renderer, so this transaction's scratch must remain local.
        const u64 max_bytes = Common::AlignUp(
            (query_cpu_range & (TRACKER_BYTES_PER_PAGE - 1)) + query_size, TRACKER_BYTES_PER_PAGE);
        u64 capacity = 0;
        const bool skip_clean = !is_written && region_cpu_flag.load(std::memory_order_relaxed);
        for (;;) {
            // Preparation releases all locks and can wait for the GPU. Revisit the
            // whole range on retry: a previously clean region may now have a writer.
            slices.clear();
            clean_regions.clear();
            clean_directories.clear();
            const auto collect = [&](RegionManager* manager, u64 offset, size_t size) {
                if (skip_clean && !manager->MayHaveCpuPages()) {
                    clean_regions.push_back(manager);
                    return;
                }
                slices.push_back({manager, offset, size, {}});
                manager->PrepareContentHashes();
            };
            if (!skip_clean || !directory_cpu_flag.load(std::memory_order_relaxed)) {
                IteratePages<true>(query_cpu_range, query_size, collect);
            } else {
                // An absent region still starts CPU-dirty. Only skip directory bits
                // whose managers have been fully published. Inspect the bitmap again
                // after acquiring all selected locks, just like the leaf summaries.
                constexpr u64 DirectorySize = u64{1} << DIRECTORY_PAGE_BITS;
                static_assert(REGIONS_PER_DIRECTORY < 64);
                const VAddr query_end = query_cpu_range + query_size;
                for (VAddr address = query_cpu_range; address < query_end;) {
                    const VAddr base = Common::AlignDown(address, DirectorySize);
                    const VAddr end = std::min(query_end, base + DirectorySize);
                    const u32 first = (address - base) >> TRACKER_HIGHER_PAGE_BITS;
                    const u32 last = ((end - base - 1) >> TRACKER_HIGHER_PAGE_BITS) + 1;
                    const u64 mask = ((u64{1} << (last - first)) - 1) << first;
                    auto* directory =
                        top_tier[base >> DIRECTORY_PAGE_BITS].load(std::memory_order_acquire);
                    if (!directory ||
                        (directory->present.load(std::memory_order_acquire) & mask) != mask) {
                        IteratePages<true>(address, end - address, collect);
                    } else {
                        u64 dirty = directory->cpu_dirty.load(std::memory_order_acquire) & mask;
                        if (const u64 clean = mask & ~dirty)
                            clean_directories.emplace_back(directory, clean);
                        while (dirty) {
                            const u32 bit = std::countr_zero(dirty);
                            dirty &= dirty - 1;
                            auto* manager = directory->regions[bit].load(std::memory_order_acquire);
                            const VAddr lo =
                                std::max(address, base + (u64{bit} << TRACKER_HIGHER_PAGE_BITS));
                            const VAddr hi =
                                std::min(end, base + (u64{bit + 1} << TRACKER_HIGHER_PAGE_BITS));
                            collect(manager, lo - manager->GetCpuAddr(), hi - lo);
                        }
                    }
                    address = end;
                }
            }
            u64 required = 0;
            {
                size_t locked = 0;
                bool modified = false;
                bool completed = false;
                SCOPE_EXIT {
                    for (size_t i = locked; i > 0; --i) {
                        auto& slice = slices[i - 1];
                        if (modified && !completed) {
                            slice.manager->RestoreCpuTracking(slice.original_cpu);
                        }
                        slice.manager->lock.unlock();
                    }
                };
                for (auto& slice : slices) {
                    slice.manager->lock.lock();
                    ++locked;
                }
                // A writer may have dirtied a skipped region while we waited for
                // one of the other locks. Validate after ALL selected locks are held.
                // Only this renderer clears CPU bits, so false flags here prove
                // every omitted region was clean when this validation began.
                if (std::ranges::any_of(clean_directories, [](const auto& entry) {
                        return (entry.first->cpu_dirty.load(std::memory_order_acquire) & entry.second) != 0;
                    }) || std::ranges::any_of(clean_regions, [](const auto* manager) {
                        return manager->MayHaveCpuPages();
                    })) {
                    continue;
                }
                for (auto& slice : slices) {
                    slice.original_cpu = slice.manager->template GetRegionBits<Type::CPU>();
                    slice.manager->ForEachPendingUpload(slice.manager->GetCpuAddr() + slice.offset,
                                                        slice.size, stream_mode, hash_pages,
                                                        [&](u64, u64 bytes) { required += bytes; });
                }
                if (required <= capacity) {
                    modified = true;
                    RegionManager::UploadPrediction prediction{};
                    for (auto& slice : slices) {
                        const auto slice_prediction = slice.manager->CleanForUpload(
                            slice.manager->GetCpuAddr() + slice.offset, slice.size, hash_contents,
                            stream_mode, copy_range, hash_pages);
                        prediction.hashed += slice_prediction.hashed;
                        prediction.rewritten += slice_prediction.rewritten;
                        prediction.unchanged += slice_prediction.unchanged;
                        prediction.kept += slice_prediction.kept;
                    }
                    if (prediction.kept != 0) {
                        content_prediction_counters.kept_pages.fetch_add(
                            prediction.kept, std::memory_order_relaxed);
                    }
                    if (prediction.hashed != 0) {
                        auto& counters = content_prediction_counters;
                        counters.hashed_pages.fetch_add(prediction.hashed,
                                                        std::memory_order_relaxed);
                        counters.rewritten_pages.fetch_add(prediction.rewritten,
                                                           std::memory_order_relaxed);
                        counters.unchanged_pages.fetch_add(prediction.unchanged,
                                                           std::memory_order_relaxed);
                    }
                    if (is_written) {
                        for (auto& slice : slices) {
                            slice.manager->template ChangeRegionState<Type::GPU, true>(
                                slice.manager->GetCpuAddr() + slice.offset, slice.size);
                        }
                    }
                    completed = true;
                    return required;
                }
            }
            capacity = capacity == 0 ? required : max_bytes;
            prepare(capacity);
        }
    }

    /// Call 'func' for each GPU modified range and unmark those pages as GPU modified
    template <bool clear>
    void ForEachDownloadRange(VAddr query_cpu_range, u64 query_size, auto&& func) {
        IteratePages<false>(query_cpu_range, query_size,
                            [&func](RegionManager* manager, u64 offset, size_t size) {
                                std::scoped_lock lk{manager->lock};
                                manager->template ForEachModifiedRange<Type::GPU, clear>(
                                    manager->GetCpuAddr() + offset, size, func);
                            });
    }

private:
    /**
     * @brief IteratePages Iterates L2 word manager page table.
     * @param cpu_address Start byte cpu address
     * @param size Size in bytes of the region of iterate.
     * @param func Callback for each word manager.
     * @return
     */
    /// Readbacks off, the CPU writes [offset, offset + size) of `manager`'s region and some of
    /// those pages hold GPU data: keep the GPU bytes of pages written beside them, count the
    /// others as lost. Called under the region lock.
    void NoteCpuWriteOverGpuPages(RegionManager* manager, u64 offset, u64 size) {
        const VAddr base = manager->GetCpuAddr();
        const VAddr lo = base + offset;
        const VAddr hi = lo + size;
        // Large explicit invalidations (unmaps, bulk HLE writes) take the pages over as before.
        if (size > 16 * TRACKER_BYTES_PER_PAGE) {
            Core::gpu_watch_counters.NoteGpuDataOverwrite(lo);
            return;
        }
        for (VAddr page = lo & ~(TRACKER_BYTES_PER_PAGE - 1); page < hi;
             page += TRACKER_BYTES_PER_PAGE) {
            if (!manager->template IsRegionModified<Type::GPU>(page - base,
                                                               TRACKER_BYTES_PER_PAGE)) {
                continue;
            }
            const VAddr begin = std::max(lo, page);
            const VAddr end = std::min<VAddr>(hi, page + TRACKER_BYTES_PER_PAGE);
            if (!keeper.OnCpuWrite(page, begin, end - begin)) {
                Core::gpu_watch_counters.NoteGpuDataOverwrite(begin);
            }
        }
    }

    template <bool create_region_on_fail, typename Func>
    bool IteratePages(VAddr cpu_address, size_t size, Func&& func) {
        RENDERER_TRACE;
        using FuncReturn = typename std::invoke_result<Func, RegionManager*, u64, size_t>::type;
        static constexpr bool BOOL_BREAK = std::is_same_v<FuncReturn, bool>;
        std::size_t remaining_size{size};
        std::size_t page_index{cpu_address >> TRACKER_HIGHER_PAGE_BITS};
        u64 page_offset{cpu_address & TRACKER_HIGHER_PAGE_MASK};
        while (remaining_size > 0) {
            const std::size_t copy_amount{
                std::min<std::size_t>(TRACKER_HIGHER_PAGE_SIZE - page_offset, remaining_size)};
            auto* manager{FindRegion(page_index)};
            if (manager) {
                if constexpr (BOOL_BREAK) {
                    if (func(manager, page_offset, copy_amount)) {
                        return true;
                    }
                } else {
                    func(manager, page_offset, copy_amount);
                }
            } else if constexpr (create_region_on_fail) {
                CreateRegion(page_index);
                manager = FindRegion(page_index);
                if constexpr (BOOL_BREAK) {
                    if (func(manager, page_offset, copy_amount)) {
                        return true;
                    }
                } else {
                    func(manager, page_offset, copy_amount);
                }
            }
            page_index++;
            page_offset = 0;
            remaining_size -= copy_amount;
        }
        return false;
    }

    struct RegionDirectory {
        std::array<std::atomic<RegionManager*>, REGIONS_PER_DIRECTORY> regions{};
        std::atomic<u64> cpu_dirty{};
        std::atomic<u64> present{};
    };

    RegionManager* FindRegion(std::size_t page_index) const {
        auto* directory =
            top_tier[page_index / REGIONS_PER_DIRECTORY].load(std::memory_order_acquire);
        return directory ? directory->regions[page_index % REGIONS_PER_DIRECTORY].load(
                               std::memory_order_acquire)
                         : nullptr;
    }

    // Renderer-owned allocation; fault handlers only perform acquire lookups.
    void CreateRegion(std::size_t page_index) {
        const VAddr base_cpu_addr = page_index << TRACKER_HIGHER_PAGE_BITS;
        if (free_managers.empty()) {
            manager_pool.emplace_back();
            auto& last_pool = manager_pool.back();
            for (size_t i = 0; i < MANAGER_POOL_SIZE; i++) {
                last_pool[i].Initialize(tracker, 0);
                free_managers.push_back(&last_pool[i]);
            }
        }
        // Each manager owns only its address region's dirty/protection state.
        auto* new_manager = free_managers.back();
        new_manager->SetCpuAddress(base_cpu_addr);
        free_managers.pop_back();
        const auto directory_index = page_index / REGIONS_PER_DIRECTORY;
        auto* directory = top_tier[directory_index].load(std::memory_order_acquire);
        if (!directory) {
            directories.emplace_back(std::make_unique<RegionDirectory>());
            directory = directories.back().get();
            top_tier[directory_index].store(directory, std::memory_order_release);
        }
        const u64 bit = u64{1} << (page_index % REGIONS_PER_DIRECTORY);
        new_manager->SetCpuDirectory(&directory->cpu_dirty, bit);
        directory->regions[page_index % REGIONS_PER_DIRECTORY].store(new_manager,
                                                                     std::memory_order_release);
        directory->present.fetch_or(bit, std::memory_order_release);
    }

    PageManager* tracker;
    GpuByteKeeper keeper;
    std::deque<std::array<RegionManager, MANAGER_POOL_SIZE>> manager_pool;
    std::vector<RegionManager*> free_managers;
    std::vector<std::unique_ptr<RegionDirectory>> directories;
    std::array<std::atomic<RegionDirectory*>, NUM_DIRECTORIES> top_tier{};
};

} // namespace VideoCore
