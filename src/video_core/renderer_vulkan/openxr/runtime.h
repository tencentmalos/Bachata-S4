// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once
#include <cstdint>
#include <functional>
#include <memory>
#include <string>
#include <vector>
#include <vulkan/vulkan_core.h>
#include "video_core/renderer_vulkan/host_passes/spatial_upscale.h"
#include "core/host_runtime/guest_vr_sensor.h"
#include "video_core/renderer_vulkan/openxr/output_extent.h"
#include "spatial/perf/PerfMetrics.hpp"
namespace spatial::imgui::overlay { struct StatusSnapshot; }

namespace Vulkan {
class Instance;
struct Driver;
using DriverLease = std::shared_ptr<const Driver>;
struct Frame;
namespace OpenXr {
// Activity is required by XR_KHR_android_create_instance. Environment attachment
// stays in the existing Foundation JniHelper; this only retains an Activity ref.
bool ConfigureActivity(void* environment, void* activity, bool enabled);
void ReleaseActivity(void* environment, void* activity);
void SetForeground(void* environment, void* activity, bool foreground);
bool IsConfigured();
// Applied once before guest launch; DebugBus can override visibility for A/B.
void ConfigureStatus(int layout, bool psvr_title); // 0 horizontal, 1 vertical, 2 none
void ShowError(std::string detail, DriverLease driver, uint64_t token);
void HideError(uint64_t token = 0);
int PollErrorAction(uint64_t token);
std::string ErrorStatus();
bool ErrorKey(int key, bool down);
// DebugBus xr_frame_copy: status | new (copy only new guest frames) | every.
std::string FrameCopyCommand(const std::vector<std::string>& args);
// While XR presents, the Android window is not drawn unless this mirror is on
// (DebugBus xr_mirror on|off|status; debug.shadps4.xr_mirror=1 starts it on).
bool MirrorEnabled();
std::string MirrorCommand(const std::vector<std::string>& args);
class Runtime {
public:
    static std::unique_ptr<Runtime> Create(); // null for the ordinary launcher
    // Read-only pre-launch query. Creates no session, device, swapchain or input actions.
    static std::array<EyeExtent, 3> ProbeOutputExtents(void* environment, void* activity,
                                                    DriverLease driver);
    ~Runtime();
    VkInstance CreateInstance(PFN_vkGetInstanceProcAddr entry, const VkInstanceCreateInfo& info);
    VkPhysicalDevice PhysicalDevice(VkInstance instance);
    VkDevice CreateDevice(PFN_vkGetInstanceProcAddr entry, VkPhysicalDevice physical,
                          const VkDeviceCreateInfo& info);
    void Start(const Instance& instance);
    void StartError(const Instance& instance, std::string detail);
    int ErrorAction() const;
    std::string ErrorStatus() const;
    // Called once on the XR thread when the frame loop stops on an error (device
    // loss, a failed XR call). The handler must not block or call into this runtime.
    void SetFailureHandler(std::function<void(const std::string&)> handler);
    void Stop();
    // Actual allocated SBS extent, independent of the Android mirror Surface.
    VkExtent2D FrameExtent() const;
    // Copies frame.image into the mailbox. False when nothing was handed over.
    // done (unsignalled by Publish) signals once the copy has read frame.image,
    // so the caller can reuse the frame after it; without one Publish waits.
    bool Publish(const Frame& frame, VkFormat format, VkFence done = VK_NULL_HANDLE);
    void PublishStatus(spatial::imgui::overlay::StatusSnapshot status,
                       std::optional<spatial::perf::DeviceMetrics> device);
    // PresentThread: draws the ImGui::Layer UI (dialogs, notifications) into the
    // XR layer panel. While this runtime presents, it hosts those layers instead
    // of the window frame.
    void UpdateLayers();
    std::array<HostPasses::FoveatedEye,2> Foveation(
        const std::array<Core::HostRuntime::GuestVrSensor::Pose,2>& rendered_eyes,
        const std::array<std::array<float,4>,2>& fov, bool perspective);
    std::string GazeStatus() const;

private:
    Runtime();
    struct Impl;
    std::unique_ptr<Impl> impl;
};
} // namespace OpenXr
} // namespace Vulkan
