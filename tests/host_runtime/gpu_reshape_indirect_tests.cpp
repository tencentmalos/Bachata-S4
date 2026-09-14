// SPDX-License-Identifier: GPL-2.0-or-later
// SDK ABI forwarding checks; GPU instrumentation coverage is a separate device run.
#include <GpuReshape/AndroidSdk.h>
#include <cstdio>
#include <initializer_list>
#include <cstring>
static unsigned calls{}, checks{}, failed{};
#define CHECK(x) do { ++checks; if (!(x)) { ++failed; std::printf("FAIL %d: %s\n", __LINE__, #x); } } while (0)
static VKAPI_ATTR void VKAPI_CALL Dispatch(VkCommandBuffer cmd, VkBuffer buffer, VkDeviceSize offset) {
    ++calls; CHECK(cmd == reinterpret_cast<VkCommandBuffer>(0x1234));
    CHECK(buffer == reinterpret_cast<VkBuffer>(0x5678)); CHECK(offset == 0x100000001ull);
}
static VKAPI_ATTR void VKAPI_CALL Draw(VkCommandBuffer cmd, VkBuffer buffer, VkDeviceSize offset,
    VkBuffer count, VkDeviceSize countOffset, uint32_t maximum, uint32_t stride) {
    Dispatch(cmd, buffer, offset); CHECK(count == reinterpret_cast<VkBuffer>(0x9abc));
    CHECK(countOffset == 0x200000002ull); CHECK(maximum == 7); CHECK(stride == 32);
}
static VKAPI_ATTR PFN_vkVoidFunction VKAPI_CALL DeviceProc(VkDevice, const char* name) {
    if (std::strcmp(name, "vkCmdDispatchIndirect") == 0) return reinterpret_cast<PFN_vkVoidFunction>(Dispatch);
    if (std::strstr(name, "IndirectCount")) return reinterpret_cast<PFN_vkVoidFunction>(Draw);
    return nullptr;
}
static VKAPI_ATTR PFN_vkVoidFunction VKAPI_CALL InstanceProc(VkInstance, const char* name) {
    if (std::strcmp(name, "vkGetDeviceProcAddr") == 0) return reinterpret_cast<PFN_vkVoidFunction>(DeviceProc);
    return nullptr;
}
int main() {
    GrsAndroidSdkCreateInfo info{}; info.struct_size=sizeof(info);
    info.abi_version=GRS_ANDROID_SDK_ABI_VERSION; info.requested_vulkan_api_version=VK_API_VERSION_1_3;
    info.next_get_instance_proc_addr=InstanceProc; info.next_get_device_proc_addr=DeviceProc;
    auto* sdk=grs_android_sdk_create(&info); CHECK(sdk != nullptr); if (!sdk) return 1;
    auto cmd=reinterpret_cast<VkCommandBuffer>(0x1234);
    auto buffer=reinterpret_cast<VkBuffer>(0x5678);
    auto fn=grs_vkGetDeviceProcAddr(sdk, VK_NULL_HANDLE, "vkCmdDispatchIndirect");
    CHECK(fn && fn != reinterpret_cast<PFN_vkVoidFunction>(Dispatch));
    reinterpret_cast<PFN_vkCmdDispatchIndirect>(fn)(cmd,buffer,0x100000001ull);
    for (const char* name : {"vkCmdDrawIndirectCount", "vkCmdDrawIndexedIndirectCount",
                            "vkCmdDrawIndirectCountKHR", "vkCmdDrawIndexedIndirectCountKHR"}) {
        fn=grs_vkGetDeviceProcAddr(sdk, VK_NULL_HANDLE, name);
        CHECK(fn && fn != reinterpret_cast<PFN_vkVoidFunction>(Draw));
        reinterpret_cast<PFN_vkCmdDrawIndirectCount>(fn)(cmd,buffer,0x100000001ull,
            reinterpret_cast<VkBuffer>(0x9abc),0x200000002ull,7,32);
    }
    CHECK(calls==5); grs_android_sdk_destroy(sdk);
    std::printf("gpu_reshape_indirect: %u checks, %u failures\n",checks,failed);
    return failed ? 1 : 0;
}
