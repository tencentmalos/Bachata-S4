// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once
// Headless probe-only validation chain for a custom ICD loaded without Android's loader.
#include <cstring>
#include <dlfcn.h>
#include <vulkan/vk_layer.h>
#include "video_core/renderer_vulkan/vk_driver.h"
namespace ProbeValidation {
inline PFN_vkGetInstanceProcAddr next{}, layer{};
inline PFN_vkGetDeviceProcAddr next_device{};
inline VkInstance instance{};
inline VkResult VKAPI_CALL SetInstanceData(VkInstance, void*) {
    return VK_SUCCESS;
}
inline VkResult VKAPI_CALL SetDeviceData(VkDevice, void*) {
    return VK_SUCCESS;
}
inline VkResult VKAPI_CALL CreateInstance(const VkInstanceCreateInfo* ci,
                                          const VkAllocationCallbacks* a, VkInstance* out) {
    VkLayerInstanceLink link{nullptr, next, reinterpret_cast<PFN_GetPhysicalDeviceProcAddr>(next)};
    VkLayerInstanceCreateInfo data{VK_STRUCTURE_TYPE_LOADER_INSTANCE_CREATE_INFO, ci->pNext,
                                   VK_LOADER_DATA_CALLBACK};
    data.u.pfnSetInstanceLoaderData = SetInstanceData;
    VkLayerInstanceCreateInfo chain{VK_STRUCTURE_TYPE_LOADER_INSTANCE_CREATE_INFO, &data,
                                    VK_LAYER_LINK_INFO};
    chain.u.pLayerInfo = &link;
    auto info = *ci;
    info.pNext = &chain;
    const auto result =
        reinterpret_cast<PFN_vkCreateInstance>(layer(nullptr, "vkCreateInstance"))(&info, a, out);
    if (result == VK_SUCCESS) {
        instance = *out;
        next_device =
            reinterpret_cast<PFN_vkGetDeviceProcAddr>(next(instance, "vkGetDeviceProcAddr"));
    }
    return result;
}
inline VkResult VKAPI_CALL CreateDevice(VkPhysicalDevice physical, const VkDeviceCreateInfo* ci,
                                        const VkAllocationCallbacks* a, VkDevice* out) {
    VkLayerDeviceLink link{nullptr, next, next_device};
    VkLayerDeviceCreateInfo data{VK_STRUCTURE_TYPE_LOADER_DEVICE_CREATE_INFO, ci->pNext,
                                 VK_LOADER_DATA_CALLBACK};
    data.u.pfnSetDeviceLoaderData = SetDeviceData;
    VkLayerDeviceCreateInfo chain{VK_STRUCTURE_TYPE_LOADER_DEVICE_CREATE_INFO, &data,
                                  VK_LAYER_LINK_INFO};
    chain.u.pLayerInfo = &link;
    auto info = *ci;
    info.pNext = &chain;
    return reinterpret_cast<PFN_vkCreateDevice>(layer(instance, "vkCreateDevice"))(physical, &info,
                                                                                   a, out);
}
inline PFN_vkVoidFunction VKAPI_CALL Entry(VkInstance i, const char* name) {
    if (!std::strcmp(name, "vkCreateInstance"))
        return reinterpret_cast<PFN_vkVoidFunction>(CreateInstance);
    if (!std::strcmp(name, "vkCreateDevice"))
        return reinterpret_cast<PFN_vkVoidFunction>(CreateDevice);
    if (!i && std::strcmp(name, "vkGetInstanceProcAddr"))
        return next(i, name);
    if (!std::strcmp(name, "vkGetInstanceProcAddr"))
        return reinterpret_cast<PFN_vkVoidFunction>(Entry);
    return layer(i, name);
}
inline Vulkan::DriverLease Wrap(Vulkan::DriverLease driver, const char* path) {
    if (!path)
        return driver;
    auto* lib = dlopen(path, RTLD_NOW | RTLD_LOCAL);
    if (!lib)
        throw std::runtime_error(dlerror());
    auto negotiate = reinterpret_cast<PFN_vkNegotiateLoaderLayerInterfaceVersion>(
        dlsym(lib, "vkNegotiateLoaderLayerInterfaceVersion"));
    VkNegotiateLayerInterface api{};
    api.sType = LAYER_NEGOTIATE_INTERFACE_STRUCT;
    api.loaderLayerInterfaceVersion = 2;
    if (!negotiate || negotiate(&api) != VK_SUCCESS)
        throw std::runtime_error("validation layer negotiation failed");
    next = driver->entry;
    layer = api.pfnGetInstanceProcAddr;
    next_device = reinterpret_cast<PFN_vkGetDeviceProcAddr>(next(nullptr, "vkGetDeviceProcAddr"));
    return std::make_shared<Vulkan::Driver>(
        Vulkan::Driver{Entry, driver->identity + " validation-probe"});
}
} // namespace ProbeValidation
