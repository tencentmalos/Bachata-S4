// SPDX-FileCopyrightText: Copyright 2024-2026 shadPS4 Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#pragma once

#include <algorithm>
#include <atomic>
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
#include "core/emulator_settings.h"
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
        return IteratePages<false>(
            query_cpu_addr, query_size, [](RegionManager* manager, u64 offset, size_t size) {
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
            cpu_addr, size, [&on_flush](RegionManager* manager, u64 offset, size_t size) {
                const bool should_flush = [&] {
                    // Perform both the GPU modification check and CPU state change with the lock
                    // in case we are racing with GPU thread trying to mark the page as GPU
                    // modified. If we need to flush the flush function is going to perform CPU
                    // state change.
                    std::scoped_lock lk{manager->lock};
                    if (EmulatorSettings.GetReadbacksMode() != GpuReadbacksMode::Disabled &&
                        manager->template IsRegionModified<Type::GPU>(offset, size)) {
                        return true;
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

    // Reserve staging outside ALL tracking locks, then recheck CPU dirtiness.
    // copy_range may only copy to that reservation; allocation, GPU commands,
    // waits and retirement callbacks belong in prepare or after this returns.
    // A racing CPU writer can require one larger reservation. Reserve the full
    // page-aligned query on that retry, bounding preparation to two calls.
    u64 SnapshotForUpload(VAddr query_cpu_range, u64 query_size, bool is_written, auto&& prepare,
                          auto&& copy_range) {
        struct Slice {
            RegionManager* manager;
            u64 offset;
            size_t size;
            RegionBits original_cpu;
        };
        boost::container::small_vector<Slice, 4> slices;
        // Allocation/publication precedes locking; preparation can re-enter the
        // renderer, so this transaction's scratch must remain local.
        IteratePages<true>(query_cpu_range, query_size,
                           [&](RegionManager* manager, u64 offset, size_t size) {
                               slices.push_back({manager, offset, size, {}});
                           });
        const u64 max_bytes = Common::AlignUp(
            (query_cpu_range & (TRACKER_BYTES_PER_PAGE - 1)) + query_size, TRACKER_BYTES_PER_PAGE);
        u64 capacity = 0;
        for (;;) {
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
                    slice.original_cpu = slice.manager->template GetRegionBits<Type::CPU>();
                    slice.manager->template ForEachModifiedRange<Type::CPU, false>(
                        slice.manager->GetCpuAddr() + slice.offset, slice.size,
                        [&](u64, u64 bytes) { required += bytes; });
                }
                if (required <= capacity) {
                    modified = true;
                    for (auto& slice : slices) {
                        slice.manager->template ForEachModifiedRange<Type::CPU, true>(
                            slice.manager->GetCpuAddr() + slice.offset, slice.size, copy_range);
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
        directory->regions[page_index % REGIONS_PER_DIRECTORY].store(new_manager,
                                                                     std::memory_order_release);
    }

    PageManager* tracker;
    std::deque<std::array<RegionManager, MANAGER_POOL_SIZE>> manager_pool;
    std::vector<RegionManager*> free_managers;
    std::vector<std::unique_ptr<RegionDirectory>> directories;
    std::array<std::atomic<RegionDirectory*>, NUM_DIRECTORIES> top_tier{};
};

} // namespace VideoCore
