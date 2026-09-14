// SPDX-FileCopyrightText: Copyright 2026 shadPS4 Emulator Project
// SPDX-License-Identifier: MPL-2.0
// Source SDK adapter adapted from Cemu Android GpuReshapeAdapter (2026-09-14).
#pragma once

// GPU Reshape Android source-level SDK adapter for shadPS4's Vulkan backend.
//
// Adapted from Cemu (2026-09-14 source SDK integration), originally Citron's source-SDK
// integration. The current Android adapter supports Descriptor and ResourceBounds shader
// instrumentation, with optional ResourceBounds safe guarding, and streams findings to the Mac MCP
// host. Initialization tracking is not implemented by this adapter.
//
// The whole adapter is a no-op unless the build was configured with the GPU Reshape Android SDK
// (SHADPS4_HAS_GPU_RESHAPE_ANDROID) AND the runtime opt-in env/property is set, so ordinary builds
// and non-Android platforms are unaffected.

#include <cstdint>
#include <memory>
#include <span>
#include <string>

#include <vulkan/vulkan_core.h>

namespace GpuReshape {

class Adapter {
public:
    Adapter();
    ~Adapter();

    Adapter(const Adapter&) = delete;
    Adapter& operator=(const Adapter&) = delete;

    // Must run before vkCreateDevice: instrumented graphics stages write export buffers.
    void ConfigureDeviceFeatures(const VkPhysicalDeviceFeatures& supported,
                                 VkPhysicalDeviceFeatures& enabled,
                                 const VkPhysicalDeviceVulkan12Features& supportedIndexing,
                                 VkPhysicalDeviceVulkan12Features& enabledIndexing);

    // Called right after the Vulkan logical device is created. Creates the GPU Reshape SDK,
    // optionally attaches ResourceBounds instrumentation, and installs the hooked device proc
    // resolver. Safe to call when the SDK is not compiled/requested (becomes a no-op).
    void Initialize(VkInstance instance, VkPhysicalDevice physicalDevice, VkDevice device,
                    uint32_t apiVersion, PFN_vkGetInstanceProcAddr getInstanceProcAddr,
                    PFN_vkGetDeviceProcAddr getDeviceProcAddr);
    void Shutdown();
    void PublishStatus(bool force = false) const;
    // Set before Initialize. Included in retained, non-blocking status snapshots.
    void SetIdentity(uint64_t generation, std::string driver);

    // True when the SDK created and the hooked device proc resolver is available; shadPS4 should
    // resolve its device-level Vulkan function pointers through GetHookedDeviceProcAddr() so
    // command observation and instrumentation see them.
    [[nodiscard]] bool IsActive() const;
    [[nodiscard]] PFN_vkGetDeviceProcAddr GetHookedDeviceProcAddr() const;

    // The SDK hooks extend layouts and bind instrumentation automatically. Do not append or
    // bind the instrumentation set again in the renderer.
    [[nodiscard]] bool ResourceBoundsActive() const;
    [[nodiscard]] bool GetInstrumentationBindings(uint32_t& setIndex, VkDescriptorSetLayout& layout,
                                                  VkDescriptorSet& descriptorSet,
                                                  uint32_t& pushConstantOffset,
                                                  uint32_t& pushConstantSize) const;

    // Reports one descriptor set's live resource identities (buffer handle/offset/range) to the
    // ResourceBounds feature so a fault can be attributed to the exact logical resource view.
    void RegisterDescriptorResources(VkDescriptorSet descriptorSet,
                                     std::span<const VkWriteDescriptorSet> writes,
                                     uint32_t logicalSet = 0) const;

    // Human/agent-readable status line for the `gpu_reshape_status` debugbus command.
    [[nodiscard]] std::string StatusText() const;

private:
    struct Impl;
    std::unique_ptr<Impl> impl;
};
} // namespace GpuReshape
