// SPDX-FileCopyrightText: Copyright 2024 shadPS4 Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#if defined(__linux__)
#include <sys/mman.h>
#endif
#if defined(__ANDROID__)
#include <sys/system_properties.h>
#endif
#include <string_view>
#include <array>
#include <numeric>

#include "common/alignment.h"
#include "common/assert.h"
#include "video_core/buffer_cache/buffer.h"
#include "video_core/renderer_vulkan/vk_instance.h"
#include "video_core/renderer_vulkan/vk_platform.h"
#include "video_core/renderer_vulkan/vk_scheduler.h"

#include <vk_mem_alloc.h>
#include "video_core/vma_diagnostics.h"

namespace VideoCore {

std::string_view BufferTypeName(MemoryType type) {
    switch (type) {
    case MemoryType::HostUncached:
        return "HostUncached";
    case MemoryType::HostCached:
        return "HostCached";
    case MemoryType::Stream:
        return "Stream";
    case MemoryType::DeviceLocal:
        return "DeviceLocal";
    case MemoryType::Sparse:
        return "Sparse";
    default:
        return "Invalid";
    }
}

[[nodiscard]] VkMemoryPropertyFlags MemoryUsagePreferredVmaFlags(MemoryType type) {
    return type != MemoryType::DeviceLocal ? VK_MEMORY_PROPERTY_HOST_COHERENT_BIT
                                           : VkMemoryPropertyFlagBits{};
}

[[nodiscard]] VmaAllocationCreateFlags MemoryUsageVmaFlags(MemoryType type) {
    switch (type) {
    case MemoryType::HostUncached:
    case MemoryType::Stream:
        return VMA_ALLOCATION_CREATE_MAPPED_BIT |
               VMA_ALLOCATION_CREATE_HOST_ACCESS_SEQUENTIAL_WRITE_BIT;
    case MemoryType::HostCached:
        return VMA_ALLOCATION_CREATE_MAPPED_BIT | VMA_ALLOCATION_CREATE_HOST_ACCESS_RANDOM_BIT;
    case MemoryType::DeviceLocal:
    default:
        return {};
    }
}

[[nodiscard]] VmaMemoryUsage MemoryUsageVma(MemoryType type) {
    switch (type) {
    case MemoryType::DeviceLocal:
    case MemoryType::Stream:
        return VMA_MEMORY_USAGE_AUTO_PREFER_DEVICE;
    case MemoryType::HostUncached:
    case MemoryType::HostCached:
        return VMA_MEMORY_USAGE_AUTO_PREFER_HOST;
    default:
        return VMA_MEMORY_USAGE_UNKNOWN;
    }
}

UniqueBuffer::UniqueBuffer(vk::Device device_, VmaAllocator allocator_)
    : device{device_}, allocator{allocator_} {}

UniqueBuffer::~UniqueBuffer() {
    Destroy();
}

void UniqueBuffer::Destroy() {
    if (allocation) {
        VideoCore::VmaDiagnostics::DestroyBuffer(allocator, buffer, allocation);
    } else if (buffer) {
        device.destroyBuffer(buffer);
    }
    buffer = VK_NULL_HANDLE;
    allocation = VK_NULL_HANDLE;
    bda_addr = 0;
}

void UniqueBuffer::Create(vk::BufferCreateInfo& buffer_ci, MemoryType mem_type,
                          VmaAllocationInfo* out_alloc_info) {
    const bool with_bda = bool(buffer_ci.usage & vk::BufferUsageFlagBits::eShaderDeviceAddress);
    if (mem_type != MemoryType::Sparse) {
        const VmaAllocationCreateFlags bda_flag =
            with_bda ? VMA_ALLOCATION_CREATE_DEDICATED_MEMORY_BIT : 0;
        const VmaAllocationCreateInfo alloc_ci = {
            .flags =
                VMA_ALLOCATION_CREATE_WITHIN_BUDGET_BIT | bda_flag | MemoryUsageVmaFlags(mem_type),
            .usage = MemoryUsageVma(mem_type),
            .requiredFlags = 0,
            .preferredFlags = MemoryUsagePreferredVmaFlags(mem_type),
            .pool = VK_NULL_HANDLE,
            .pUserData = nullptr,
        };

        const VkBufferCreateInfo buffer_ci_unsafe = static_cast<VkBufferCreateInfo>(buffer_ci);
        VkBuffer unsafe_buffer{};
        VkResult result = VideoCore::VmaDiagnostics::CreateBuffer(
            allocator, &buffer_ci_unsafe, &alloc_ci, &unsafe_buffer, &allocation, out_alloc_info,
            "buffer/" + std::string(BufferTypeName(mem_type)));
        if (result != VK_SUCCESS) {
            // Distinguish VMA's own budget refusal (WITHIN_BUDGET) from a driver failure.
            std::array<VmaBudget, VK_MAX_MEMORY_HEAPS> budgets{};
            vmaGetHeapBudgets(allocator, budgets.data());
            const VkPhysicalDeviceMemoryProperties* props{};
            vmaGetMemoryProperties(allocator, &props);
            for (u32 i = 0; props && i < props->memoryHeapCount; ++i)
                LOG_ERROR(Render_Vulkan,
                          "heap {}: size={} MiB usage={} MiB budget={} MiB blocks={} MiB", i,
                          props->memoryHeaps[i].size >> 20, budgets[i].usage >> 20,
                          budgets[i].budget >> 20, budgets[i].statistics.blockBytes >> 20);
        }
        ASSERT_MSG(result == VK_SUCCESS,
                   "Failed allocating buffer with error {} (size={:#x} type={} bda={})",
                   vk::to_string(vk::Result{result}), buffer_ci.size, BufferTypeName(mem_type),
                   with_bda);
        buffer = vk::Buffer{unsafe_buffer};
    } else {
        buffer_ci.flags |=
            vk::BufferCreateFlagBits::eSparseBinding | vk::BufferCreateFlagBits::eSparseResidency;
        buffer = Vulkan::Check(device.createBuffer(buffer_ci));
    }

    if (with_bda) {
        const vk::BufferDeviceAddressInfo bda_info = {
            .buffer = buffer,
        };
        auto bda_result = device.getBufferAddress(bda_info);
        ASSERT_MSG(bda_result != 0, "Failed to get buffer device address");
        bda_addr = bda_result;
    }
}

Buffer::Buffer(const Vulkan::Instance& instance, VAddr cpu_addr_, u64 size_bytes_,
               MemoryType mem_type_, std::string_view debug_name,
               std::span<const u32> concurrent_families)
    : cpu_addr{cpu_addr_}, size_bytes{size_bytes_}, mem_type{mem_type_},
      buffer{instance.GetDevice(), instance.GetAllocator()} {

    const bool concurrent = concurrent_families.size() > 1;
    vk::BufferCreateInfo buffer_ci = {
        .size = size_bytes,
        .usage = AllFlags,
        .sharingMode = concurrent ? vk::SharingMode::eConcurrent : vk::SharingMode::eExclusive,
        .queueFamilyIndexCount = concurrent ? static_cast<u32>(concurrent_families.size()) : 0u,
        .pQueueFamilyIndices = concurrent ? concurrent_families.data() : nullptr,
    };
    VmaAllocationInfo alloc_info{};
    buffer.Create(buffer_ci, mem_type, &alloc_info);
    if (cpu_addr && buffer.allocation) {
        VmaDiagnostics::Tag(instance.GetAllocator(), buffer.allocation, "buffer/guest-cache");
    }

    const auto device = instance.GetDevice();
    if (!debug_name.empty()) {
        Vulkan::SetObjectName(device, Handle(), debug_name);
    } else {
        Vulkan::SetObjectName(device, Handle(), "Buffer {:#x}:{:#x}", cpu_addr, size_bytes);
    }

    if (mem_type != MemoryType::Sparse) {
        VkMemoryPropertyFlags property_flags{};
        vmaGetAllocationMemoryProperties(instance.GetAllocator(), buffer.allocation,
                                         &property_flags);
        if (alloc_info.pMappedData) {
            mapped_data = std::span<u8>{std::bit_cast<u8*>(alloc_info.pMappedData), size_bytes};
        }
        is_coherent = property_flags & VK_MEMORY_PROPERTY_HOST_COHERENT_BIT;
    }
}

void Buffer::Flush(u64 offset, u64 size) {
    if (mapped_data.empty() || is_coherent) {
        return;
    }
    vmaFlushAllocation(buffer.allocator, buffer.allocation, offset, size);
}

void Buffer::Invalidate(u64 offset, u64 size) {
    if (mapped_data.empty() || is_coherent) {
        return;
    }
    vmaInvalidateAllocation(buffer.allocator, buffer.allocation, offset, size);
}

namespace {
/// Maps every page of a fresh host-visible ring up front. The driver maps buffer memory into the
/// process lazily, so otherwise each page faults on its first write: for the stream buffer and
/// the staging rings that is a fault per 4 KiB of streamed data until the ring has wrapped once
/// (Bloodborne on the AYN Thor: about 3300 faults per frame on the command processor).
void PrefaultMapping(std::span<u8> data) {
#if defined(__linux__)
#if defined(__ANDROID__)
    // Diagnostic switch for A/B (debug.shadps4.stream_prefault=0 turns it off).
    static const bool enabled = [] {
        char value[PROP_VALUE_MAX]{};
        return __system_property_get("debug.shadps4.stream_prefault", value) <= 0 ||
               std::string_view{value} != "0";
    }();
    if (!enabled) {
        return;
    }
#endif
    if (data.empty()) {
        return;
    }
    constexpr int PopulateWrite = 23; // MADV_POPULATE_WRITE, Linux 5.14
    constexpr uintptr_t Page = 4096;
    const uintptr_t begin = Common::AlignUp(reinterpret_cast<uintptr_t>(data.data()), Page);
    const uintptr_t end = Common::AlignDown(reinterpret_cast<uintptr_t>(data.data() + data.size()),
                                            Page);
    if (end > begin && ::madvise(reinterpret_cast<void*>(begin), end - begin, PopulateWrite) == 0) {
        return;
    }
    for (size_t offset = 0; offset < data.size(); offset += Page) {
        reinterpret_cast<volatile u8*>(data.data())[offset] = 0;
    }
#endif
}
} // namespace

constexpr u64 WATCHES_INITIAL_RESERVE = 0x100;
constexpr u64 WATCHES_RESERVE_CHUNK = 0x100;

StreamBuffer::StreamBuffer(const Vulkan::Instance& instance, Vulkan::Scheduler& scheduler_,
                           MemoryType mem_type, u64 size_bytes)
    : Buffer{instance, 0, size_bytes, mem_type}, scheduler{scheduler_},
      non_coherent_atom_size{instance.NonCoherentAtomSize()} {
    ReserveWatches(current_watches, WATCHES_INITIAL_RESERVE);
    ReserveWatches(previous_watches, WATCHES_INITIAL_RESERVE);
    PrefaultMapping(mapped_data);
    Vulkan::SetObjectName(instance.GetDevice(), Handle(), "StreamBuffer({}):{:#x}",
                          BufferTypeName(mem_type), size_bytes);
}

StreamBuffer::StreamBuffer(const Vulkan::Instance& instance, Vulkan::Scheduler& scheduler_,
                           u64 size_bytes, std::span<const u32> families, Staged)
    : Buffer{instance, 0, size_bytes, MemoryType::DeviceLocal, "StreamBuffer(staged)", families},
      staging{std::make_unique<Buffer>(instance, 0, size_bytes, MemoryType::HostUncached,
                                       "StreamBuffer(staged host copy)")},
      scheduler{scheduler_}, non_coherent_atom_size{instance.NonCoherentAtomSize()} {
    ASSERT_MSG(!staging->mapped_data.empty(), "Staged stream host copy is not mapped");
    ReserveWatches(current_watches, WATCHES_INITIAL_RESERVE);
    ReserveWatches(previous_watches, WATCHES_INITIAL_RESERVE);
}

bool StreamBuffer::PrepareMap(u64 size, u64 alignment, bool allow_wait) {
    const Buffer& target = WriteTarget();
    if (!target.mapped_data.empty() && !target.is_coherent) {
        size = Common::AlignUp(size, non_coherent_atom_size);
        alignment =
            alignment > 0 ? std::lcm(alignment, non_coherent_atom_size) : non_coherent_atom_size;
    }

    if (size > this->size_bytes) {
        return false;
    }

    if (alignment > 0) {
        offset = Common::AlignUp(offset, alignment);
    }

    if (offset + size > this->size_bytes) {
        // The buffer would overflow, save the amount of used watches and reset the state.
        invalidation_mark = current_watch_cursor;
        current_watch_cursor = 0;
        offset = 0;

        // Swap watches and reset waiting cursors.
        std::swap(previous_watches, current_watches);
        wait_cursor = 0;
        wait_bound = 0;
    }

    if (!WaitPendingOperations(offset + size, allow_wait)) {
        return false;
    }

    mapped_size = size;
    return true;
}

std::pair<u8*, u64> StreamBuffer::Map(u64 size, u64 alignment, bool allow_wait) {
    if (!PrepareMap(size, alignment, allow_wait)) {
        return {nullptr, 0};
    }
    const auto written = WriteTarget().mapped_data;
    u8* const data = written.empty() ? nullptr : written.data() + offset;
    return {data, offset};
}

void StreamBuffer::Commit(u64 used_size) {
    const Buffer& target = WriteTarget();
    if (!target.mapped_data.empty() && !target.is_coherent) {
        used_size = Common::AlignUp(used_size, non_coherent_atom_size);
    }
    ASSERT(used_size <= mapped_size);
    mapped_size = used_size;
    if (mapped_size != 0) {
        Commit();
    }
}

void StreamBuffer::Commit() {
    if (staging) {
        staging->Flush(offset, mapped_size);
        // The ring only moves forward between submissions, wrapping at most once (a wrap
        // into data of the current submission waits for it, which submits it first). One
        // span per lap covers every commit; the alignment padding it also copies is unused.
        if (!staged_ranges.empty() && offset >= staged_ranges.back().srcOffset) {
            staged_ranges.back().size = offset + mapped_size - staged_ranges.back().srcOffset;
        } else if (mapped_size != 0) {
            staged_ranges.push_back({offset, offset, mapped_size});
        }
    } else if (mem_type == MemoryType::HostCached) {
        Invalidate(offset, mapped_size);
    } else {
        Flush(offset, mapped_size);
    }
    AdvanceAndWatch();
}

std::optional<u64> StreamBuffer::Reserve(u64 size, u64 alignment, bool allow_wait) {
    if (!PrepareMap(size, alignment, allow_wait)) {
        return std::nullopt;
    }
    const u64 reserved_offset = offset;
    AdvanceAndWatch();
    return reserved_offset;
}

void StreamBuffer::AdvanceAndWatch() {
    offset += mapped_size;
    const u64 tick = scheduler.CurrentTick();
    last_tick = tick;

    // Extend the last watch if it belongs to the same tick.
    if (current_watch_cursor != 0 && current_watches[current_watch_cursor - 1].tick == tick) {
        current_watches[current_watch_cursor - 1].upper_bound = offset;
        return;
    }

    if (current_watch_cursor + 1 >= current_watches.size()) {
        // Ensure that there are enough watches.
        ReserveWatches(current_watches, WATCHES_RESERVE_CHUNK);
    }

    auto& watch = current_watches[current_watch_cursor++];
    watch.upper_bound = offset;
    watch.tick = tick;
}

void StreamBuffer::ReserveWatches(std::vector<Watch>& watches, std::size_t grow_size) {
    watches.resize(watches.size() + grow_size);
}

bool StreamBuffer::WaitPendingOperations(u64 requested_upper_bound, bool allow_wait) {
    if (!invalidation_mark) {
        return true;
    }
    while (requested_upper_bound > wait_bound && wait_cursor < *invalidation_mark) {
        auto& watch = previous_watches[wait_cursor];
        if (!scheduler.IsFree(watch.tick) && !allow_wait) {
            return false;
        }
        scheduler.Wait(watch.tick);
        wait_bound = watch.upper_bound;
        ++wait_cursor;
    }
    return true;
}

} // namespace VideoCore
