// SPDX-License-Identifier: GPL-2.0-or-later
// Sparse residency support of the Android driver the app would use, as the upstream sparse-arena
// buffer cache needs it: sparseBinding + sparseResidencyBuffer, a queue with sparse binding, and
// 4 GiB (and merged 8 GiB) residency buffers whose blocks are bound and unbound on demand.
// Usage: android_sparse_probe system | <hook_dir> <driver_dir>
#include <cinttypes>
#include <cstdio>
#include <cstdlib>
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
// copies a pattern into every bound block and back through the GPU, then unbinds them, each step
// fenced.
void TryArena(PFN_vkGetInstanceProcAddr gipa, VkInstance instance, VkPhysicalDevice physical,
              VkDevice device, VkQueue queue, uint32_t queue_family, VkDeviceSize size,
              uint32_t blocks) {
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
    auto bind_buffer_memory = reinterpret_cast<PFN_vkBindBufferMemory>(dev("vkBindBufferMemory"));
    auto map_memory = reinterpret_cast<PFN_vkMapMemory>(dev("vkMapMemory"));
    auto create_pool = reinterpret_cast<PFN_vkCreateCommandPool>(dev("vkCreateCommandPool"));
    auto destroy_pool = reinterpret_cast<PFN_vkDestroyCommandPool>(dev("vkDestroyCommandPool"));
    auto allocate_cmd =
        reinterpret_cast<PFN_vkAllocateCommandBuffers>(dev("vkAllocateCommandBuffers"));
    auto begin_cmd = reinterpret_cast<PFN_vkBeginCommandBuffer>(dev("vkBeginCommandBuffer"));
    auto end_cmd = reinterpret_cast<PFN_vkEndCommandBuffer>(dev("vkEndCommandBuffer"));
    auto cmd_copy = reinterpret_cast<PFN_vkCmdCopyBuffer>(dev("vkCmdCopyBuffer"));
    auto cmd_barrier = reinterpret_cast<PFN_vkCmdPipelineBarrier>(dev("vkCmdPipelineBarrier"));
    auto queue_submit = reinterpret_cast<PFN_vkQueueSubmit>(dev("vkQueueSubmit"));

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
    // Host buffer -> each bound block (at its start and end) -> host buffer, compared.
    const auto RoundTrip = [&](VkBuffer target, const std::vector<VkSparseMemoryBind>& list,
                               VkDeviceSize block, const std::string& what) {
        constexpr VkDeviceSize Chunk = 256;
        const VkDeviceSize bytes = Chunk * 2 * list.size();
        VkBuffer host[2]{};
        VkDeviceMemory host_memory[2]{};
        uint8_t* mapped[2]{};
        for (int i = 0; i < 2; ++i) {
            const VkBufferCreateInfo info{VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO, nullptr, 0, bytes,
                                          VK_BUFFER_USAGE_TRANSFER_SRC_BIT |
                                              VK_BUFFER_USAGE_TRANSFER_DST_BIT,
                                          VK_SHARING_MODE_EXCLUSIVE};
            bool ok = create_buffer(device, &info, nullptr, &host[i]) == VK_SUCCESS;
            VkMemoryRequirements host_req{};
            if (ok) {
                requirements(device, host[i], &host_req);
            }
            uint32_t host_type = UINT32_MAX;
            for (uint32_t type = 0; ok && type < mem.memoryTypeCount; ++type) {
                constexpr VkMemoryPropertyFlags want =
                    VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT;
                if ((host_req.memoryTypeBits & (1u << type)) &&
                    (mem.memoryTypes[type].propertyFlags & want) == want) {
                    host_type = type;
                    break;
                }
            }
            const VkMemoryAllocateInfo alloc{VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO, nullptr,
                                             host_req.size, host_type};
            ok = ok && host_type != UINT32_MAX &&
                 allocate(device, &alloc, nullptr, &host_memory[i]) == VK_SUCCESS &&
                 bind_buffer_memory(device, host[i], host_memory[i], 0) == VK_SUCCESS &&
                 map_memory(device, host_memory[i], 0, bytes, 0,
                            reinterpret_cast<void**>(&mapped[i])) == VK_SUCCESS &&
                 mapped[i] != nullptr;
            if (!ok) {
                Check(false, label + " host staging buffer for the round trip");
                for (int j = 0; j <= i; ++j) {
                    if (host[j]) {
                        destroy_buffer(device, host[j], nullptr);
                    }
                    if (host_memory[j]) {
                        free_memory(device, host_memory[j], nullptr);
                    }
                }
                return;
            }
        }
        for (VkDeviceSize i = 0; i < bytes; ++i) {
            mapped[0][i] = static_cast<uint8_t>(i * 7 + 3);
            mapped[1][i] = 0;
        }
        std::vector<VkBufferCopy> in, out;
        for (size_t b = 0; b < list.size(); ++b) {
            for (VkDeviceSize end = 0; end < 2; ++end) {
                const VkDeviceSize host_offset = (b * 2 + end) * Chunk;
                const VkDeviceSize sparse_offset =
                    list[b].resourceOffset + (end ? block - Chunk : 0);
                in.push_back({host_offset, sparse_offset, Chunk});
                out.push_back({sparse_offset, host_offset, Chunk});
            }
        }
        const VkCommandPoolCreateInfo pool_info{VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO,
                                                nullptr, 0, queue_family};
        VkCommandPool pool{};
        create_pool(device, &pool_info, nullptr, &pool);
        const VkCommandBufferAllocateInfo cmd_info{VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO,
                                                   nullptr, pool, VK_COMMAND_BUFFER_LEVEL_PRIMARY,
                                                   1};
        VkCommandBuffer cmd{};
        if (!pool || allocate_cmd(device, &cmd_info, &cmd) != VK_SUCCESS) {
            Check(false, label + " command buffer for the round trip");
            return;
        }
        const VkCommandBufferBeginInfo begin{VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO};
        begin_cmd(cmd, &begin);
        cmd_copy(cmd, host[0], target, static_cast<uint32_t>(in.size()), in.data());
        const VkMemoryBarrier barrier{VK_STRUCTURE_TYPE_MEMORY_BARRIER, nullptr,
                                      VK_ACCESS_TRANSFER_WRITE_BIT,
                                      VK_ACCESS_TRANSFER_READ_BIT | VK_ACCESS_HOST_READ_BIT};
        cmd_barrier(cmd, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT, 0, 1,
                    &barrier, 0, nullptr, 0, nullptr);
        cmd_copy(cmd, target, host[1], static_cast<uint32_t>(out.size()), out.data());
        cmd_barrier(cmd, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_HOST_BIT, 0, 1,
                    &barrier, 0, nullptr, 0, nullptr);
        end_cmd(cmd);
        const VkSubmitInfo submit_info{VK_STRUCTURE_TYPE_SUBMIT_INFO, nullptr, 0, nullptr,
                                       nullptr, 1, &cmd};
        const auto result = queue_submit(queue, 1, &submit_info, fence);
        const auto waited =
            result == VK_SUCCESS ? wait_fences(device, 1, &fence, VK_TRUE, 5'000'000'000ull)
                                 : result;
        reset_fences(device, 1, &fence);
        size_t mismatches = 0;
        for (VkDeviceSize i = 0; i < bytes; ++i) {
            mismatches += mapped[0][i] != mapped[1][i];
        }
        Check(result == VK_SUCCESS && waited == VK_SUCCESS && mismatches == 0,
              label + " GPU copy round trip " + what + " (" + std::to_string(mismatches) +
                  " of " + std::to_string(bytes) + " bytes differ)");
        destroy_pool(device, pool, nullptr);
        for (int i = 0; i < 2; ++i) {
            destroy_buffer(device, host[i], nullptr);
            free_memory(device, host_memory[i], nullptr);
        }
    };
    if (allocated) {
        submit(binds, "bind");
        RoundTrip(buffer, binds, req.alignment, "through bound blocks");
        // Control: the same copies through an ordinary buffer bound to one allocation.
        VkBufferCreateInfo plain_info{VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO};
        plain_info.size = req.alignment * blocks;
        plain_info.usage = VK_BUFFER_USAGE_TRANSFER_SRC_BIT | VK_BUFFER_USAGE_TRANSFER_DST_BIT;
        VkBuffer plain{};
        create_buffer(device, &plain_info, nullptr, &plain);
        VkMemoryRequirements plain_req{};
        requirements(device, plain, &plain_req);
        const VkMemoryAllocateInfo plain_alloc{VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO, nullptr,
                                               plain_req.size, type};
        VkDeviceMemory plain_memory{};
        allocate(device, &plain_alloc, nullptr, &plain_memory);
        bind_buffer_memory(device, plain, plain_memory, 0);
        std::vector<VkSparseMemoryBind> plain_blocks(blocks);
        for (uint32_t i = 0; i < blocks; ++i) {
            plain_blocks[i].resourceOffset = i * req.alignment;
        }
        RoundTrip(plain, plain_blocks, req.alignment, "through an ordinary buffer (control)");
        destroy_buffer(device, plain, nullptr);
        free_memory(device, plain_memory, nullptr);
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
    std::setvbuf(stdout, nullptr, _IONBF, 0);
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
            // The buffer cache's sizes: pages are the largest power of two at most half of
            // maxBufferSize (at most 4 GiB), and two pages merge into one arena.
            VkDeviceSize page = VkDeviceSize{4} << 30;
            while (page > (VkDeviceSize{256} << 20) && page * 2 > m4.maxBufferSize) {
                page >>= 1;
            }
            std::printf("  arena page %" PRIu64 " MiB, merged %" PRIu64 " MiB\n", page >> 20,
                        (page * 2) >> 20);
            TryArena(gipa, instance, physical, device, queue, sparse_family, page, 8);
            TryArena(gipa, instance, physical, device, queue, sparse_family, page * 2, 8);
            // Beyond the limit: informational only (upstream used 4/8 GiB regardless).
            for (VkDeviceSize size = page * 4; size <= (VkDeviceSize{8} << 30); size *= 2) {
                const int before = failures;
                TryArena(gipa, instance, physical, device, queue, sparse_family, size, 8);
                failures = before;
            }
            destroy_device(device, nullptr);
        }
        Get<PFN_vkDestroyInstance>(gipa, instance, "vkDestroyInstance")(instance, nullptr);
    } catch (const std::exception& e) {
        std::printf("FAIL exception: %s\n", e.what());
        return 1;
    }
    std::printf("failures=%d\n", failures);
    std::fflush(stdout);
    // Skip static destructors of the linked host library, which this probe never set up.
    std::_Exit(failures ? 1 : 0);
}
