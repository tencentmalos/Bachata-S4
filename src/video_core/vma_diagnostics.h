// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once

#include <iosfwd>
#include <string>
#include <vk_mem_alloc.h>
#include "video_core/host_memory_policy.h"

// One ledger per allocator, owned by the renderer library (not by each DSO
// including this header). Reserved bytes come from actual device-memory callbacks;
// allocation bytes come from VMA requirements, not logical image dimensions.
namespace VideoCore::VmaDiagnostics {
extern const VmaDeviceMemoryCallbacks memory_callbacks;
VkResult CreateAllocator(VmaAllocatorCreateInfo info, HostMemoryPolicy policy,
                         VmaAllocator* allocator);
VkResult CreateImage(VmaAllocator allocator, const VkImageCreateInfo* ci,
                     const VmaAllocationCreateInfo* ai, VkImage* image, VmaAllocation* allocation,
                     VmaAllocationInfo* info, const char* category = "image");
VkResult CreateBuffer(VmaAllocator allocator, const VkBufferCreateInfo* ci,
                      const VmaAllocationCreateInfo* ai, VkBuffer* buffer,
                      VmaAllocation* allocation, VmaAllocationInfo* info,
                      std::string category = "buffer");
void DestroyImage(VmaAllocator allocator, VkImage image, VmaAllocation allocation);
void DestroyBuffer(VmaAllocator allocator, VkBuffer buffer, VmaAllocation allocation);
void Tag(VmaAllocator allocator, VmaAllocation allocation, const char* category,
         bool retiring = false);
void End(VmaAllocator allocator);
void Append(VmaAllocator allocator, const VkPhysicalDeviceMemoryProperties& properties,
            bool driver_budget, std::ostream& out);
} // namespace VideoCore::VmaDiagnostics
