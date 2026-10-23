// Standalone system-loader Vulkan probe; no KGSL ioctls or custom ICD.
#include <vulkan/vulkan.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#define VK_OK(expr) do { VkResult r_ = (expr); if (r_ != VK_SUCCESS) { \
    fprintf(stderr, "%s failed: %d\n", #expr, r_); exit(1); } } while (0)

int main(void) {
    setvbuf(stdout, NULL, _IONBF, 0);
    printf("pid=%d loader=system libvulkan.so\n", getpid());
    VkApplicationInfo app = {.sType=VK_STRUCTURE_TYPE_APPLICATION_INFO,
        .pApplicationName="a8xx-vulkan-slot-repro", .apiVersion=VK_API_VERSION_1_2};
    VkInstanceCreateInfo ci = {.sType=VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO,
        .pApplicationInfo=&app};
    VkInstance instance;
    VK_OK(vkCreateInstance(&ci, NULL, &instance));
    uint32_t count=0;
    VK_OK(vkEnumeratePhysicalDevices(instance, &count, NULL));
    VkPhysicalDevice *devs=calloc(count, sizeof(*devs));
    VK_OK(vkEnumeratePhysicalDevices(instance, &count, devs));
    for (uint32_t i=0; i<count; ++i) {
        VkPhysicalDeviceDriverProperties dp={.sType=VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_DRIVER_PROPERTIES};
        VkPhysicalDeviceProperties2 p={.sType=VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_PROPERTIES_2,.pNext=&dp};
        VkPhysicalDeviceBufferDeviceAddressFeatures bda={.sType=VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_BUFFER_DEVICE_ADDRESS_FEATURES};
        VkPhysicalDeviceFeatures2 f={.sType=VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FEATURES_2,.pNext=&bda};
        vkGetPhysicalDeviceProperties2(devs[i], &p);
        vkGetPhysicalDeviceFeatures2(devs[i], &f);
        printf("device=%s vendor=%x driverVersion=%x driverID=%u driverName=%s driverInfo=%s api=%x\n",
            p.properties.deviceName,p.properties.vendorID,p.properties.driverVersion,dp.driverID,dp.driverName,dp.driverInfo,p.properties.apiVersion);
        printf("sparseBinding=%u sparseResidencyBuffer=%u strict=%u bda=%u shaderInt64=%u sparseAddressSpace=%llu\n",
            f.features.sparseBinding,f.features.sparseResidencyBuffer,p.properties.sparseProperties.residencyNonResidentStrict,
            bda.bufferDeviceAddress,f.features.shaderInt64,(unsigned long long)p.properties.limits.sparseAddressSpaceSize);
        uint32_t n=0; vkGetPhysicalDeviceQueueFamilyProperties(devs[i],&n,NULL);
        VkQueueFamilyProperties *q=calloc(n,sizeof(*q));
        vkGetPhysicalDeviceQueueFamilyProperties(devs[i],&n,q);
        for (uint32_t j=0;j<n;++j) printf("queue=%u flags=%x count=%u\n",j,q[j].queueFlags,q[j].queueCount);
        free(q);
    }
    FILE *maps=fopen("/proc/self/maps","r"); char line[1024];
    if(maps) { while(fgets(line,sizeof(line),maps)) if(strstr(line,"vulkan")) fputs(line,stdout); fclose(maps); }
    free(devs); vkDestroyInstance(instance,NULL);
    return 0;
}
