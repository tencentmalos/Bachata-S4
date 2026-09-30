// SPDX-License-Identifier: GPL-2.0-or-later
// Establish the host Vulkan-Hpp configuration before Foundation headers.
#include "video_core/renderer_vulkan/vk_common.h"

#include <cstdlib>
#include "common/logging/log.h"
#include "common/path_util.h"
#include "frontend/window.h"
#include "spatial/upscale/VulkanUpscaler.h"
#include "validation_driver.h"
#include "video_core/renderer_vulkan/host_passes/spatial_upscale.h"
#include "video_core/renderer_vulkan/vk_instance.h"
namespace spatial::upscale {
int RunDeviceTests(const Binding&, VkQueue, uint32_t);
}
struct Window : Frontend::Window {
    s32 GetWidth() const override {
        return 64;
    }
    s32 GetHeight() const override {
        return 64;
    }
    Frontend::WindowSystemInfo GetWindowInfo() const override {
        return {};
    }
    bool RequestKeyboard() override {
        return false;
    }
    void ReleaseKeyboard() override {}
};
int main(int argc, char** argv) {
    if (argc != 3)
        return 2;
    Common::FS::InitializeAndroidUserPaths(argv[2]);
    Common::Log::Setup("upscale-probe");
    int result = 1;
    try {
        auto options = Vulkan::HostPasses::GetSpatialOptions();
        options.foveation = spatial::foveation::Mode::Fixed;
        Vulkan::HostPasses::SetSpatialOptions(options);
        Window window;
        auto driver = Vulkan::LoadAndroidTurnip(argv[1], argv[2]);
        driver = ProbeValidation::Wrap(driver, std::getenv("UPSCALE_VALIDATE"));
        Vulkan::Instance i(window, 0, false, false, driver);
        result = spatial::upscale::RunDeviceTests(
            {.vulkan = {.instance = i.GetInstance(),
                        .physical_device = i.GetPhysicalDevice(),
                        .device = i.GetDevice(),
                        .vk_get_instance_proc_addr =
                            VULKAN_HPP_DEFAULT_DISPATCHER.vkGetInstanceProcAddr,
                        .vk_get_device_proc_addr =
                            VULKAN_HPP_DEFAULT_DISPATCHER.vkGetDeviceProcAddr,
                        .queue_submit_mutex = &i.QueueMutex()},
             .enabled_fdm = i.IsFdmSupported() ? i.FdmCapabilities()
                                               : spatial::foveation::vulkan::Capabilities{}},
            i.GetGraphicsQueue(), i.GetGraphicsQueueFamilyIndex());
    } catch (const std::exception& error) {
        fprintf(stderr, "upscale probe: %s\n", error.what());
    }
    Common::Log::Shutdown();
    return result;
}
