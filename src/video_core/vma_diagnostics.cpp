// SPDX-License-Identifier: GPL-2.0-or-later
#include "video_core/vma_diagnostics.h"

#include <algorithm>
#include <chrono>
#include <fstream>
#include <map>
#include <mutex>
#include <ostream>
#include <sstream>
#include <string>
#include <vector>
#include <vk_mem_alloc.h>

namespace VideoCore::VmaDiagnostics {
struct Allocation {
    VkDeviceSize bytes{}, requested{};
    uint32_t type{}, properties{};
    std::string category;
    bool retiring{};
};
struct Group {
    uint64_t bytes{}, requested{}, count{}, retiring{}, retiring_count{}, peak{}, retiring_peak{};
};
struct Ledger {
    HostMemoryPolicy policy;
    std::map<std::string, Group> groups;
    std::map<VmaAllocation, Allocation> allocations;
    uint64_t reserved{}, reserved_peak{}, blocks{}, blocks_peak{};
    uint64_t version{}, operations{}, created{}, freed{}, live{}, peak{}, created_count{},
        freed_count{};
    uint64_t retiring{}, retiring_count{}, retiring_peak{};
    uint64_t device_created{}, device_freed{}, device_create_count{}, device_free_count{};
};
static std::mutex mutex;
static std::map<VmaAllocator, Ledger> ledgers;
static void Account(Ledger& ledger, const Allocation& item, bool add) {
    auto& group = ledger.groups[item.category + "/type" + std::to_string(item.type) + "/flags" +
                                std::to_string(item.properties)];
    if (add) {
        group.bytes += item.bytes;
        group.requested += item.requested;
        ++group.count;
        if (item.retiring) {
            group.retiring += item.bytes;
            ++group.retiring_count;
            ledger.retiring += item.bytes;
            ++ledger.retiring_count;
            ledger.retiring_peak = std::max(ledger.retiring_peak, ledger.retiring);
        }
        group.peak = std::max(group.peak, group.bytes);
        group.retiring_peak = std::max(group.retiring_peak, group.retiring);
    } else {
        group.bytes -= item.bytes;
        group.requested -= item.requested;
        --group.count;
        if (item.retiring) {
            group.retiring -= item.bytes;
            --group.retiring_count;
            ledger.retiring -= item.bytes;
            --ledger.retiring_count;
        }
    }
}
void VKAPI_PTR DeviceAllocated(VmaAllocator allocator, uint32_t, VkDeviceMemory, VkDeviceSize bytes,
                               void*) {
    std::scoped_lock lock{mutex};
    auto& ledger = ledgers[allocator];
    ledger.reserved += bytes;
    ledger.device_created += bytes;
    ++ledger.device_create_count;
    ledger.reserved_peak = std::max(ledger.reserved_peak, ledger.reserved);
    ledger.blocks_peak = std::max(ledger.blocks_peak, ++ledger.blocks);
    ++ledger.version;
}
void VKAPI_PTR DeviceFreed(VmaAllocator allocator, uint32_t, VkDeviceMemory, VkDeviceSize bytes,
                           void*) {
    std::scoped_lock lock{mutex};
    auto& ledger = ledgers[allocator];
    ledger.reserved -= bytes;
    ledger.device_freed += bytes;
    ++ledger.device_free_count;
    --ledger.blocks;
    ++ledger.version;
}
const VmaDeviceMemoryCallbacks memory_callbacks{DeviceAllocated, DeviceFreed, nullptr};
VkResult CreateAllocator(VmaAllocatorCreateInfo info, HostMemoryPolicy policy,
                         VmaAllocator* allocator) {
    info.preferredLargeHeapBlockSize = policy.preferred_block_bytes;
    info.pDeviceMemoryCallbacks = &memory_callbacks;
    const auto result = vmaCreateAllocator(&info, allocator);
    if (result == VK_SUCCESS) {
        std::scoped_lock lock{mutex};
        ledgers[*allocator].policy = policy;
    }
    return result;
}
// No Vulkan call or GPU wait runs under the ledger lock. Version/operations
// identify snapshots that overlapped allocation/free work on another thread.
HostMemoryPolicy Begin(VmaAllocator allocator) {
    std::scoped_lock lock{mutex};
    auto& ledger = ledgers[allocator];
    ++ledger.operations;
    ++ledger.version;
    return ledger.policy;
}
void Created(VmaAllocator allocator, VmaAllocation allocation, VkDeviceSize requested,
             std::string category) {
    Allocation record{};
    if (allocation) {
        VmaAllocationInfo info{};
        vmaGetAllocationInfo(allocator, allocation, &info);
        record.bytes = info.size;
        record.type = info.memoryType;
        record.requested = requested;
        record.category = std::move(category);
        vmaGetAllocationMemoryProperties(allocator, allocation, &record.properties);
        vmaSetAllocationName(allocator, allocation, record.category.c_str());
    }
    std::scoped_lock lock{mutex};
    auto& ledger = ledgers[allocator];
    if (allocation) {
        ledger.live += record.bytes;
        ledger.created += record.bytes;
        ledger.peak = std::max(ledger.peak, ledger.live);
        ++ledger.created_count;
        Account(ledger, record, true);
        ledger.allocations.emplace(allocation, std::move(record));
    }
    --ledger.operations;
    ++ledger.version;
}
void Freed(VmaAllocator allocator, VmaAllocation allocation) {
    std::scoped_lock lock{mutex};
    auto& ledger = ledgers[allocator];
    if (const auto it = ledger.allocations.find(allocation); it != ledger.allocations.end()) {
        Account(ledger, it->second, false);
        ledger.live -= it->second.bytes;
        ledger.freed += it->second.bytes;
        ++ledger.freed_count;
        ledger.allocations.erase(it);
    }
    ++ledger.version;
}
void Complete(VmaAllocator allocator) {
    std::scoped_lock lock{mutex};
    auto& ledger = ledgers[allocator];
    --ledger.operations;
    ++ledger.version;
}
VkResult CreateImage(VmaAllocator allocator, const VkImageCreateInfo* ci,
                     const VmaAllocationCreateInfo* ai, VkImage* image, VmaAllocation* allocation,
                     VmaAllocationInfo* info, const char* category) {
    const auto policy = Begin(allocator);
    auto effective = *ai;
    if (policy.compact_allocations && !(effective.flags & VMA_ALLOCATION_CREATE_STRATEGY_MASK))
        effective.flags |= VMA_ALLOCATION_CREATE_STRATEGY_MIN_MEMORY_BIT;
    const auto result = vmaCreateImage(allocator, ci, &effective, image, allocation, info);
    Created(allocator, result == VK_SUCCESS ? *allocation : nullptr, 0, category);
    return result;
}
VkResult CreateBuffer(VmaAllocator allocator, const VkBufferCreateInfo* ci,
                      const VmaAllocationCreateInfo* ai, VkBuffer* buffer,
                      VmaAllocation* allocation, VmaAllocationInfo* info, std::string category) {
    const auto policy = Begin(allocator);
    auto effective = *ai;
    if (policy.compact_allocations && !(effective.flags & VMA_ALLOCATION_CREATE_STRATEGY_MASK))
        effective.flags |= VMA_ALLOCATION_CREATE_STRATEGY_MIN_MEMORY_BIT;
    const auto result = vmaCreateBuffer(allocator, ci, &effective, buffer, allocation, info);
    Created(allocator, result == VK_SUCCESS ? *allocation : nullptr, ci->size, std::move(category));
    return result;
}
void DestroyImage(VmaAllocator allocator, VkImage image, VmaAllocation allocation) {
    Begin(allocator);
    Freed(allocator, allocation);
    vmaDestroyImage(allocator, image, allocation);
    Complete(allocator);
}
void DestroyBuffer(VmaAllocator allocator, VkBuffer buffer, VmaAllocation allocation) {
    Begin(allocator);
    Freed(allocator, allocation);
    vmaDestroyBuffer(allocator, buffer, allocation);
    Complete(allocator);
}
void Tag(VmaAllocator allocator, VmaAllocation allocation, const char* category, bool retiring) {
    std::scoped_lock lock{mutex};
    auto it = ledgers.find(allocator);
    if (it == ledgers.end())
        return;
    auto found = it->second.allocations.find(allocation);
    if (found == it->second.allocations.end())
        return;
    if ((!category || found->second.category == category) && found->second.retiring == retiring)
        return;
    Account(it->second, found->second, false);
    if (category)
        found->second.category = category;
    found->second.retiring = retiring;
    Account(it->second, found->second, true);
    ++it->second.version;
}
void End(VmaAllocator allocator) {
    std::scoped_lock lock{mutex};
    ledgers.erase(allocator);
}
void Append(VmaAllocator allocator, const VkPhysicalDeviceMemoryProperties& properties,
            bool driver_budget, std::ostream& out) {
    const auto sample_begin = std::chrono::steady_clock::now();
    Ledger sampled;
    {
        std::scoped_lock lock{mutex};
        if (auto it = ledgers.find(allocator); it != ledgers.end())
            sampled = it->second;
    }
    VmaTotalStatistics statistics{};
    vmaCalculateStatistics(allocator, &statistics);
    VmaBudget budgets[VK_MAX_MEMORY_HEAPS]{};
    vmaGetHeapBudgets(allocator, budgets);
    bool stable;
    {
        std::scoped_lock lock{mutex};
        const auto it = ledgers.find(allocator);
        stable = it != ledgers.end() && !sampled.operations && !it->second.operations &&
                 sampled.version == it->second.version;
    }
    const auto& total = statistics.total;
    out << "vma_policy=" << TextureQualityName(sampled.policy.quality)
        << " preferred_block_bytes=" << sampled.policy.preferred_block_bytes
        << " compact_allocations=" << sampled.policy.compact_allocations
        << " staging_policy_bytes=" << sampled.policy.staging_bytes
        << " idle_asset_submits=" << sampled.policy.idle_asset_submits
        << " idle_evictions_per_sweep=" << sampled.policy.idle_evictions_per_sweep << '\n';
    out << "vma_consistent=" << stable << " ledger_version=" << sampled.version
        << " vma_reserved_bytes=" << total.statistics.blockBytes
        << " vma_live_allocation_bytes=" << total.statistics.allocationBytes
        << " vma_free_block_bytes="
        << total.statistics.blockBytes - total.statistics.allocationBytes
        << " vma_block_count=" << total.statistics.blockCount
        << " vma_allocation_count=" << total.statistics.allocationCount
        << " vma_free_range_count=" << total.unusedRangeCount
        << " vma_largest_free_range=" << (total.unusedRangeCount ? total.unusedRangeSizeMax : 0)
        << '\n';
    out << "vma_reserved_peak_bytes=" << sampled.reserved_peak
        << " vma_blocks_peak=" << sampled.blocks_peak << " vma_reserved_reconciliation_delta="
        << int64_t(total.statistics.blockBytes) - int64_t(sampled.reserved)
        << " tracked_live_bytes=" << sampled.live << " tracked_peak_bytes=" << sampled.peak
        << " tracked_created_bytes=" << sampled.created << " tracked_freed_bytes=" << sampled.freed
        << " tracked_created_count=" << sampled.created_count
        << " tracked_freed_count=" << sampled.freed_count << " vma_reconciliation_delta="
        << int64_t(total.statistics.allocationBytes) - int64_t(sampled.live) << '\n';
    out << "tracked_retiring_bytes=" << sampled.retiring
        << " tracked_retiring_count=" << sampled.retiring_count
        << " tracked_retiring_peak_bytes=" << sampled.retiring_peak
        << " tracked_nonretiring_bytes=" << sampled.live - sampled.retiring << '\n';
    out << "device_memory_created_bytes=" << sampled.device_created
        << " device_memory_freed_bytes=" << sampled.device_freed
        << " device_memory_create_count=" << sampled.device_create_count
        << " device_memory_free_count=" << sampled.device_free_count << '\n';
    for (uint32_t i = 0; i < properties.memoryHeapCount; ++i) {
        const auto& heap = statistics.memoryHeap[i];
        out << "vma_heap=" << i << " flags=" << properties.memoryHeaps[i].flags
            << " capacity_bytes=" << properties.memoryHeaps[i].size
            << " reserved_bytes=" << heap.statistics.blockBytes
            << " allocation_bytes=" << heap.statistics.allocationBytes
            << " driver_usage_bytes=" << budgets[i].usage
            << " driver_budget_bytes=" << budgets[i].budget
            << " budget_source=" << (driver_budget ? "VK_EXT_memory_budget" : "VMA_estimate")
            << '\n';
    }
    for (const auto& [name, group] : sampled.groups)
        out << "vma_group=" << name << " allocation_bytes=" << group.bytes
            << " buffer_requested_bytes=" << group.requested << " count=" << group.count
            << " peak_bytes=" << group.peak << " retiring_peak_bytes=" << group.retiring_peak
            << " retiring_bytes=" << group.retiring << " retiring_count=" << group.retiring_count
            << '\n';
    std::vector<const Allocation*> largest;
    largest.reserve(sampled.allocations.size());
    for (const auto& [allocation, item] : sampled.allocations)
        largest.push_back(&item);
    const auto count = std::min<size_t>(16, largest.size());
    std::partial_sort(largest.begin(), largest.begin() + count, largest.end(),
                      [](const Allocation* a, const Allocation* b) { return a->bytes > b->bytes; });
    for (size_t i = 0; i < count; ++i) {
        const auto& item = *largest[i];
        out << "vma_largest_rank=" << i + 1 << " category=" << item.category
            << " allocation_bytes=" << item.bytes << " buffer_requested_bytes=" << item.requested
            << " memory_type=" << item.type << " properties=" << item.properties
            << " retiring=" << item.retiring << '\n';
    }
#if defined(__linux__)
    std::ifstream rollup("/proc/self/smaps_rollup");
    std::string line;
    bool read = false;
    size_t lines = 0;
    while (++lines <= 128 && std::getline(rollup, line)) {
        std::istringstream fields(line);
        std::string key, unit;
        uint64_t value;
        if (!(fields >> key >> value >> unit) || unit != "kB")
            continue;
        if (key == "Rss:" || key == "Pss:" || key == "Private_Clean:" || key == "Private_Dirty:" ||
            key == "Shared_Clean:" || key == "Shared_Dirty:" || key == "SwapPss:") {
            key.pop_back();
            out << "process_" << key << "_bytes=" << value * 1024 << '\n';
            read = true;
        }
    }
    if (!read)
        out << "process_memory=unavailable\n";
#else
    out << "process_memory=unavailable\n";
#endif
    out << "memory_sample_elapsed_us="
        << std::chrono::duration_cast<std::chrono::microseconds>(std::chrono::steady_clock::now() -
                                                                 sample_begin)
               .count()
        << '\n';
    out << "memory_scope=VMA_allocations_only; process_RSS_PSS_include_guest_and_CPU_memory;"
           " driver_heap_usage_is_VMA_adjusted_estimate; "
           "non_VMA_ImGui_swapchain_driver_internal_allocations_excluded; UMA_flags_overlap; "
           "do_not_sum_VMA_and_PSS;"
           " image_requested_size_is_driver_defined; retiring_is_subset_of_allocation_bytes; "
           "group_peaks_are_not_additive; no_GPU_wait\n";
}
} // namespace VideoCore::VmaDiagnostics
