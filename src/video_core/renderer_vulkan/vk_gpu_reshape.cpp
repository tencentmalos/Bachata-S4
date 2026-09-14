// SPDX-FileCopyrightText: Copyright 2026 shadPS4 Emulator Project
// SPDX-License-Identifier: MPL-2.0
// Source SDK adapter adapted from Cemu Android GpuReshapeAdapter (2026-09-14).
#include <chrono>
#include "video_core/gpu_reshape_status.h"
#include "video_core/gpu_reshape_config.h"
#include "video_core/renderer_vulkan/vk_gpu_reshape.h"

#include <array>
#include <atomic>
#include <cctype>
#include <charconv>
#include <cstdlib>
#include <cstring>
#include <limits>
#include <mutex>
#include <sstream>
#include <string>
#include <string_view>
#include <unordered_map>
#include <vector>

#if defined(SHADPS4_HAS_GPU_RESHAPE_ANDROID)
#include <GpuReshape/AndroidSdk.h>

#endif

#if defined(__ANDROID__)
#include <sys/system_properties.h>
#endif

namespace GpuReshape {
namespace {
// Runtime opt-in. shadPS4 Android has no settings entry for GPU Reshape yet, so the adapter is
// gated on environment variables (desktop) and, on Android where app processes do not
// inherit shell env, on an equivalent Android system property. All default off.
//   env  SHADPS4_GPU_RESHAPE=1
//   prop debug.shadps4.gpu_reshape=1   (adb shell setprop debug.shadps4.gpu_reshape 1)
std::string RuntimeValue(const char* name) {
    if (const char* v = std::getenv(name); v && *v)
        return v;
#if defined(__ANDROID__)
    // Map SHADPS4_GPU_RESHAPE[_SUFFIX] -> debug.shadps4.gpu_reshape[_suffix] (lowercased,
    // underscores preserved), because Android app processes do not inherit shell env.
    std::string prop = "debug.shadps4.";
    std::string_view sv{name};
    if (sv.starts_with("SHADPS4_"))
        sv.remove_prefix(8);
    for (char c : sv)
        prop.push_back(static_cast<char>(std::tolower(static_cast<unsigned char>(c))));
    char value[PROP_VALUE_MAX] = {};
    if (__system_property_get(prop.c_str(), value) > 0)
        return value;
#endif
    return {};
}

bool EnvFlag(const char* name) {
    return RuntimeValue(name) == "1";
}

#if defined(SHADPS4_HAS_GPU_RESHAPE_ANDROID)
constexpr char kSocketName[] = "shadps4_gpu_reshape";
constexpr uint32_t kResourceTokenPuidMask = (1U << 22U) - 1U;
constexpr uint32_t kResourceTokenPuidFirstDynamic = 4U;
constexpr uint32_t kResourceTokenPuidInvalidStart = kResourceTokenPuidMask - 2U;
constexpr uint32_t kResourceTokenTypeShift = 22U;

struct ResourceViewIdentity {
    uint64_t handle{};
    VkDeviceSize offset{};
    VkDeviceSize range{};
    VkDescriptorType descriptorType{};
    bool operator==(const ResourceViewIdentity&) const = default;
};

struct ResourceViewIdentityHash {
    std::size_t operator()(const ResourceViewIdentity& id) const noexcept {
        std::size_t seed = std::hash<uint64_t>{}(id.handle);
        auto mix = [&](uint64_t v) {
            seed ^= std::hash<uint64_t>{}(v) + 0x9e3779b97f4a7c15ull + (seed << 6) + (seed >> 2);
        };
        mix(id.offset);
        mix(id.range);
        mix(static_cast<uint64_t>(id.descriptorType));
        return seed;
    }
};

const char* TransportStateName(uint32_t state) {
    switch (state) {
    case GRS_ANDROID_TRANSPORT_DISABLED:
        return "disabled";
    case GRS_ANDROID_TRANSPORT_LISTENING:
        return "listening";
    case GRS_ANDROID_TRANSPORT_CONNECTED:
        return "connected";
    case GRS_ANDROID_TRANSPORT_STOPPED:
        return "stopped";
    case GRS_ANDROID_TRANSPORT_ERROR:
        return "error";
    default:
        return "unknown";
    }
}

uint64_t HandleIdentity(VkBuffer h) {
    return reinterpret_cast<uintptr_t>(h);
}
uint64_t HandleIdentity(VkBufferView h) {
    return reinterpret_cast<uintptr_t>(h);
}
uint64_t HandleIdentity(VkImageView h) {
    return reinterpret_cast<uintptr_t>(h);
}
uint64_t HandleIdentity(VkSampler h) {
    return reinterpret_cast<uintptr_t>(h);
}
#endif
} // namespace

struct Adapter::Impl {
    uint64_t generation{};
    std::string driver;
    mutable std::atomic<uint64_t> lastPublication{};
    bool requested = EnvFlag("SHADPS4_GPU_RESHAPE");
    bool resourceBoundsRequested = EnvFlag("SHADPS4_GPU_RESHAPE_RESOURCE_BOUNDS");
    bool safeGuardRequested = EnvFlag("SHADPS4_GPU_RESHAPE_SAFE_GUARD");
    bool shaderReplacementRequested = EnvFlag("SHADPS4_GPU_RESHAPE_SHADER_REPLACEMENT");
    bool shaderAnalysisRequested = EnvFlag("SHADPS4_GPU_RESHAPE_SHADER_ANALYSIS");
    const std::optional<std::vector<uint64_t>> shaderHashConfig =
        ParseShaderHashAllowlist(RuntimeValue("SHADPS4_GPU_RESHAPE_SHADER_HASHES"));
    std::vector<uint64_t> shaderHashAllowlist = shaderHashConfig.value_or(std::vector<uint64_t>{});
    bool graphicsStoresEnabled = false;
    bool indirectHooks = false;
    mutable std::atomic<uint64_t> descriptorRegistrations{0};
    mutable std::atomic<uint64_t> descriptorRegistrationFailures{0};
    bool commandObservationActive = false;
    bool resourceBoundsActive = false;
    bool shaderReplacementActive = false;
    std::string reason = "not_requested";
    PFN_vkGetDeviceProcAddr hookedGetDeviceProcAddr = nullptr;

#if defined(SHADPS4_HAS_GPU_RESHAPE_ANDROID)
    GrsAndroidSdk* sdk = nullptr;
    mutable std::mutex resourceMutex;
    mutable std::unordered_map<ResourceViewIdentity, uint32_t, ResourceViewIdentityHash>
        resourcePuids;
    mutable uint32_t nextPuid = kResourceTokenPuidFirstDynamic;
    struct ImageShape {
        VkExtent3D extent;
        uint32_t layers;
        uint32_t mips;
    };
    struct ImageViewMetadata {
        std::array<uint32_t, GRS_ANDROID_RESOURCE_TOKEN_METADATA_WORD_COUNT> words{};
    };
    std::unordered_map<VkImage, ImageShape> images;
    std::unordered_map<VkImageView, ImageViewMetadata> imageViews;
    inline static Impl* active = nullptr;
    inline static Adapter* adapter = nullptr;

    template <class T>
    static T Next(VkDevice device, const char* name) {
        return reinterpret_cast<T>(active->hookedGetDeviceProcAddr(device, name));
    }

    static VKAPI_ATTR VkResult VKAPI_CALL CreateImage(VkDevice device,
                                                      const VkImageCreateInfo* info,
                                                      const VkAllocationCallbacks* allocator,
                                                      VkImage* image) {
        VkResult result =
            Next<PFN_vkCreateImage>(device, "vkCreateImage")(device, info, allocator, image);
        if (result == VK_SUCCESS) {
            std::scoped_lock lock{active->resourceMutex};
            active->images[*image] = {info->extent, info->arrayLayers, info->mipLevels};
        }
        return result;
    }

    static VKAPI_ATTR void VKAPI_CALL DestroyImage(VkDevice device, VkImage image,
                                                   const VkAllocationCallbacks* allocator) {
        {
            std::scoped_lock lock{active->resourceMutex};
            active->images.erase(image);
        }
        Next<PFN_vkDestroyImage>(device, "vkDestroyImage")(device, image, allocator);
    }

    static VKAPI_ATTR VkResult VKAPI_CALL CreateImageView(VkDevice device,
                                                          const VkImageViewCreateInfo* info,
                                                          const VkAllocationCallbacks* allocator,
                                                          VkImageView* view) {
        VkResult result =
            Next<PFN_vkCreateImageView>(device, "vkCreateImageView")(device, info, allocator, view);
        if (result == VK_SUCCESS) {
            std::scoped_lock lock{active->resourceMutex};
            if (const auto it = active->images.find(info->image);
                it != active->images.end() && active->nextPuid < kResourceTokenPuidInvalidStart) {
                const auto& shape = it->second;
                const auto& range = info->subresourceRange;
                auto& words = active->imageViews[*view].words;
                words[0] = active->nextPuid++;
                words[2] = shape.extent.width;
                words[3] = shape.extent.height;
                words[4] = std::max(shape.extent.depth, shape.layers);
                words[5] = shape.mips;
                words[9] = range.baseMipLevel;
                words[10] = range.baseArrayLayer;
                words[11] = range.layerCount == VK_REMAINING_ARRAY_LAYERS
                                ? shape.layers - range.baseArrayLayer
                                : range.layerCount;
                words[12] = range.levelCount == VK_REMAINING_MIP_LEVELS
                                ? shape.mips - range.baseMipLevel
                                : range.levelCount;
            }
        }
        return result;
    }

    static VKAPI_ATTR void VKAPI_CALL DestroyImageView(VkDevice device, VkImageView view,
                                                       const VkAllocationCallbacks* allocator) {
        {
            std::scoped_lock lock{active->resourceMutex};
            active->imageViews.erase(view);
        }
        Next<PFN_vkDestroyImageView>(device, "vkDestroyImageView")(device, view, allocator);
    }

    static VKAPI_ATTR void VKAPI_CALL UpdateDescriptorSets(VkDevice device, uint32_t count,
                                                           const VkWriteDescriptorSet* writes,
                                                           uint32_t copyCount,
                                                           const VkCopyDescriptorSet* copies) {
        Next<PFN_vkUpdateDescriptorSets>(device, "vkUpdateDescriptorSets")(device, count, writes,
                                                                           copyCount, copies);
        if (copyCount)
            active->descriptorRegistrationFailures.fetch_add(
                copyCount); // Copy metadata is not yet covered.
        for (uint32_t i = 0; i < count; ++i)
            adapter->RegisterDescriptorResources(writes[i].dstSet, {writes + i, 1}, 0);
    }

    static VKAPI_ATTR PFN_vkVoidFunction VKAPI_CALL Resolve(VkDevice device, const char* name) {
        if (!active || !name)
            return nullptr;
        if (std::strcmp(name, "vkGetDeviceProcAddr") == 0)
            return reinterpret_cast<PFN_vkVoidFunction>(Resolve);
        if (active->resourceBoundsActive) {
            if (std::strcmp(name, "vkUpdateDescriptorSets") == 0)
                return reinterpret_cast<PFN_vkVoidFunction>(UpdateDescriptorSets);
            if (std::strcmp(name, "vkCreateImage") == 0)
                return reinterpret_cast<PFN_vkVoidFunction>(CreateImage);
            if (std::strcmp(name, "vkDestroyImage") == 0)
                return reinterpret_cast<PFN_vkVoidFunction>(DestroyImage);
            if (std::strcmp(name, "vkCreateImageView") == 0)
                return reinterpret_cast<PFN_vkVoidFunction>(CreateImageView);
            if (std::strcmp(name, "vkDestroyImageView") == 0)
                return reinterpret_cast<PFN_vkVoidFunction>(DestroyImageView);
        }
        return active->hookedGetDeviceProcAddr(device, name);
    }

    uint32_t ResourcePuid(uint64_t handle, VkDeviceSize offset, VkDeviceSize range,
                          VkDescriptorType type) const {
        if (handle == 0)
            return kResourceTokenPuidMask;
        const ResourceViewIdentity id{handle, offset, range, type};
        auto it = resourcePuids.find(id);
        if (it != resourcePuids.end())
            return it->second;
        if (nextPuid >= kResourceTokenPuidInvalidStart)
            return kResourceTokenPuidMask;
        const uint32_t puid = nextPuid++;
        resourcePuids.emplace(id, puid);
        return puid;
    }
#endif
};

namespace {
std::shared_ptr<const std::string> lastStatus =
    std::make_shared<const std::string>("reason=no_renderer\n");
}
std::string StatusSnapshot() {
    return *std::atomic_load(&lastStatus);
}
void Adapter::SetIdentity(uint64_t generation, std::string driver) {
    impl->generation = generation;
    impl->driver = std::move(driver);
}
void Adapter::PublishStatus(bool force) const {
    if (!force && !IsActive())
        return;
    const auto now = static_cast<uint64_t>(std::chrono::duration_cast<std::chrono::nanoseconds>(
                                               std::chrono::steady_clock::now().time_since_epoch())
                                               .count());
    auto previous = impl->lastPublication.load(std::memory_order_relaxed);
    if (!force && (now - previous < 100000000 ||
                   !impl->lastPublication.compare_exchange_strong(previous, now)))
        return;
    auto text = std::make_shared<const std::string>(StatusText() +
                                                    "snapshot_ns=" + std::to_string(now) + "\n");
    std::atomic_store(&lastStatus, std::move(text));
}
Adapter::Adapter() : impl(std::make_unique<Impl>()) {}
Adapter::~Adapter() {
    Shutdown();
}

void Adapter::ConfigureDeviceFeatures(const VkPhysicalDeviceFeatures& supported,
                                      VkPhysicalDeviceFeatures& enabled,
                                      const VkPhysicalDeviceVulkan12Features& supportedIndexing,
                                      VkPhysicalDeviceVulkan12Features& enabledIndexing) {
#if defined(SHADPS4_HAS_GPU_RESHAPE_ANDROID)
    if (impl->requested && impl->resourceBoundsRequested && impl->shaderReplacementRequested) {
        if (supported.vertexPipelineStoresAndAtomics && supported.fragmentStoresAndAtomics &&
            supportedIndexing.descriptorBindingStorageTexelBufferUpdateAfterBind) {
            enabled.vertexPipelineStoresAndAtomics = VK_TRUE;
            enabled.fragmentStoresAndAtomics = VK_TRUE;
            enabledIndexing.descriptorBindingStorageTexelBufferUpdateAfterBind = VK_TRUE;
            impl->graphicsStoresEnabled = true;
        } else
            impl->reason = "instrumentation_device_features_unsupported";
    }
#else
    (void)supported;
    (void)enabled;
    (void)supportedIndexing;
    (void)enabledIndexing;
#endif
}

bool Adapter::IsActive() const {
    return impl && impl->hookedGetDeviceProcAddr != nullptr;
}

PFN_vkGetDeviceProcAddr Adapter::GetHookedDeviceProcAddr() const {
#if defined(SHADPS4_HAS_GPU_RESHAPE_ANDROID)
    return IsActive() ? Impl::Resolve : nullptr;
#else
    return impl ? impl->hookedGetDeviceProcAddr : nullptr;
#endif
}

bool Adapter::ResourceBoundsActive() const {
    return impl && impl->resourceBoundsActive;
}

bool Adapter::GetInstrumentationBindings(uint32_t& setIndex, VkDescriptorSetLayout& layout,
                                         VkDescriptorSet& descriptorSet,
                                         uint32_t& pushConstantOffset,
                                         uint32_t& pushConstantSize) const {
#if defined(SHADPS4_HAS_GPU_RESHAPE_ANDROID)
    if (!impl->resourceBoundsActive || !impl->sdk)
        return false;
    GrsAndroidResourceBoundsBindings bindings{};
    bindings.struct_size = sizeof(GrsAndroidResourceBoundsBindings);
    if (!grs_android_sdk_get_resource_bounds_bindings(impl->sdk, &bindings))
        return false;
    setIndex = bindings.descriptor_set_index;
    layout = bindings.descriptor_set_layout;
    descriptorSet = bindings.descriptor_set;
    pushConstantOffset = bindings.push_constant_offset;
    pushConstantSize = bindings.push_constant_size;
    return true;
#else
    (void)setIndex;
    (void)layout;
    (void)descriptorSet;
    (void)pushConstantOffset;
    (void)pushConstantSize;
    return false;
#endif
}

void Adapter::Initialize(VkInstance instance, VkPhysicalDevice physicalDevice, VkDevice device,
                         uint32_t apiVersion, PFN_vkGetInstanceProcAddr getInstanceProcAddr,
                         PFN_vkGetDeviceProcAddr getDeviceProcAddr) {
    if (!impl->requested)
        return;
#if !defined(SHADPS4_HAS_GPU_RESHAPE_ANDROID)
    impl->reason = "adapter_not_compiled";
    (void)instance;
    (void)physicalDevice;
    (void)device;
    (void)apiVersion;
    (void)getInstanceProcAddr;
    (void)getDeviceProcAddr;
#else
    const GrsAndroidSdkCreateInfo createInfo{
        .struct_size = sizeof(GrsAndroidSdkCreateInfo),
        .abi_version = GRS_ANDROID_SDK_ABI_VERSION,
        .requested_vulkan_api_version = apiVersion,
        .next_get_instance_proc_addr = getInstanceProcAddr,
        .next_get_device_proc_addr = getDeviceProcAddr,
        .abstract_socket_name = kSocketName,
        .enable_shader_analysis = impl->shaderAnalysisRequested ? 1U : 0U,
    };
    impl->sdk = grs_android_sdk_create(&createInfo);
    if (!impl->sdk) {
        impl->reason = "sdk_create_failed";
        return;
    }

    impl->indirectHooks = true;
    for (const char* name :
         {"vkCmdDispatchIndirect", "vkCmdDrawIndirectCount", "vkCmdDrawIndexedIndirectCount"}) {
        const auto original = getDeviceProcAddr(device, name);
        impl->indirectHooks &=
            original && grs_vkGetDeviceProcAddr(impl->sdk, device, name) != original;
    }
    if (!impl->indirectHooks)
        impl->reason = "sdk_missing_indirect_hooks";
    VkPhysicalDeviceProperties properties{};
    const auto getProperties = reinterpret_cast<PFN_vkGetPhysicalDeviceProperties>(
        getInstanceProcAddr(instance, "vkGetPhysicalDeviceProperties"));
    if (getProperties)
        getProperties(physicalDevice, &properties);
    // Main PushData occupies at most 128 bytes; SDK appends an 8-byte PCID.
    const bool pushBudget = properties.limits.maxPushConstantsSize >= 136;
    if (!pushBudget)
        impl->reason = "instrumentation_push_constant_budget_unsupported";
    if (impl->resourceBoundsRequested && impl->shaderReplacementRequested &&
        impl->graphicsStoresEnabled && impl->indirectHooks && pushBudget && impl->shaderHashConfig) {
        constexpr uint32_t kStreamWordCapacity = 65536U;
        const GrsAndroidResourceBoundsConfig config{
            .struct_size = sizeof(GrsAndroidResourceBoundsConfig),
            .mode = GRS_ANDROID_RESOURCE_BOUNDS_SHADER_REPLACEMENT,
            .export_id = 0U,
            .descriptor_export_id = 1U,
            // Main guest pipelines use set 0. SDK pipeline hooks select the actual next
            // free set and append PCID after each application push constant range.
            .instrumentation_descriptor_set = 1U,
            .counter_descriptor_binding = 0U,
            .stream_descriptor_binding = 1U,
            .stream_descriptor_count = 2U,
            .stream_word_capacity = kStreamWordCapacity,
            .prmt_descriptor_binding = 2U,
            .descriptor_data_descriptor_binding = 3U,
            .instrumentation_push_constant_offset = 0U,
            .detail = 1U,
            .safe_guard = impl->safeGuardRequested ? 1U : 0U,
            .shader_spirv_hash_allowlist_count =
                static_cast<uint32_t>(impl->shaderHashAllowlist.size()),
            .shader_spirv_hash_allowlist = impl->shaderHashAllowlist.data(),
        };
        const GrsAndroidResourceBoundsDeviceCreateInfo deviceInfo{
            .struct_size = sizeof(GrsAndroidResourceBoundsDeviceCreateInfo),
            .instance = instance,
            .physical_device = physicalDevice,
            .device = device,
            .stream_word_capacity = kStreamWordCapacity,
            .prmt_slot_capacity = 32768U,
            .descriptor_data_chunk_capacity = 2048U,
            .stream_segment_capacity = 16U,
            .command_descriptor_set_capacity = 2048U,
        };
        if (grs_android_sdk_configure_resource_bounds(impl->sdk, &config) &&
            grs_android_sdk_set_resource_bounds_update_after_bind(impl->sdk, device, 1U) &&
            grs_android_sdk_attach_resource_bounds_device(impl->sdk, &deviceInfo)) {
            impl->resourceBoundsActive = true;
            impl->shaderReplacementActive = true;
        } else {
            impl->reason = "resource_bounds_attach_failed";
        }
    }

    if (!impl->shaderHashConfig)
        impl->reason = "invalid_shader_hash_allowlist";

    impl->hookedGetDeviceProcAddr = reinterpret_cast<PFN_vkGetDeviceProcAddr>(
        grs_vkGetDeviceProcAddr(impl->sdk, device, "vkGetDeviceProcAddr"));
    if (!impl->hookedGetDeviceProcAddr) {
        impl->reason = "device_proc_resolver_failed";
        Shutdown();
        return;
    }
    impl->commandObservationActive = true;
    Impl::active = impl.get();
    Impl::adapter = this;
    if (impl->resourceBoundsActive)
        impl->reason = "active";
    else if (impl->reason == "not_requested")
        impl->reason = "command_observation_only";
#endif
}

void Adapter::Shutdown() {
#if defined(SHADPS4_HAS_GPU_RESHAPE_ANDROID)
    if (impl && impl->sdk) {
        grs_android_sdk_destroy(impl->sdk);
        impl->sdk = nullptr;
        if (Impl::active == impl.get()) {
            Impl::active = nullptr;
            Impl::adapter = nullptr;
        }
    }
#endif
    if (impl) {
        impl->hookedGetDeviceProcAddr = nullptr;
        impl->commandObservationActive = false;
        impl->resourceBoundsActive = false;
        impl->shaderReplacementActive = false;
    }
}

void Adapter::RegisterDescriptorResources(VkDescriptorSet descriptorSet,
                                          std::span<const VkWriteDescriptorSet> writes,
                                          uint32_t logicalSet) const {
#if defined(SHADPS4_HAS_GPU_RESHAPE_ANDROID)
    if (!impl->resourceBoundsActive)
        return;
    std::scoped_lock lock{impl->resourceMutex};
    std::vector<GrsAndroidResourceBoundsResource> resources;
    size_t count = 0;
    for (const auto& e : writes)
        count += e.descriptorCount;
    resources.reserve(count);
    for (const auto& entry : writes) {
        if (entry.dstSet != descriptorSet)
            continue;
        for (uint32_t element = 0; element < entry.descriptorCount; ++element) {
            uint64_t identity = 0;
            VkDeviceSize offset = 0, range = 0;
            bool boundedBuffer = false;
            switch (entry.descriptorType) {
            case VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER:
            case VK_DESCRIPTOR_TYPE_STORAGE_BUFFER:
            case VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER_DYNAMIC:
            case VK_DESCRIPTOR_TYPE_STORAGE_BUFFER_DYNAMIC: {
                const auto& b = entry.pBufferInfo[element];
                identity = HandleIdentity(b.buffer);
                offset = b.offset;
                range = b.range;
                boundedBuffer = b.buffer != VK_NULL_HANDLE;
                break;
            }
            case VK_DESCRIPTOR_TYPE_UNIFORM_TEXEL_BUFFER:
            case VK_DESCRIPTOR_TYPE_STORAGE_TEXEL_BUFFER:
                identity = HandleIdentity(entry.pTexelBufferView[element]);
                break;
            case VK_DESCRIPTOR_TYPE_SAMPLER:
            case VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER:
            case VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE:
            case VK_DESCRIPTOR_TYPE_STORAGE_IMAGE:
            case VK_DESCRIPTOR_TYPE_INPUT_ATTACHMENT: {
                const auto& img = entry.pImageInfo[element];
                identity = entry.descriptorType == VK_DESCRIPTOR_TYPE_SAMPLER
                               ? HandleIdentity(img.sampler)
                               : HandleIdentity(img.imageView);
                break;
            }
            default:
                break;
            }

            uint32_t tokenType = 0;
            switch (entry.descriptorType) {
            case VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER:
            case VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER_DYNAMIC:
                tokenType = 2U;
                break;
            case VK_DESCRIPTOR_TYPE_STORAGE_BUFFER:
            case VK_DESCRIPTOR_TYPE_STORAGE_BUFFER_DYNAMIC:
            case VK_DESCRIPTOR_TYPE_UNIFORM_TEXEL_BUFFER:
            case VK_DESCRIPTOR_TYPE_STORAGE_TEXEL_BUFFER:
                tokenType = 1U;
                break;
            case VK_DESCRIPTOR_TYPE_SAMPLER:
                tokenType = 3U;
                break;
            default:
                tokenType = 0U;
                break;
            }
            const uint32_t puid =
                boundedBuffer ? impl->ResourcePuid(identity, offset, range, entry.descriptorType)
                              : kResourceTokenPuidMask;
            const uint32_t width =
                boundedBuffer
                    ? static_cast<uint32_t>(std::min<VkDeviceSize>(
                          range == VK_WHOLE_SIZE ? std::numeric_limits<uint32_t>::max() : range,
                          std::numeric_limits<uint32_t>::max()))
                    : 1U;
            GrsAndroidResourceBoundsResource res{};
            res.struct_size = sizeof(GrsAndroidResourceBoundsResource);
            res.descriptor_set = logicalSet;
            res.descriptor_binding = entry.dstBinding;
            res.descriptor_array_element = entry.dstArrayElement + element;
            res.descriptor_type = entry.descriptorType;
            res.metadata_word_count = GRS_ANDROID_RESOURCE_TOKEN_METADATA_WORD_COUNT;
            res.metadata_words[0] = (tokenType << kResourceTokenTypeShift) | puid;
            res.metadata_words[1] = 1U << 16U;
            res.metadata_words[2] = width;
            res.metadata_words[3] = 1U;
            res.metadata_words[4] = 1U;
            res.metadata_words[5] = 1U;
            res.metadata_words[6] = 1U << 16U;
            res.metadata_words[7] = boundedBuffer ? 0U : static_cast<uint32_t>(offset);
            res.metadata_words[8] = width;
            if (entry.pImageInfo && entry.descriptorType != VK_DESCRIPTOR_TYPE_SAMPLER) {
                if (const auto it = impl->imageViews.find(entry.pImageInfo[element].imageView);
                    it != impl->imageViews.end())
                    std::copy(it->second.words.begin(), it->second.words.end(), res.metadata_words);
            } else if (entry.descriptorType == VK_DESCRIPTOR_TYPE_SAMPLER && identity)
                res.metadata_words[0] = (tokenType << kResourceTokenTypeShift) |
                                        impl->ResourcePuid(identity, 0, 0, entry.descriptorType);
            res.resource_handle = identity;
            res.resource_offset = offset;
            res.resource_range = range;
            resources.push_back(res);
        }
    }
    if (resources.empty())
        return;
    // vkUpdateDescriptorSets is incremental. The plural SDK function replaces
    // the whole set; using it once per write silently erased earlier bindings,
    // causing false Descriptor faults (and safe-guarded black output).
    for (const auto& resource : resources) {
        const GrsAndroidResourceBoundsDescriptorResource update{
            .struct_size = sizeof(GrsAndroidResourceBoundsDescriptorResource),
            .descriptor_set_handle = descriptorSet,
            .resource = resource,
        };
        if (grs_android_sdk_set_resource_bounds_descriptor_resource(impl->sdk, &update))
            ++impl->descriptorRegistrations;
        else
            ++impl->descriptorRegistrationFailures;
    }
#else
    (void)descriptorSet;
    (void)writes;
    (void)logicalSet;
#endif
}

std::string Adapter::StatusText() const {
    std::ostringstream out;
    out << "schema=shadps4.gpu-reshape.v1\n";
#if !defined(SHADPS4_HAS_GPU_RESHAPE_ANDROID)
    out << "adapter_compiled=false\n";
#else
    out << "adapter_compiled=true\n";
#endif
    out << "generation=" << impl->generation << "\n";
    out << "driver=" << impl->driver << "\n";
    out << "sampled_status=true\n";
    out << "requested=" << (impl->requested ? "true" : "false") << "\n";
    out << "buffer_lifetime=not_annotated\n";
    out << "reason=" << impl->reason << "\n";
    out << "command_observation_active=" << (impl->commandObservationActive ? "true" : "false")
        << "\n";
    out << "resource_bounds_active=" << (impl->resourceBoundsActive ? "true" : "false") << "\n";
    out << "shader_replacement_active=" << (impl->shaderReplacementActive ? "true" : "false")
        << "\n";
    out << "shader_hash_allowlist_valid=" << impl->shaderHashConfig.has_value() << "\n";
    out << "descriptor_safe_guard=implicit_when_replaced\n";
    out << "shader_hash_allowlist=" << impl->shaderHashAllowlist.size() << "\n";
    out << "indirect_hooks=" << impl->indirectHooks << "\n";
    out << "graphics_stores_enabled=" << impl->graphicsStoresEnabled << "\n";
    out << "descriptor_registrations=" << impl->descriptorRegistrations.load() << "\n";
    out << "descriptor_registration_failures=" << impl->descriptorRegistrationFailures.load()
        << "\n";
#if defined(SHADPS4_HAS_GPU_RESHAPE_ANDROID)
    if (impl->sdk) {
        GrsAndroidSdkStatus status{};
        status.struct_size = sizeof(GrsAndroidSdkStatus);
        if (grs_android_sdk_get_status(impl->sdk, &status)) {
            out << "transport=" << TransportStateName(status.transport_state) << "\n";
            out << "accepted_connections=" << status.accepted_connections << "\n";
            out << "sent_events=" << status.sent_events << "\n";
            out << "dropped_events=" << status.dropped_events << "\n";
            out << "shader_enqueued_modules=" << status.shader_enqueued_modules << "\n";
            out << "shader_parsed_modules=" << status.shader_parsed_modules << "\n";
            out << "shader_failed_modules=" << status.shader_failed_modules << "\n";
            out << "shader_replacement_attempts=" << status.shader_replacement_attempts << "\n";
            out << "shader_replaced_modules=" << status.shader_replaced_modules << "\n";
            out << "shader_replacement_compile_failures="
                << status.shader_replacement_compile_failures << "\n";
            out << "shader_replacement_driver_rejections="
                << status.shader_replacement_driver_rejections << "\n";
            out << "shader_replacement_fallbacks=" << status.shader_replacement_fallbacks << "\n";
            out << "collector_overflows=" << status.collector_overflows << "\n";
            out << "dynamic_prmt_updates=" << status.dynamic_prmt_updates << "\n";
            out << "dynamic_prmt_chunks=" << status.dynamic_prmt_chunks << "\n";
            out << "dynamic_prmt_misses=" << status.dynamic_prmt_misses << "\n";
            out << "dynamic_prmt_drops=" << status.dynamic_prmt_drops << "\n";
            out << "collector_enqueued=" << status.collector_enqueued << "\n";
            out << "collector_completed=" << status.collector_completed << "\n";
            out << "collector_failed=" << status.collector_failed << "\n";
            out << "collector_dropped=" << status.collector_dropped << "\n";
            out << "collector_descriptor_overflows=" << status.collector_descriptor_overflows
                << "\n";
            out << "automatic_submits=" << status.automatic_submits << "\n";
            out << "automatic_submit_marker_failures=" << status.automatic_submit_marker_failures
                << "\n";
            out << "command_descriptor_set_exhaustions="
                << status.command_descriptor_set_exhaustions << "\n";
        }
        out << "socket=" << kSocketName << "\n";
    }
#endif
    return out.str();
}
} // namespace GpuReshape
