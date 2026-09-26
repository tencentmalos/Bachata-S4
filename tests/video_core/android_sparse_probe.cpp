// SPDX-License-Identifier: GPL-2.0-or-later
// Sparse residency support of the Android driver the app would use, as the upstream sparse-arena
// buffer cache needs it: sparseBinding + sparseResidencyBuffer, a queue with sparse binding, and
// 4 GiB (and merged 8 GiB) residency buffers whose blocks are bound and unbound on demand.
// Usage: android_sparse_probe system | <hook_dir> <driver_dir>
#include <cinttypes>
#include <cstdio>
#include <stdexcept>
#include <string>
#include <string_view>
#include <vector>

#include "video_core/renderer_vulkan/vk_driver.h"

namespace {
int failures = 0;

void Check(bool ok, const std::string& what) {
    std::printf("%s %s\n", ok ? "PASS" : "FAIL", what.c_str());
    failures += !ok;
}

template <typename T>
T Get(PFN_vkGetInstanceProcAddr gipa, VkInstance instance, const char* name) {
    auto function = reinterpret_cast<T>(gipa(instance, name));
    if (!function) {
        throw std::runtime_error(std::string("missing entry ") + name);
    }
    return function;
}

// Binds `blocks` memory blocks at spread offsets of a sparse residency buffer of `size` bytes,
// then unbinds them, each step fenced.
void TryArena(PFN_vkGetInstanceProcAddr gipa, VkInstance instance, VkPhysicalDevice physical,
              VkDevice device, VkQueue queue, VkDeviceSize size, uint32_t blocks) {
    auto gdpa = Get<PFN_vkGetDeviceProcAddr>(gipa, instance, "vkGetDeviceProcAddr");
    const auto dev = [&](const char* name) {
        auto function = gdpa(device, name);
        if (!function) {
            throw std::runtime_error(std::string("missing device entry ") + name);
        }
        return function;
    };
    auto create_buffer = reinterpret_cast<PFN_vkCreateBuffer>(dev("vkCreateBuffer"));
    auto destroy_buffer = reinterpret_cast<PFN_vkDestroyBuffer>(dev("vkDestroyBuffer"));
    auto requirements =
        reinterpret_cast<PFN_vkGetBufferMemoryRequirements>(dev("vkGetBufferMemoryRequirements"));
    auto allocate = reinterpret_cast<PFN_vkAllocateMemory>(dev("vkAllocateMemory"));
    auto free_memory = reinterpret_cast<PFN_vkFreeMemory>(dev("vkFreeMemory"));
    auto bind_sparse = reinterpret_cast<PFN_vkQueueBindSparse>(dev("vkQueueBindSparse"));
    auto create_fence = reinterpret_cast<PFN_vkCreateFence>(dev("vkCreateFence"));
    auto destroy_fence = reinterpret_cast<PFN_vkDestroyFence>(dev("vkDestroyFence"));
    auto wait_fences = reinterpret_cast<PFN_vkWaitForFences>(dev("vkWaitForFences"));
    auto reset_fences = reinterpret_cast<PFN_vkResetFences>(dev("vkResetFences"));
    auto memory_properties = Get<PFN_vkGetPhysicalDeviceMemoryProperties>(
        gipa, instance, "vkGetPhysicalDeviceMemoryProperties");

    const auto label = std::to_string(size >> 20) + " MiB arena";
    const VkBufferCreateInfo info{
        VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO,
        nullptr,
        VK_BUFFER_CREATE_SPARSE_BINDING_BIT | VK_BUFFER_CREATE_SPARSE_RESIDENCY_BIT,
        size,
        VK_BUFFER_USAGE_STORAGE_BUFFER_BIT | VK_BUFFER_USAGE_TRANSFER_SRC_BIT |
            VK_BUFFER_USAGE_TRANSFER_DST_BIT | VK_BUFFER_USAGE_VERTEX_BUFFER_BIT |
            VK_BUFFER_USAGE_INDEX_BUFFER_BIT | VK_BUFFER_USAGE_UNIFORM_BUFFER_BIT |
            VK_BUFFER_USAGE_SHADER_DEVICE_ADDRESS_BIT,
        VK_SHARING_MODE_EXCLUSIVE};
    VkBuffer buffer{};
    const auto created = create_buffer(device, &info, nullptr, &buffer);
    Check(created == VK_SUCCESS, label + " create (" + std::to_string(created) + ")");
    if (created != VK_SUCCESS) {
        return;
    }
    VkMemoryRequirements req{};
    requirements(device, buffer, &req);
    std::printf("  %s: block alignment %" PRIu64 " bytes, memory types 0x%x\n", label.c_str(),
                req.alignment, req.memoryTypeBits);

    VkPhysicalDeviceMemoryProperties mem{};
    memory_properties(physical, &mem);
    uint32_t type = UINT32_MAX;
    for (uint32_t i = 0; i < mem.memoryTypeCount; ++i) {
        if ((req.memoryTypeBits & (1u << i)) &&
            (mem.memoryTypes[i].propertyFlags & VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT)) {
            type = i;
            break;
        }
    }
    Check(type != UINT32_MAX, label + " device-local memory type");
    if (type == UINT32_MAX) {
        destroy_buffer(device, buffer, nullptr);
        return;
    }

    std::vector<VkDeviceMemory> memory(blocks);
    std::vector<VkSparseMemoryBind> binds(blocks);
    bool allocated = true;
    for (uint32_t i = 0; i < blocks; ++i) {
        const VkMemoryAllocateInfo alloc{VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO, nullptr,
                                         req.alignment, type};
        allocated &= allocate(device, &alloc, nullptr, &memory[i]) == VK_SUCCESS;
        // First, last and spread blocks, as a real arena would touch them.
        const VkDeviceSize slots = size / req.alignment;
        const VkDeviceSize slot = blocks == 1 ? 0 : (slots - 1) * i / (blocks - 1);
        binds[i] = {slot * req.alignment, req.alignment, memory[i], 0, 0};
    }
    Check(allocated, label + " allocate " + std::to_string(blocks) + " blocks");

    VkFence fence{};
    const VkFenceCreateInfo fence_info{VK_STRUCTURE_TYPE_FENCE_CREATE_INFO};
    create_fence(device, &fence_info, nullptr, &fence);
    const auto submit = [&](const std::vector<VkSparseMemoryBind>& list, const char* what) {
        const VkSparseBufferMemoryBindInfo buffer_bind{buffer, static_cast<uint32_t>(list.size()),
                                                       list.data()};
        VkBindSparseInfo bind{VK_STRUCTURE_TYPE_BIND_SPARSE_INFO};
        bind.bufferBindCount = 1;
        bind.pBufferBinds = &buffer_bind;
        const auto result = bind_sparse(queue, 1, &bind, fence);
        const auto waited =
            result == VK_SUCCESS ? wait_fences(device, 1, &fence, VK_TRUE, 5'000'000'000ull)
                                 : result;
        reset_fences(device, 1, &fence);
        Check(result == VK_SUCCESS && waited == VK_SUCCESS,
              label + " " + what + " (bind " + std::to_string(result) + ", wait " +
                  std::to_string(waited) + ")");
    };
    if (allocated) {
        submit(binds, "bind");
        auto unbinds = binds;
        for (auto& b : unbinds) {
            b.memory = VK_NULL_HANDLE;
        }
        submit(unbinds, "unbind");
    }
    destroy_fence(device, fence, nullptr);
    for (auto m : memory) {
        if (m) {
            free_memory(device, m, nullptr);
        }
    }
    destroy_buffer(device, buffer, nullptr);
}
} // namespace

int main(int argc, char** argv) {
    if (argc < 2) {
        std::fprintf(stderr, "usage: %s system | <hook_dir> <driver_dir>\n", argv[0]);
        return 2;
    }
    try {
        const auto driver = std::string_view(argv[1]) == "system"
                                ? Vulkan::LoadAndroidSystemDriver()
                                : Vulkan::LoadAndroidTurnip(argv[1], argc > 2 ? argv[2] : argv[1]);
        std::printf("driver: %s\n", driver->identity.c_str());
        const auto gipa = driver->entry;
        const VkApplicationInfo app{VK_STRUCTURE_TYPE_APPLICATION_INFO, nullptr, "sparse-probe",
                                    1, "shadPS4", 1, VK_API_VERSION_1_3};
        const VkInstanceCreateInfo instance_info{VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO, nullptr,
                                                 0, &app};
        VkInstance instance{};
        if (Get<PFN_vkCreateInstance>(gipa, nullptr, "vkCreateInstance")(
                &instance_info, nullptr, &instance) != VK_SUCCESS) {
            throw std::runtime_error("vkCreateInstance failed");
        }
        uint32_t count{};
        auto enumerate =
            Get<PFN_vkEnumeratePhysicalDevices>(gipa, instance, "vkEnumeratePhysicalDevices");
        enumerate(instance, &count, nullptr);
        std::vector<VkPhysicalDevice> physicals(count);
        enumerate(instance, &count, physicals.data());
        auto properties2 =
            Get<PFN_vkGetPhysicalDeviceProperties2>(gipa, instance, "vkGetPhysicalDeviceProperties2");
        auto features =
            Get<PFN_vkGetPhysicalDeviceFeatures>(gipa, instance, "vkGetPhysicalDeviceFeatures");
        auto queue_families = Get<PFN_vkGetPhysicalDeviceQueueFamilyProperties>(
            gipa, instance, "vkGetPhysicalDeviceQueueFamilyProperties");
        auto create_device = Get<PFN_vkCreateDevice>(gipa, instance, "vkCreateDevice");
        auto destroy_device = Get<PFN_vkDestroyDevice>(gipa, instance, "vkDestroyDevice");
        auto get_queue = Get<PFN_vkGetDeviceQueue>(gipa, instance, "vkGetDeviceQueue");

        for (auto physical : physicals) {
            VkPhysicalDeviceMaintenance4Properties m4{
                VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_MAINTENANCE_4_PROPERTIES};
            VkPhysicalDeviceProperties2 props{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_PROPERTIES_2, &m4};
            properties2(physical, &props);
            VkPhysicalDeviceFeatures caps{};
            features(physical, &caps);
            const auto& sparse = props.properties.sparseProperties;
            std::printf("device: %s\n", props.properties.deviceName);
            std::printf("  sparseAddressSpaceSize=%" PRIu64 " maxBufferSize=%" PRIu64 "\n",
                        props.properties.limits.sparseAddressSpaceSize, m4.maxBufferSize);
            std::printf("  residencyStandard2DBlockShape=%u residencyNonResidentStrict=%u "
                        "residencyAlignedMipSize=%u\n",
                        sparse.residencyStandard2DBlockShape, sparse.residencyNonResidentStrict,
                        sparse.residencyAlignedMipSize);
            Check(caps.sparseBinding, "sparseBinding");
            Check(caps.sparseResidencyBuffer, "sparseResidencyBuffer");
            std::printf("  sparseResidencyAliased=%u sparseResidencyImage2D=%u\n",
                        caps.sparseResidencyAliased, caps.sparseResidencyImage2D);

            uint32_t families{};
            queue_families(physical, &families, nullptr);
            std::vector<VkQueueFamilyProperties> family_props(families);
            queue_families(physical, &families, family_props.data());
            uint32_t sparse_family = UINT32_MAX;
            for (uint32_t i = 0; i < families; ++i) {
                std::printf("  queue family %u flags=0x%x count=%u\n", i,
                            family_props[i].queueFlags, family_props[i].queueCount);
                if (sparse_family == UINT32_MAX &&
                    (family_props[i].queueFlags & VK_QUEUE_SPARSE_BINDING_BIT) &&
                    (family_props[i].queueFlags & VK_QUEUE_GRAPHICS_BIT)) {
                    sparse_family = i;
                }
            }
            Check(sparse_family != UINT32_MAX, "graphics queue with sparse binding");
            if (!caps.sparseBinding || !caps.sparseResidencyBuffer ||
                sparse_family == UINT32_MAX) {
                continue;
            }

            const float priority = 1.0f;
            const VkDeviceQueueCreateInfo queue_info{VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO,
                                                     nullptr, 0, sparse_family, 1, &priority};
            VkPhysicalDeviceFeatures enabled{};
            enabled.sparseBinding = VK_TRUE;
            enabled.sparseResidencyBuffer = VK_TRUE;
            VkPhysicalDeviceVulkan12Features v12{
                VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_2_FEATURES};
            v12.bufferDeviceAddress = VK_TRUE;
            const VkDeviceCreateInfo device_info{VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO,
                                                 &v12,
                                                 0,
                                                 1,
                                                 &queue_info,
                                                 0,
                                                 nullptr,
                                                 0,
                                                 nullptr,
                                                 &enabled};
            VkDevice device{};
            const auto created = create_device(physical, &device_info, nullptr, &device);
            Check(created == VK_SUCCESS, "create device with sparse features (" +
                                             std::to_string(created) + ")");
            if (created != VK_SUCCESS) {
                continue;
            }
            VkQueue queue{};
            get_queue(device, sparse_family, 0, &queue);
            TryArena(gipa, instance, physical, device, queue, VkDeviceSize{4} << 30, 8);
            TryArena(gipa, instance, physical, device, queue, VkDeviceSize{8} << 30, 8);
            destroy_device(device, nullptr);
        }
        Get<PFN_vkDestroyInstance>(gipa, instance, "vkDestroyInstance")(instance, nullptr);
    } catch (const std::exception& e) {
        std::printf("FAIL exception: %s\n", e.what());
        return 1;
    }
    std::printf("failures=%d\n", failures);
    return failures ? 1 : 0;
}
