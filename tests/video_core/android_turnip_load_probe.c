// SPDX-License-Identifier: GPL-2.0-or-later
// Standalone C process: deliberately do not link a C++ runtime or Vulkan loader.
// Usage: probe /absolute/path/to/vulkan.ad07xx.so
// Loads the Android HAL, creates a device and retires one empty queue submission.
#include <dlfcn.h>
#include <hardware/hwvulkan.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define CHECK(call)                                                                                \
    do {                                                                                           \
        VkResult result = (call);                                                                  \
        if (result != VK_SUCCESS) {                                                                \
            fprintf(stderr, "%s failed: %d\n", #call, result);                                     \
            return 1;                                                                              \
        }                                                                                          \
    } while (0)
#define INSTANCE_PROC(name)                                                                        \
    PFN_vk##name name = (PFN_vk##name)hal->GetInstanceProcAddr(instance, "vk" #name);              \
    if (!name) {                                                                                   \
        fprintf(stderr, "Missing vk%s\n", #name);                                                  \
        return 1;                                                                                  \
    }
#define DEVICE_PROC(name)                                                                          \
    PFN_vk##name name = (PFN_vk##name)GetDeviceProcAddr(device, "vk" #name);                       \
    if (!name) {                                                                                   \
        fprintf(stderr, "Missing vk%s\n", #name);                                                  \
        return 1;                                                                                  \
    }

static int check_no_shared_cxx(void) {
    FILE* maps = fopen("/proc/self/maps", "r");
    if (!maps)
        return 0;
    char line[2048];
    int clean = 1;
    while (fgets(line, sizeof(line), maps)) {
        // Android's allocator/platform libraries may load system libc++.so even
        // before main. Reject the app/NDK runtime that caused the driver failure.
        if (strstr(line, "libc++_shared.so") || strstr(line, "libc++abi.so") ||
            strstr(line, "libunwind.so") || strstr(line, "libstdc++")) {
            fprintf(stderr, "Unexpected shared C++ runtime: %s", line);
            clean = 0;
        }
    }
    fclose(maps);
    return clean;
}

int main(int argc, char** argv) {
    setbuf(stdout, NULL);
    if (argc != 2 || !check_no_shared_cxx())
        return 2;
    void* library = dlopen(argv[1], RTLD_NOW | RTLD_LOCAL);
    if (!library) {
        fprintf(stderr, "dlopen: %s\n", dlerror());
        return 1;
    }
    hw_module_t* module = dlsym(library, HAL_MODULE_INFO_SYM_AS_STR);
    hw_device_t* raw = NULL;
    if (!module || !module->methods ||
        module->methods->open(module, HWVULKAN_DEVICE_0, &raw) != 0 || !raw) {
        fprintf(stderr, "Vulkan HAL open failed\n");
        return 1;
    }
    hwvulkan_device_t* hal = (hwvulkan_device_t*)raw;
    VkApplicationInfo app = {.sType = VK_STRUCTURE_TYPE_APPLICATION_INFO,
                             .pApplicationName = "turnip-runtime-probe",
                             .apiVersion = VK_API_VERSION_1_3};
    VkInstanceCreateInfo instance_info = {.sType = VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO,
                                          .pApplicationInfo = &app};
    VkInstance instance;
    CHECK(hal->CreateInstance(&instance_info, NULL, &instance));
    INSTANCE_PROC(DestroyInstance);
    INSTANCE_PROC(EnumeratePhysicalDevices);
    INSTANCE_PROC(GetPhysicalDeviceProperties2);
    INSTANCE_PROC(GetPhysicalDeviceQueueFamilyProperties);
    INSTANCE_PROC(EnumerateDeviceExtensionProperties);
    INSTANCE_PROC(CreateDevice);
    INSTANCE_PROC(GetDeviceProcAddr);
    uint32_t count = 1;
    VkPhysicalDevice physical;
    CHECK(EnumeratePhysicalDevices(instance, &count, &physical));
    if (count != 1)
        return 1;
    VkPhysicalDeviceDriverProperties driver = {
        .sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_DRIVER_PROPERTIES};
    VkPhysicalDeviceProperties2 properties = {
        .sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_PROPERTIES_2, .pNext = &driver};
    GetPhysicalDeviceProperties2(physical, &properties);
    printf("device=%s driver=%s info=%s id=%u\n", properties.properties.deviceName,
           driver.driverName, driver.driverInfo, driver.driverID);
    if (driver.driverID != VK_DRIVER_ID_MESA_TURNIP)
        return 1;
    uint32_t extension_count = 0;
    CHECK(EnumerateDeviceExtensionProperties(physical, NULL, &extension_count, NULL));
    VkExtensionProperties* extensions = calloc(extension_count, sizeof(*extensions));
    if (!extensions)
        return 1;
    CHECK(EnumerateDeviceExtensionProperties(physical, NULL, &extension_count, extensions));
    int ext_calibrated = 0, khr_calibrated = 0;
    for (uint32_t i = 0; i < extension_count; ++i) {
        ext_calibrated |= !strcmp(extensions[i].extensionName, "VK_EXT_calibrated_timestamps");
        khr_calibrated |= !strcmp(extensions[i].extensionName, "VK_KHR_calibrated_timestamps");
    }
    free(extensions);
    printf("calibrated timestamps: EXT=%d KHR=%d\n", ext_calibrated, khr_calibrated);
    const int call_calibrated = getenv("PROBE_CALL_CALIBRATED") != NULL;
    if (getenv("PROBE_EXPECT_NO_CALIBRATED") && (ext_calibrated || khr_calibrated)) {
        fprintf(stderr, "FAIL: KGSL advertises an unimplemented timestamp callback\n");
        return 1;
    }
    GetPhysicalDeviceQueueFamilyProperties(physical, &count, NULL);
    VkQueueFamilyProperties* families = calloc(count, sizeof(*families));
    if (!families)
        return 1;
    GetPhysicalDeviceQueueFamilyProperties(physical, &count, families);
    uint32_t family = 0;
    while (family < count &&
           (!families[family].queueCount || !(families[family].queueFlags & VK_QUEUE_GRAPHICS_BIT)))
        ++family;
    free(families);
    if (family == count)
        return 1;
    float priority = 1.0f;
    VkDeviceQueueCreateInfo queue_info = {.sType = VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO,
                                          .queueFamilyIndex = family,
                                          .queueCount = 1,
                                          .pQueuePriorities = &priority};
    VkDeviceCreateInfo device_info = {.sType = VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO,
                                      .queueCreateInfoCount = 1,
                                      .pQueueCreateInfos = &queue_info};
    const char* calibrated_extension = "VK_EXT_calibrated_timestamps";
    if (call_calibrated && ext_calibrated) {
        device_info.enabledExtensionCount = 1;
        device_info.ppEnabledExtensionNames = &calibrated_extension;
    }
    VkDevice device;
    CHECK(CreateDevice(physical, &device_info, NULL, &device));
    DEVICE_PROC(DestroyDevice);
    DEVICE_PROC(GetDeviceQueue);
    DEVICE_PROC(CreateFence);
    DEVICE_PROC(DestroyFence);
    DEVICE_PROC(QueueSubmit);
    DEVICE_PROC(WaitForFences);
    VkQueue queue;
    GetDeviceQueue(device, family, 0, &queue);
    VkFenceCreateInfo fence_info = {.sType = VK_STRUCTURE_TYPE_FENCE_CREATE_INFO};
    VkFence fence;
    CHECK(CreateFence(device, &fence_info, NULL, &fence));
    VkSubmitInfo submit = {.sType = VK_STRUCTURE_TYPE_SUBMIT_INFO};
    CHECK(QueueSubmit(queue, 1, &submit, fence));
    CHECK(WaitForFences(device, 1, &fence, VK_TRUE, 3000000000ULL));
    if (call_calibrated && ext_calibrated) {
        DEVICE_PROC(GetCalibratedTimestampsEXT);
        VkCalibratedTimestampInfoEXT info[2] = {
            {.sType = VK_STRUCTURE_TYPE_CALIBRATED_TIMESTAMP_INFO_EXT,
             .timeDomain = VK_TIME_DOMAIN_DEVICE_EXT},
            {.sType = VK_STRUCTURE_TYPE_CALIBRATED_TIMESTAMP_INFO_EXT,
             .timeDomain = VK_TIME_DOMAIN_CLOCK_MONOTONIC_EXT}};
        const uint64_t sentinel = UINT64_C(0xfeedbad0feedbad0);
        uint64_t values[2] = {sentinel, sentinel}, deviation = sentinel;
        puts("calling advertised device/host calibration");
        CHECK(GetCalibratedTimestampsEXT(device, 2, info, values, &deviation));
        printf("calibrated GPU=%llu CPU=%llu deviation=%llu\n",
               (unsigned long long)values[0], (unsigned long long)values[1],
               (unsigned long long)deviation);
        if (!values[0] || values[0] == sentinel || !values[1] || values[1] == sentinel ||
            deviation == sentinel) {
            fputs("FAIL: advertised calibration returned an unwritten timestamp\n", stderr);
            return 1;
        }
    }
    if (!check_no_shared_cxx())
        return 1;
    DestroyFence(device, fence, NULL);
    DestroyDevice(device, NULL);
    DestroyInstance(instance, NULL);
    raw->close(raw);
    dlclose(library);
    puts("PASS: Turnip HAL, instance, device, queue/fence; no NDK shared C++ runtime mapped");
    return 0;
}
