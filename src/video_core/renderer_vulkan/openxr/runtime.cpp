// SPDX-License-Identifier: GPL-2.0-or-later
#include "video_core/renderer_vulkan/openxr/runtime.h"

#include <jni.h>
#include <android/log.h>
#define XR_USE_PLATFORM_ANDROID
#define XR_USE_TIMESPEC
#define XR_USE_GRAPHICS_API_VULKAN
#define XR_NO_PROTOTYPES
#include <algorithm>
#include <atomic>
#include <chrono>
#include <cmath>
#include <cstring>
#include <mutex>
#include <stdexcept>
#include <string>
#include <thread>
#include <vector>
#include <dlfcn.h>
#include <fstream>
#include <iomanip>
#include <openxr/openxr.h>
#include <openxr/openxr_platform.h>
#include "video_core/renderer_vulkan/openxr/error_panel.h"
#include "video_core/renderer_vulkan/openxr/status_scene.h"
#include "video_core/renderer_vulkan/openxr/cinema_environment.h"
#include "video_core/renderer_vulkan/openxr/status_panel.h"
#include "video_core/renderer_vulkan/openxr/layer_panel.h"
#include "imgui/renderer/imgui_core.h"
#include "video_core/renderer_vulkan/openxr/output_extent.h"
#if defined(SHADPS4_HAS_SCRCPY_CAPTURE_SDK)
#include "video_core/renderer_vulkan/openxr/xr_capture.h"
#endif
#include <sys/system_properties.h>
#include <vk_mem_alloc.h>
#include "common/logging/log.h"
#include "common/profiler.h"
#include "common/path_util.h"
#include "core/host_runtime/guest_vr_sensor.h"
#include "core/host_runtime/vr_geometry.h"
#include "core/host_runtime/openxr_pad_mapping.h"
#include "core/host_runtime/orbis_pad_adapter.h"
#include "core/libraries/hmd/hmd.h"
#include "spatial/platform/android/JniHelper.h"
#include "spatial/xr/XrInputSystem.h"
#include "spatial/xr/XrEyeGazeTracker.h"
#include "spatial/xr/XrHandJointTracker.h"
#include "spatial/xr/XrMath.h"
#include "video_core/renderer_vulkan/vk_instance.h"
#include "video_core/renderer_vulkan/vk_driver.h"
#include "video_core/renderer_vulkan/vk_presenter.h"

namespace Vulkan::OpenXr {
unsigned ErrorKeys();
namespace {
using Sensor = Core::HostRuntime::GuestVrSensor;
using Jni = spatial::platform::JniHelper;
std::mutex activity_mutex;
std::mutex runtime_creation_mutex;
std::atomic_uint live_runtimes{};
std::mutex output_cache_mutex;
std::string SystemFingerprint() {
    char value[PROP_VALUE_MAX]{};
    __system_property_get("ro.build.fingerprint", value);
    return value;
}
auto OutputCachePath() {
    return Common::FS::GetUserPath(Common::FS::PathType::CacheDir) / "xr-output-capabilities.txt";
}
void SaveOutputLimits(const DriverLease& driver, const std::array<EyeExtent, 2>& extents) {
    if (!driver) return;
    std::scoped_lock lock(output_cache_mutex);
    try {
        const auto path = OutputCachePath();
        std::filesystem::create_directories(path.parent_path());
        const auto temporary = std::filesystem::path(path.string() + ".tmp");
        std::ofstream file(temporary);
        file << "XR_OUTPUT_1\n" << std::quoted(SystemFingerprint()) << '\n'
             << std::quoted(driver->identity) << '\n';
        for (auto e : extents) file << e.width << ' ' << e.height << '\n';
        file.close();
        if (!file) throw std::runtime_error("XR capability cache write failed");
        std::filesystem::rename(temporary, path);
    } catch (const std::exception& e) {
        LOG_WARNING(Render_Vulkan, "Cannot cache XR output capabilities: {}", e.what());
    }
}
std::array<EyeExtent, 3> ReadOutputLimits(const DriverLease& driver) {
    std::scoped_lock lock(output_cache_mutex);
    std::array<EyeExtent, 3> result{};
    try {
        const auto path = OutputCachePath();
        if (!driver || std::filesystem::file_size(path) > 8192) return result;
        std::ifstream file(path);
        std::string magic, fingerprint, identity;
        EyeExtent recommended{}, limit{};
        if (!(file >> magic >> std::quoted(fingerprint) >> std::quoted(identity)
              >> recommended.width >> recommended.height >> limit.width >> limit.height) ||
            magic != "XR_OUTPUT_1" || fingerprint != SystemFingerprint() || identity != driver->identity ||
            limit.width > 32768 || limit.height > 32768) return result;
        for (uint32_t tier = 0; tier < result.size(); ++tier)
            result[tier] = ChooseOutputExtent(recommended, limit, tier);
    } catch (...) { return {}; }
    return result;
}
jobject activity{};
jobject activity_owner{};
std::atomic_bool foreground{false};
uint64_t NowNs() {
    return std::chrono::duration_cast<std::chrono::nanoseconds>(
               std::chrono::steady_clock::now().time_since_epoch())
        .count();
}
void CheckXr(XrResult result, const char* operation) {
    if (XR_FAILED(result))
        throw std::runtime_error(std::string(operation) + ": " + std::to_string(result));
}
void CheckVk(VkResult result, const char* operation) {
    if (result != VK_SUCCESS)
        throw std::runtime_error(std::string(operation) + ": " + std::to_string(result));
}
#define VKF(name) VULKAN_HPP_DEFAULT_DISPATCHER.vk##name
#define XR_FUNCTIONS(X)                                                                            \
    X(DestroyInstance)                                                                             \
    X(GetSystem)                                                                                   \
    X(GetVulkanGraphicsRequirements2KHR) X(CreateVulkanInstanceKHR) X(GetVulkanGraphicsDevice2KHR) \
        X(CreateVulkanDeviceKHR) X(CreateSession) X(DestroySession) X(PollEvent) X(BeginSession)   \
            X(EndSession) X(WaitFrame) X(BeginFrame) X(EndFrame) X(LocateViews) X(LocateSpace)     \
                X(CreateReferenceSpace) X(DestroySpace) X(EnumerateViewConfigurationViews)         \
                    X(EnumerateSwapchainFormats) X(CreateSwapchain) X(DestroySwapchain)            \
                        X(EnumerateSwapchainImages) X(AcquireSwapchainImage) X(WaitSwapchainImage) \
                            X(ReleaseSwapchainImage) X(RequestExitSession)                         \
                                X(ConvertTimeToTimespecTimeKHR)
Sensor::Pose Pose(const XrPosef& p, XrSpaceLocationFlags flags,
                  const XrSpaceVelocity* v = nullptr) {
    Sensor::Pose out;
    out.orientation = {p.orientation.x, p.orientation.y, p.orientation.z, p.orientation.w};
    out.position = {p.position.x, p.position.y, p.position.z};
    out.orientation_valid = flags & XR_SPACE_LOCATION_ORIENTATION_VALID_BIT;
    out.position_valid = flags & XR_SPACE_LOCATION_POSITION_VALID_BIT;
    if (v && (v->velocityFlags & XR_SPACE_VELOCITY_ANGULAR_VALID_BIT))
        out.angular_velocity = {v->angularVelocity.x, v->angularVelocity.y, v->angularVelocity.z};
    if (v && (v->velocityFlags & XR_SPACE_VELOCITY_LINEAR_VALID_BIT)) {
        out.linear_velocity = {v->linearVelocity.x, v->linearVelocity.y, v->linearVelocity.z};
        out.linear_velocity_valid = true;
    }
    return out;
}
XrPosef XrPose(const Sensor::Pose& p) {
    return {{p.orientation[0], p.orientation[1], p.orientation[2], p.orientation[3]},
            {p.position[0], p.position[1], p.position[2]}};
}
// A runtime frame without a new guest image submits the layers again without
// acquiring: the runtime composites the image last released. "every" copies the
// mailbox on every rendered runtime frame instead (the previous behaviour, A/B).
std::atomic_int copy_mode{-1}; // -1 unread, 0 new frames only, 1 every frame
std::atomic_uint64_t frames_copied{}, frames_repeated{};
bool CopyEveryFrame() {
    int mode = copy_mode.load(std::memory_order_relaxed);
    if (mode < 0) [[unlikely]] {
        char value[PROP_VALUE_MAX]{};
        mode = __system_property_get("debug.shadps4.xr_copy_every_frame", value) > 0 &&
               value[0] == '1';
        int expected = -1;
        copy_mode.compare_exchange_strong(expected, mode, std::memory_order_relaxed);
        mode = copy_mode.load(std::memory_order_relaxed);
    }
    return mode == 1;
}
} // namespace

namespace {
std::atomic_int mirror_mode{-1}; // -1 unread, 0 off, 1 on
}
bool MirrorEnabled() {
    int mode = mirror_mode.load(std::memory_order_relaxed);
    if (mode < 0) [[unlikely]] {
        char value[PROP_VALUE_MAX]{};
        mode = __system_property_get("debug.shadps4.xr_mirror", value) > 0 && value[0] == '1';
        int expected = -1;
        mirror_mode.compare_exchange_strong(expected, mode, std::memory_order_relaxed);
        mode = mirror_mode.load(std::memory_order_relaxed);
    }
    return mode == 1;
}
std::string MirrorCommand(const std::vector<std::string>& args) {
    if (args.size() == 1 && (args[0] == "on" || args[0] == "off")) {
        mirror_mode.store(args[0] == "on" ? 1 : 0);
    } else if (!args.empty() && !(args.size() == 1 && args[0] == "status")) {
        return "usage: xr_mirror status | on | off\n";
    }
    return fmt::format("xr_mirror={}\n", MirrorEnabled() ? "on" : "off");
}

std::string FrameCopyCommand(const std::vector<std::string>& args) {
    if (args.size() == 1 && (args[0] == "every" || args[0] == "new")) {
        copy_mode.store(args[0] == "every" ? 1 : 0);
    } else if (!args.empty() && !(args.size() == 1 && args[0] == "status")) {
        return "usage: xr_frame_copy status | new | every\n";
    }
    return fmt::format("xr_frame_copy={} copied={} repeated={}\n",
                       CopyEveryFrame() ? "every" : "new", frames_copied.load(),
                       frames_repeated.load());
}

bool ConfigureActivity(void* environment, void* object, bool enabled) {
    auto* env = static_cast<JNIEnv*>(environment);
    if (!env || !object) return false;
    std::scoped_lock lock(activity_mutex);
    auto owner = env->NewGlobalRef(static_cast<jobject>(object));
    auto next = enabled ? env->NewGlobalRef(static_cast<jobject>(object)) : nullptr;
    if (!owner || (enabled && !next)) {
        if (owner) env->DeleteGlobalRef(owner);
        return false;
    }
    if (activity) env->DeleteGlobalRef(activity);
    if (activity_owner) env->DeleteGlobalRef(activity_owner);
    activity = next;
    activity_owner = owner;
    foreground = false; // Only the owning Activity's onResume admits input.
    return true;
}
void ReleaseActivity(void* environment, void* object) {
    auto* env = static_cast<JNIEnv*>(environment);
    std::scoped_lock lock(activity_mutex);
    if (!env || !env->IsSameObject(activity_owner, static_cast<jobject>(object))) return;
    if (activity) env->DeleteGlobalRef(activity);
    if (activity_owner) env->DeleteGlobalRef(activity_owner);
    activity = activity_owner = nullptr;
    foreground = false;
}
void SetForeground(void* environment, void* object, bool value) {
    auto* env = static_cast<JNIEnv*>(environment);
    std::scoped_lock lock(activity_mutex);
    if (env && activity && env->IsSameObject(activity_owner, static_cast<jobject>(object)))
        foreground = value;
}
bool IsConfigured() {
    std::scoped_lock lock(activity_mutex);
    return activity != nullptr;
}

struct Runtime::Impl {
    bool counted{};
    void* loader{};
    jobject activity_ref{};
    PFN_xrGetInstanceProcAddr proc{};
#define XR_FIELD(name) PFN_xr##name name{};
    XR_FUNCTIONS(XR_FIELD)
#undef XR_FIELD
    XrInstance xr{};
    XrSystemId system{};
    XrSession session{};
    XrSpace local{}, view{};
    XrSwapchain swapchain{};
    XrSessionState state{XR_SESSION_STATE_UNKNOWN};
    XrGraphicsRequirementsVulkan2KHR requirements{XR_TYPE_GRAPHICS_REQUIREMENTS_VULKAN2_KHR};
    uint32_t width{}, height{}, eye_width{}, eye_height{};
    VkFormat format{};
    std::vector<XrSwapchainImageVulkanKHR> images;
    std::unique_ptr<spatial::xr::XrInputSystem> input;
    std::unique_ptr<spatial::xr::XrEyeGazeTracker> gaze_tracker;
    bool gaze_extension{}, gaze_supported{};
    // Hand joints (XR_EXT_hand_tracking): the palms place a DualShock 4 held in both hands.
    std::unique_ptr<spatial::xr::XrHandJointTracker> hand_tracker;
    bool hand_extension{};
    std::mutex gaze_mutex;
    spatial::xr::EyeGazeSample gaze_sample{};
    uint64_t gaze_received{}, gaze_valid_count{}, gaze_invalid_count{};
    const Instance* vk{};
    std::jthread pump;
    uint64_t generation{}, pad_token{};
    std::array<float, 2> vibration{};
    bool running{};
    std::string error_detail;
    std::atomic_int error_action{};
    std::atomic_uint64_t ui_frames{};
    std::mutex failure_mutex;
    std::string failure;
    // Called on the XR thread when the frame loop stops on an error. Held while
    // it runs, so clearing it waits out a call in progress.
    std::mutex failure_handler_mutex;
    std::function<void(const std::string&)> failure_handler;
    bool error_left{}, error_right{}, error_up{}, error_down{}, error_accept{}, error_cancel{};
    Sensor::HardwareFrame error_input{};
    bool presence_extension{}, user_present{};
    std::atomic_bool failed{false};
    bool warned_missing_pose{}, logged_projection{};
    // Mailbox contains a complete owned GPU image, never a guest/frame-pool pointer.
    std::mutex mailbox_mutex;
    std::mutex status_mutex;
    std::mutex status_panel_mutex;
    std::unique_ptr<StatusPanel> status_panel;
    std::unique_ptr<LayerPanel> layer_panel; // under status_panel_mutex
    bool layer_panel_failed{};
    std::atomic_bool hosting_layers{};
    std::atomic_bool immersive{};
    bool status_panel_failed{};
    spatial::imgui::overlay::StatusSnapshot status_snapshot;
    std::optional<spatial::perf::DeviceMetrics> status_device;
    VkImage mailbox{};
    VmaAllocation allocation{};
    bool has_frame{}, stereo{}, perspective{};
    float display_aspect{16.f / 9.f};
    XrPosef cinema_pose{};
    bool cinema_anchored{};
    XrTime recenter_time{};
    bool recenter_notified{};
    XrSpaceVelocityFlags logged_velocity_flags{~XrSpaceVelocityFlags{0}};
    std::array<Sensor::Pose, 2> render_eyes{};
    std::array<std::array<float, 4>, 2> render_fov{};
    // Publish counts mailbox images; shown is the one last copied into the
    // swapchain and released. Both under mailbox_mutex.
    uint64_t mailbox_serial{}, shown_serial{};
    // The mailbox region holding the published image (top left). A cinema frame
    // smaller than the SBS extent keeps its own size; layers sample only this.
    uint32_t content_width{}, content_height{};
    struct CopyContext {
        VkCommandPool pool{};
        VkCommandBuffer command{};
        VkFence fence{};
        bool pending{}; // submitted, fence not yet waited
    } consumer;
    // Publish rotates through these so that it never waits for its previous copy.
    std::array<CopyContext, 3> producers;
    uint32_t next_producer{};

    template <class T>
    T Load(const char* name, bool required = true) {
        PFN_xrVoidFunction p{};
        const auto r = proc(xr, name, &p);
        if (required && (XR_FAILED(r) || !p))
            throw std::runtime_error(std::string("Missing ") + name);
        return reinterpret_cast<T>(p);
    }
    void Init() {
        ++live_runtimes;
        counted = true;
        for (const char* name : {"libopenxr_loader.so", "libpico_openxr_loader.so",
                                 "libopenxr_forwardloader.oculus.so"}) {
            loader = dlopen(name, RTLD_NOW | RTLD_LOCAL);
            if (loader)
                break;
        }
        if (!loader)
            throw std::runtime_error("OpenXR loader unavailable");
        proc = reinterpret_cast<PFN_xrGetInstanceProcAddr>(dlsym(loader, "xrGetInstanceProcAddr"));
        if (!proc)
            throw std::runtime_error("OpenXR loader has no xrGetInstanceProcAddr");
        if (auto init = Load<PFN_xrInitializeLoaderKHR>("xrInitializeLoaderKHR", false)) {
            XrLoaderInitInfoAndroidKHR info{XR_TYPE_LOADER_INIT_INFO_ANDROID_KHR};
            info.applicationVM = Jni::getJavaVM();
            info.applicationContext = Jni::getActivityContext();
            CheckXr(init(reinterpret_cast<XrLoaderInitInfoBaseHeaderKHR*>(&info)),
                    "xrInitializeLoaderKHR");
        }
        auto enumerate = Load<PFN_xrEnumerateInstanceExtensionProperties>(
            "xrEnumerateInstanceExtensionProperties");
        uint32_t count{};
        CheckXr(enumerate(nullptr, 0, &count, nullptr), "xrEnumerateInstanceExtensionProperties");
        std::vector<XrExtensionProperties> properties(count, {XR_TYPE_EXTENSION_PROPERTIES});
        CheckXr(enumerate(nullptr, count, &count, properties.data()),
                "xrEnumerateInstanceExtensionProperties");
        const auto supported = [&](const char* name) {
            return std::ranges::any_of(
                properties, [&](const auto& p) { return std::strcmp(name, p.extensionName) == 0; });
        };
        std::vector<const char*> extensions{XR_KHR_ANDROID_CREATE_INSTANCE_EXTENSION_NAME,
                                            XR_KHR_VULKAN_ENABLE2_EXTENSION_NAME};
        for (auto name : extensions)
            if (!supported(name))
                throw std::runtime_error(std::string("OpenXR requires ") + name);
        for (auto name :
             {XR_KHR_CONVERT_TIMESPEC_TIME_EXTENSION_NAME, "XR_BD_controller_interaction",
              "XR_BD_pico4_controller_interaction", XR_EXT_USER_PRESENCE_EXTENSION_NAME})
            if (supported(name))
                extensions.push_back(name);
        presence_extension = supported(XR_EXT_USER_PRESENCE_EXTENSION_NAME);
        gaze_extension = supported(XR_EXT_EYE_GAZE_INTERACTION_EXTENSION_NAME);
        if(gaze_extension)extensions.push_back(XR_EXT_EYE_GAZE_INTERACTION_EXTENSION_NAME);
        hand_extension = supported(XR_EXT_HAND_TRACKING_EXTENSION_NAME);
        if (hand_extension)
            extensions.push_back(XR_EXT_HAND_TRACKING_EXTENSION_NAME);
        XrInstanceCreateInfoAndroidKHR android{XR_TYPE_INSTANCE_CREATE_INFO_ANDROID_KHR};
        android.applicationVM = Jni::getJavaVM();
        android.applicationActivity = activity_ref;
        XrInstanceCreateInfo create{XR_TYPE_INSTANCE_CREATE_INFO};
        create.next = &android;
        std::strcpy(create.applicationInfo.applicationName, "shadPS4 VR");
        std::strcpy(create.applicationInfo.engineName, "shadPS4");
        create.applicationInfo.apiVersion = XR_MAKE_VERSION(1, 0, 34);
        create.enabledExtensionCount = extensions.size();
        create.enabledExtensionNames = extensions.data();
        CheckXr(Load<PFN_xrCreateInstance>("xrCreateInstance")(&create, &xr), "xrCreateInstance");
#define XR_LOAD(name)                                                                              \
    name = Load<PFN_xr##name>("xr" #name, std::strcmp(#name, "ConvertTimeToTimespecTimeKHR") != 0);
        XR_FUNCTIONS(XR_LOAD)
#undef XR_LOAD
        if (!supported(XR_KHR_CONVERT_TIMESPEC_TIME_EXTENSION_NAME))
            ConvertTimeToTimespecTimeKHR = nullptr;
        XrSystemGetInfo system_info{XR_TYPE_SYSTEM_GET_INFO};
        system_info.formFactor = XR_FORM_FACTOR_HEAD_MOUNTED_DISPLAY;
        CheckXr(GetSystem(xr, &system_info, &system), "xrGetSystem");
        XrSystemEyeGazeInteractionPropertiesEXT gaze_properties{XR_TYPE_SYSTEM_EYE_GAZE_INTERACTION_PROPERTIES_EXT};
        XrSystemProperties system_properties{XR_TYPE_SYSTEM_PROPERTIES};
        if(gaze_extension)system_properties.next=&gaze_properties;
        CheckXr(Load<PFN_xrGetSystemProperties>("xrGetSystemProperties")(xr,system,&system_properties),"xrGetSystemProperties");
        gaze_supported=gaze_extension && gaze_properties.supportsEyeGazeInteraction;
        LOG_INFO(Render_Vulkan,"OpenXR eye gaze: extension={} system={}",gaze_extension,gaze_supported);
        CheckXr(GetVulkanGraphicsRequirements2KHR(xr, system, &requirements),
                "xrGetVulkanGraphicsRequirements2KHR");
        LOG_INFO(Render_Vulkan, "OpenXR instance ready (Vulkan enable2)");
    }
    void InitCopy(CopyContext& c) {
        VkCommandPoolCreateInfo pool{VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO};
        pool.flags = VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT;
        pool.queueFamilyIndex = vk->GetGraphicsQueueFamilyIndex();
        CheckVk(VKF(CreateCommandPool)(vk->GetDevice(), &pool, nullptr, &c.pool),
                "XR command pool");
        VkCommandBufferAllocateInfo alloc{VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO};
        alloc.commandPool = c.pool;
        alloc.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
        alloc.commandBufferCount = 1;
        CheckVk(VKF(AllocateCommandBuffers)(vk->GetDevice(), &alloc, &c.command),
                "XR command buffer");
        VkFenceCreateInfo fence{VK_STRUCTURE_TYPE_FENCE_CREATE_INFO};
        CheckVk(VKF(CreateFence)(vk->GetDevice(), &fence, nullptr, &c.fence), "XR fence");
    }
    // The previous submission of this context has finished: its command buffer
    // can be reset. Normally long done (it ran a guest frame earlier).
    void Settle(CopyContext& c) {
        if (!c.pending) return;
        Common::Profiler::Scope scope{"XR.Mailbox.SettleWait"};
        CheckVk(VKF(WaitForFences)(vk->GetDevice(), 1, &c.fence, VK_TRUE, UINT64_MAX),
                "XR copy completion");
        CheckVk(VKF(ResetFences)(vk->GetDevice(), 1, &c.fence), "XR reset fence");
        c.pending = false;
    }
    // A GPU fault or hang lost the device. Stop before the next xrEndFrame: on a lost device the
    // Pico runtime's commit fails to submit, skips the commit that resets its per-frame layer
    // count and still reports success, so each frame's layers pile up until they overrun its
    // 16-layer slot and crash the process (handle_layer in its IPC client compositor).
    // Mesa answers a fence status query with DEVICE_LOST as soon as any submit saw the loss.
    void CheckDeviceAlive() {
        if (consumer.fence &&
            VKF(GetFenceStatus)(vk->GetDevice(), consumer.fence) == VK_ERROR_DEVICE_LOST)
            throw std::runtime_error("Vulkan device lost (GPU fault or hang); XR frames stopped");
    }
    void BeginCopy(CopyContext& c) {
        Settle(c);
        CheckVk(VKF(ResetCommandPool)(vk->GetDevice(), c.pool, 0), "XR reset pool");
        VkCommandBufferBeginInfo begin{VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO};
        begin.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
        CheckVk(VKF(BeginCommandBuffer)(c.command, &begin), "XR begin copy");
    }
    void Barrier(VkCommandBuffer cb, VkImage image, VkImageLayout from, VkImageLayout to) {
        VkImageMemoryBarrier barrier{VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER};
        barrier.oldLayout = from;
        barrier.newLayout = to;
        barrier.srcQueueFamilyIndex = barrier.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
        barrier.image = image;
        barrier.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
        barrier.srcAccessMask = from == VK_IMAGE_LAYOUT_UNDEFINED
                                    ? 0
                                    : VK_ACCESS_MEMORY_WRITE_BIT | VK_ACCESS_MEMORY_READ_BIT;
        barrier.dstAccessMask = VK_ACCESS_MEMORY_READ_BIT | VK_ACCESS_MEMORY_WRITE_BIT;
        VKF(CmdPipelineBarrier)
        (cb, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT, 0, 0, nullptr,
         0, nullptr, 1, &barrier);
    }
    // block=false leaves the fence to the next Settle. Swapchain release needs no
    // completion: the copy is on the session's queue, ahead of the runtime's use.
    // frame_done: signalled (by an empty submission behind the copy) once everything
    // up to the copy has finished, for the caller's own reuse of the source.
    void FinishCopy(CopyContext& c, VkSemaphore wait = {}, uint64_t tick = 0, bool block = true,
                    VkFence frame_done = VK_NULL_HANDLE) {
        Common::Profiler::Scope scope{wait ? "XR.Mailbox.PublishSubmitWait" : "XR.Mailbox.ConsumeSubmit"};
        CheckVk(VKF(EndCommandBuffer)(c.command), "XR end copy");
        VkPipelineStageFlags stage = VK_PIPELINE_STAGE_ALL_COMMANDS_BIT;
        VkTimelineSemaphoreSubmitInfo timeline{VK_STRUCTURE_TYPE_TIMELINE_SEMAPHORE_SUBMIT_INFO};
        timeline.waitSemaphoreValueCount = wait ? 1 : 0;
        timeline.pWaitSemaphoreValues = &tick;
        VkSubmitInfo submit{VK_STRUCTURE_TYPE_SUBMIT_INFO};
        submit.pNext = &timeline;
        submit.waitSemaphoreCount = wait ? 1 : 0;
        submit.pWaitSemaphores = &wait;
        submit.pWaitDstStageMask = &stage;
        submit.commandBufferCount = 1;
        submit.pCommandBuffers = &c.command;
        {
            std::scoped_lock lock(vk->QueueMutex());
            CheckVk(VKF(QueueSubmit)(vk->GetGraphicsQueue(), 1, &submit, c.fence),
                    "XR submit copy");
            if (frame_done)
                CheckVk(VKF(QueueSubmit)(vk->GetGraphicsQueue(), 0, nullptr, frame_done),
                        "XR frame release");
        }
        c.pending = true;
        // Never hold QueueMutex while waiting: the guest submission worker needs it too.
        if (block) Settle(c);
    }
    std::array<EyeExtent, 2> QueryOutputLimits(const VkPhysicalDeviceProperties& gpu_properties) {
        uint32_t count{};
        CheckXr(EnumerateViewConfigurationViews(
                    xr, system, XR_VIEW_CONFIGURATION_TYPE_PRIMARY_STEREO, 0, &count, nullptr),
                "XR view count");
        if (count != 2)
            throw std::runtime_error("OpenXR runtime requires two primary stereo views");
        std::array<XrViewConfigurationView, 2> views{
            {{XR_TYPE_VIEW_CONFIGURATION_VIEW}, {XR_TYPE_VIEW_CONFIGURATION_VIEW}}};
        CheckXr(EnumerateViewConfigurationViews(
                    xr, system, XR_VIEW_CONFIGURATION_TYPE_PRIMARY_STEREO, 2, &count, views.data()),
                "XR view configuration");
        eye_width =
            std::max(views[0].recommendedImageRectWidth, views[1].recommendedImageRectWidth);
        eye_height =
            std::max(views[0].recommendedImageRectHeight, views[1].recommendedImageRectHeight);
        XrSystemProperties properties{XR_TYPE_SYSTEM_PROPERTIES};
        CheckXr(Load<PFN_xrGetSystemProperties>("xrGetSystemProperties")(xr, system, &properties),
                "XR system properties");
        const auto& limits = gpu_properties.limits;
        const EyeExtent eye_limit{
            std::min({views[0].maxImageRectWidth, views[1].maxImageRectWidth,
                      properties.graphicsProperties.maxSwapchainImageWidth / 2,
                      limits.maxImageDimension2D / 2, limits.maxFramebufferWidth / 2}),
            std::min({views[0].maxImageRectHeight, views[1].maxImageRectHeight,
                      properties.graphicsProperties.maxSwapchainImageHeight,
                      limits.maxImageDimension2D, limits.maxFramebufferHeight})};
        return {{{eye_width, eye_height}, eye_limit}};
    }
    void StartSession(const Instance& instance) {
        vk = &instance;
        XrGraphicsBindingVulkan2KHR binding{XR_TYPE_GRAPHICS_BINDING_VULKAN2_KHR};
        binding.instance = vk->GetInstance();
        binding.physicalDevice = vk->GetPhysicalDevice();
        binding.device = vk->GetDevice();
        binding.queueFamilyIndex = vk->GetGraphicsQueueFamilyIndex();
        XrSessionCreateInfo info{XR_TYPE_SESSION_CREATE_INFO};
        info.next = &binding;
        info.systemId = system;
        // Runtimes may use the graphics queue inside these calls (creating the session,
        // creating a swapchain and listing its images): hold the queue lock as for frames.
        CheckXr(QueueCall(CreateSession, xr, &info, &session), "xrCreateSession");
        XrReferenceSpaceCreateInfo space{XR_TYPE_REFERENCE_SPACE_CREATE_INFO};
        space.poseInReferenceSpace.orientation.w = 1.f;
        space.referenceSpaceType = XR_REFERENCE_SPACE_TYPE_LOCAL;
        CheckXr(CreateReferenceSpace(session, &space, &local), "xrCreateReferenceSpace LOCAL");
        space.referenceSpaceType = XR_REFERENCE_SPACE_TYPE_VIEW;
        CheckXr(CreateReferenceSpace(session, &space, &view), "xrCreateReferenceSpace VIEW");
        VkPhysicalDeviceProperties gpu_properties{};
        VKF(GetPhysicalDeviceProperties)(vk->GetPhysicalDevice(), &gpu_properties);
        const auto extents = QueryOutputLimits(gpu_properties);
        SaveOutputLimits(instance.GetDriverLease(), extents);
        const auto eye_limit = extents[1];
        uint32_t count{};
        const auto output_mode = HostPasses::GetSpatialOptions().output_resolution;
        const auto output = ChooseOutputExtent({eye_width, eye_height}, eye_limit,
            u32(output_mode));
        if (!output.width || !output.height)
            throw std::runtime_error("OpenXR runtime reported an invalid image extent");
        width = output.width * 2;
        height = output.height;
        HostPasses::PublishXrOutputExtent({eye_width, eye_height},
            {eye_limit.width, eye_limit.height}, {output.width, output.height});
        LOG_INFO(Render_Vulkan,
            "XR output mode={} per_eye={}x{} recommended={}x{} limit={}x{} SBS={}x{}",
            u32(output_mode), output.width, output.height, eye_width, eye_height,
            eye_limit.width, eye_limit.height, width, height);
        if (error_detail.empty()) {
        CheckXr(EnumerateSwapchainFormats(session, 0, &count, nullptr),
                "XR swapchain format count");
        std::vector<int64_t> formats(count);
        CheckXr(EnumerateSwapchainFormats(session, count, &count, formats.data()),
                "XR swapchain formats");
        // PostProcessingPass already encodes SDR gamma into an UNORM image.
        // Resize into UNORM, then copy its bytes into an sRGB XR image. Blitting
        // directly to sRGB would encode those bytes a second time.
        for (auto candidate : {VK_FORMAT_R8G8B8A8_SRGB, VK_FORMAT_B8G8R8A8_SRGB}) {
            VkFormatProperties p{};
            const auto copy_format = candidate == VK_FORMAT_R8G8B8A8_SRGB
                ? VK_FORMAT_R8G8B8A8_UNORM : VK_FORMAT_B8G8R8A8_UNORM;
            VKF(GetPhysicalDeviceFormatProperties)(vk->GetPhysicalDevice(), copy_format, &p);
            if (std::ranges::find(formats, candidate) != formats.end() &&
                (p.optimalTilingFeatures & VK_FORMAT_FEATURE_BLIT_DST_BIT)) {
                format = candidate;
                break;
            }
        }
        if (!format || !width || !height)
            throw std::runtime_error("OpenXR requires an RGBA/BGRA sRGB swapchain for SDR presentation");
        XrSwapchainCreateInfo chain{XR_TYPE_SWAPCHAIN_CREATE_INFO};
        chain.usageFlags =
            XR_SWAPCHAIN_USAGE_COLOR_ATTACHMENT_BIT | XR_SWAPCHAIN_USAGE_TRANSFER_DST_BIT;
        chain.format = format;
        chain.sampleCount = 1;
        chain.width = width;
        chain.height = height;
        chain.faceCount = chain.arraySize = chain.mipCount = 1;
        CheckXr(QueueCall(CreateSwapchain, session, &chain, &swapchain), "xrCreateSwapchain");
        CheckXr(EnumerateSwapchainImages(swapchain, 0, &count, nullptr),
                "XR swapchain image count");
        images.resize(count, {XR_TYPE_SWAPCHAIN_IMAGE_VULKAN_KHR});
        CheckXr(QueueCall(EnumerateSwapchainImages, swapchain, count, &count,
                          reinterpret_cast<XrSwapchainImageBaseHeader*>(images.data())),
                "XR swapchain images");
        }
        spatial::xr::XrInputFunctions f;
#define INPUT(name) f.name = Load<decltype(f.name)>("xr" #name)
        // Function table member names begin with lower case.
#define INPUT_FN(field, name) f.field = Load<decltype(f.field)>("xr" #name)
        INPUT_FN(createActionSet, CreateActionSet);
        INPUT_FN(destroyActionSet, DestroyActionSet);
        INPUT_FN(createAction, CreateAction);
        INPUT_FN(destroyAction, DestroyAction);
        INPUT_FN(stringToPath, StringToPath);
        INPUT_FN(suggestInteractionProfileBindings, SuggestInteractionProfileBindings);
        INPUT_FN(attachSessionActionSets, AttachSessionActionSets);
        INPUT_FN(createActionSpace, CreateActionSpace);
        INPUT_FN(destroySpace, DestroySpace);
        INPUT_FN(syncActions, SyncActions);
        INPUT_FN(getActionStateBoolean, GetActionStateBoolean);
        INPUT_FN(getActionStateFloat, GetActionStateFloat);
        INPUT_FN(getActionStateVector2f, GetActionStateVector2f);
        INPUT_FN(getActionStatePose, GetActionStatePose);
        INPUT_FN(locateSpace, LocateSpace);
        INPUT_FN(applyHapticFeedback, ApplyHapticFeedback);
        INPUT_FN(stopHapticFeedback, StopHapticFeedback);
#undef INPUT_FN
#undef INPUT
        std::vector<XrActionSet> extra;
        if(gaze_supported && error_detail.empty()) {
            spatial::xr::XrEyeGazeFunctions g{
                .createActionSet=f.createActionSet,.destroyActionSet=f.destroyActionSet,
                .createAction=f.createAction,.destroyAction=f.destroyAction,.stringToPath=f.stringToPath,
                .suggestInteractionProfileBindings=f.suggestInteractionProfileBindings,
                .createActionSpace=f.createActionSpace,.destroySpace=f.destroySpace,
                .getActionStatePose=f.getActionStatePose,.locateSpace=f.locateSpace,.getInstanceProcAddr=proc};
            gaze_tracker=std::make_unique<spatial::xr::XrEyeGazeTracker>(xr,session,g,
                spatial::xr::XrEyeGazeOptions{.prefer_filtered=true,
                    .platform_hook=spatial::xr::EyeGazePlatformHook::PicoTrackingMode});
            if(gaze_tracker->IsValid())extra.push_back(gaze_tracker->ActionSet());
            else {LOG_WARNING(Render_Vulkan,"Eye tracking unavailable: {}",gaze_tracker->GetError());gaze_tracker.reset();}
        }
        if (hand_extension && error_detail.empty()) {
            hand_tracker = std::make_unique<spatial::xr::XrHandJointTracker>(xr, session, proc);
            if (!hand_tracker->IsValid()) {
                LOG_WARNING(Render_Vulkan, "Hand tracking unavailable: {}", hand_tracker->GetError());
                hand_tracker.reset();
            }
        }
        LOG_INFO(Render_Vulkan, "OpenXR hand tracking: extension={} tracker={}", hand_extension,
                 hand_tracker != nullptr);
        input = std::make_unique<spatial::xr::XrInputSystem>(xr, session, f,
            std::vector<spatial::xr::ProfileBindings>{},extra);
        if(input->IsValid() && gaze_tracker)gaze_tracker->OnActionSetsAttached();
        if (!input->IsValid())
            throw std::runtime_error(input->GetError());
        if (error_detail.empty()) {
        for (auto& c : producers) InitCopy(c);
        InitCopy(consumer);
        VkImageCreateInfo image{VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO};
        image.imageType = VK_IMAGE_TYPE_2D;
        image.format = format == VK_FORMAT_R8G8B8A8_SRGB
                           ? VK_FORMAT_R8G8B8A8_UNORM : VK_FORMAT_B8G8R8A8_UNORM;
        image.extent = {width, height, 1};
        image.mipLevels = image.arrayLayers = 1;
        image.samples = VK_SAMPLE_COUNT_1_BIT;
        image.tiling = VK_IMAGE_TILING_OPTIMAL;
        image.usage = VK_IMAGE_USAGE_TRANSFER_SRC_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT;
        VmaAllocationCreateInfo alloc{};
        alloc.usage = VMA_MEMORY_USAGE_GPU_ONLY;
        CheckVk(vmaCreateImage(vk->GetAllocator(), &image, &alloc, &mailbox, &allocation, nullptr),
                "XR mailbox image");
        }
        generation = Sensor::Instance().BeginOpenXr();
        pad_token = Core::HostRuntime::GlobalPadAdapter().CurrentToken();
        // The game's ImGui layers draw into the XR layer panel, not the window frame.
        if (error_detail.empty()) {
            ImGui::Core::SetExternalLayerHost(true);
            hosting_layers = true;
        }
        pump = std::jthread([this](std::stop_token stop) { Run(stop); });
    }
    void UpdateInput(XrTime time, const std::array<XrView, 2>& eyes, XrViewStateFlags flags) {
        Sensor::HardwareFrame frame{};
        frame.generation = generation;
        frame.received_ns = NowNs();
        frame.predicted_ns = frame.received_ns;
        if (ConvertTimeToTimespecTimeKHR) {
            timespec ts{};
            if (XR_SUCCEEDED(ConvertTimeToTimespecTimeKHR(xr, time, &ts)))
                frame.predicted_ns = uint64_t(ts.tv_sec) * 1'000'000'000 + ts.tv_nsec;
        }
        frame.running = running;
        frame.focused = foreground && state == XR_SESSION_STATE_FOCUSED;
        frame.mounted = presence_extension ? user_present : state == XR_SESSION_STATE_FOCUSED;
        frame.eye_width = eye_width;
        frame.eye_height = eye_height;
        XrSpaceVelocity velocity{XR_TYPE_SPACE_VELOCITY};
        XrSpaceLocation head{XR_TYPE_SPACE_LOCATION, &velocity};
        if (XR_SUCCEEDED(LocateSpace(view, local, time, &head))) {
            frame.head = Pose(head.pose, head.locationFlags, &velocity);
            if (velocity.velocityFlags != logged_velocity_flags) {
                // Whether the runtime gives the head's velocities, or the sensor derives the
                // linear one from positions.
                LOG_INFO(Render_Vulkan, "XR head velocityFlags {:#x} (linear {}, angular {})",
                         velocity.velocityFlags,
                         (velocity.velocityFlags & XR_SPACE_VELOCITY_LINEAR_VALID_BIT) != 0,
                         (velocity.velocityFlags & XR_SPACE_VELOCITY_ANGULAR_VALID_BIT) != 0);
                logged_velocity_flags = velocity.velocityFlags;
            }
        }
        XrSpaceLocationFlags eye_flags{};
        if (flags & XR_VIEW_STATE_ORIENTATION_VALID_BIT)
            eye_flags |= XR_SPACE_LOCATION_ORIENTATION_VALID_BIT;
        if (flags & XR_VIEW_STATE_POSITION_VALID_BIT)
            eye_flags |= XR_SPACE_LOCATION_POSITION_VALID_BIT;
        for (unsigned i = 0; i < 2; ++i) {
            frame.eyes[i] = Pose(eyes[i].pose, eye_flags);
            frame.fov[i] = {eyes[i].fov.angleLeft, eyes[i].fov.angleRight, eyes[i].fov.angleDown,
                            eyes[i].fov.angleUp};
        }
        // Always sync, including shouldRender=false; lost focus releases both hands.
        const auto controls = input->Sync(local, time);
        {
            std::scoped_lock gaze_lock(gaze_mutex);
            gaze_sample = gaze_tracker && frame.focused && frame.mounted
                ? gaze_tracker->Locate(local,time) : spatial::xr::EyeGazeSample{};
            gaze_received=frame.received_ns;
            if(gaze_sample.valid)++gaze_valid_count;else ++gaze_invalid_count;
        }
        if (hand_tracker && frame.focused) {
            for (unsigned i = 0; i < 2; ++i) {
                const auto joints = hand_tracker->Locate(i, local, time);
                if (!joints.located || !joints.tracked[XR_HAND_JOINT_PALM_EXT])
                    continue;
                frame.palms[i] = Pose(joints.poses[XR_HAND_JOINT_PALM_EXT],
                                      XR_SPACE_LOCATION_ORIENTATION_VALID_BIT |
                                          XR_SPACE_LOCATION_POSITION_VALID_BIT);
            }
        }
        for (unsigned i = 0; i < 2; ++i) {
            const auto& c = controls.hands[i];
            auto& h = frame.hands[i];
            if (!frame.focused || !c.active)
                continue;
            h.active = true;
            h.grip = Pose(c.grip_pose, c.grip_flags, &c.grip_velocity);
            h.aim = Pose(c.aim_pose, c.aim_flags);
            Core::HostRuntime::MapXrHand(c, i, h);
        }
        const auto mapping = Core::HostRuntime::MapXrPad(frame.hands);
        const auto& axes = mapping.axes;
        const Core::HostRuntime::PadSnapshot pad{mapping.buttons, axes[0], axes[1], axes[2],
                                                 axes[3],         axes[4], axes[5]};
        if (!error_detail.empty()) {
            error_input = frame;
            error_left = axes[0] < -.5f || axes[2] < -.5f;
            error_right = axes[0] > .5f || axes[2] > .5f;
            error_up = axes[1] < -.5f || axes[3] < -.5f;
            error_down = axes[1] > .5f || axes[3] > .5f;
            error_accept = (mapping.buttons & (0x4000 | 0x8000)) != 0; // Cross / left X
            error_cancel = (mapping.buttons & (0x2000 | 0x1000)) != 0; // Circle / left Y
            return;
        }
        Sensor::Instance().PublishOpenXr(frame);
        Core::HostRuntime::GlobalPadAdapter().SubmitXr(pad_token, pad, mapping.connected);
        const auto haptics = Sensor::Instance().TakeHaptics(generation);
        for (unsigned i = 0; i < 2; ++i) {
            if (!frame.hands[i].active)
                vibration[i] = 0;
            if (haptics[i] >= 0)
                vibration[i] = haptics[i];
            if (vibration[i] > 0 || haptics[i] == 0)
                input->RequestHaptic(static_cast<spatial::xr::Hand>(i), 100'000'000,
                                     XR_FREQUENCY_UNSPECIFIED, vibration[i]);
        }
    }
    void ReleaseInput() {
        {std::scoped_lock lock(gaze_mutex);gaze_sample={};gaze_received=0;}
        Sensor::HardwareFrame empty{};
        empty.generation = generation;
        empty.received_ns = NowNs();
        Sensor::Instance().PublishOpenXr(empty);
        Core::HostRuntime::GlobalPadAdapter().SubmitXr(pad_token, {}, false);
        if (input) {
            input->ResetState();
            for (auto hand : {spatial::xr::Hand::Left, spatial::xr::Hand::Right})
                input->RequestHaptic(hand, XR_MIN_HAPTIC_DURATION, XR_FREQUENCY_UNSPECIFIED, 0);
        }
        vibration = {};
    }
    template <class F, class... Args>
    XrResult QueueCall(F f, Args... args) {
        std::scoped_lock lock(vk->QueueMutex());
        return f(args...);
    }
    void Run(std::stop_token stop) {
        try {
            std::unique_ptr<ErrorPanel> panel;
#if defined(SHADPS4_HAS_SCRCPY_CAPTURE_SDK)
            // Declared before the scenes: they hold its images as copy targets.
            auto xr_capture = std::make_unique<XrCapture>(*vk);
#endif
            std::unique_ptr<StatusScene> status_scene;
            std::unique_ptr<CinemaEnvironment> cinema_env;
            std::string cinema_env_failed_key, cinema_env_wanted = CinemaWorld();
            uint64_t cinema_env_checks{};
            const spatial::xr::XrImguiVulkanBinding scene_binding{
                .api_version = VK_API_VERSION_1_3,
                .instance = vk->GetInstance(), .physical_device = vk->GetPhysicalDevice(),
                .device = vk->GetDevice(), .queue = vk->GetGraphicsQueue(),
                .queue_family_index = vk->GetGraphicsQueueFamilyIndex(),
                .vk_get_instance_proc_addr = VKF(GetInstanceProcAddr),
                .vk_get_device_proc_addr = VKF(GetDeviceProcAddr),
                .queue_submit_mutex = &vk->QueueMutex()};
            // PFN callbacks are called only on this pump thread, including
            // teardown. Acquire/release can submit on the bound Vulkan queue.
            static thread_local Impl* scene_owner{};
            scene_owner = this;
            const spatial::xr::SwapchainFunctions scene_functions{
                CreateSwapchain, DestroySwapchain, EnumerateSwapchainFormats,
                EnumerateSwapchainImages,
                +[](XrSwapchain chain, const XrSwapchainImageAcquireInfo* info, uint32_t* index) {
                    return scene_owner->QueueCall(scene_owner->AcquireSwapchainImage, chain, info, index);
                }, WaitSwapchainImage,
                +[](XrSwapchain chain, const XrSwapchainImageReleaseInfo* info) {
                    return scene_owner->QueueCall(scene_owner->ReleaseSwapchainImage, chain, info);
                }};
            if (!error_detail.empty()) {
                panel = std::make_unique<ErrorPanel>();
                panel->SetDetail(error_detail);
                spatial::xr::XrImguiVulkanBinding binding{
                    .api_version = VK_API_VERSION_1_3,
                    .instance = vk->GetInstance(), .physical_device = vk->GetPhysicalDevice(),
                    .device = vk->GetDevice(), .queue = vk->GetGraphicsQueue(),
                    .queue_family_index = vk->GetGraphicsQueueFamilyIndex(),
                    .vk_get_instance_proc_addr = VKF(GetInstanceProcAddr),
                    .vk_get_device_proc_addr = VKF(GetDeviceProcAddr),
                    .queue_submit_mutex = &vk->QueueMutex()};
                if (!panel->Renderer().Create(session, CreateSwapchain, DestroySwapchain,
                    EnumerateSwapchainFormats, EnumerateSwapchainImages,
                    AcquireSwapchainImage, WaitSwapchainImage, ReleaseSwapchainImage,
                    binding, panel->Config())) throw std::runtime_error("XR ImGui layer creation failed");
            }
            bool panel_submitted = false;
            while (!stop.stop_requested()) {
                XrEventDataBuffer event{XR_TYPE_EVENT_DATA_BUFFER};
                while (PollEvent(xr, &event) == XR_SUCCESS) {
                    if (event.type == XR_TYPE_EVENT_DATA_SESSION_STATE_CHANGED) {
                        state =
                            reinterpret_cast<const XrEventDataSessionStateChanged&>(event).state;
                        LOG_INFO(Render_Vulkan, "OpenXR session state={}", int(state));
                        if (state == XR_SESSION_STATE_READY && !running) {
                            XrSessionBeginInfo begin{XR_TYPE_SESSION_BEGIN_INFO};
                            begin.primaryViewConfigurationType =
                                XR_VIEW_CONFIGURATION_TYPE_PRIMARY_STEREO;
                            CheckXr(BeginSession(session, &begin), "xrBeginSession");
                            running = true;
                            // Do not lean on an image released before the session stopped.
                            std::scoped_lock reset(mailbox_mutex);
                            shown_serial = 0;
                        } else if (state == XR_SESSION_STATE_STOPPING && running) {
                            ReleaseInput();
                            CheckXr(EndSession(session), "xrEndSession");
                            running = false;
                        } else if (state == XR_SESSION_STATE_LOSS_PENDING ||
                                   state == XR_SESSION_STATE_EXITING) {
                            failed = true;
                            ReleaseInput();
                            return;
                        }
                        if (state != XR_SESSION_STATE_FOCUSED)
                            ReleaseInput();
                    } else if (event.type == XR_TYPE_EVENT_DATA_REFERENCE_SPACE_CHANGE_PENDING) {
                        const auto& change = reinterpret_cast<const XrEventDataReferenceSpaceChangePending&>(event);
                        if (change.referenceSpaceType == XR_REFERENCE_SPACE_TYPE_LOCAL) {
                            recenter_time = change.changeTime;
                            recenter_notified = false;
                        }
                    } else if (event.type == XR_TYPE_EVENT_DATA_USER_PRESENCE_CHANGED_EXT) {
                        user_present =
                            reinterpret_cast<const XrEventDataUserPresenceChangedEXT&>(event)
                                .isUserPresent;
                    } else if (event.type == XR_TYPE_EVENT_DATA_INSTANCE_LOSS_PENDING) {
                        failed = true;
                        ReleaseInput();
                        return;
                    }
                    event = {XR_TYPE_EVENT_DATA_BUFFER};
                }
                if (!running) {
                    std::this_thread::sleep_for(std::chrono::milliseconds(10));
                    continue;
                }
                XrFrameWaitInfo wait{XR_TYPE_FRAME_WAIT_INFO};
                XrFrameState frame{XR_TYPE_FRAME_STATE};
                CheckXr(WaitFrame(session, &wait, &frame), "xrWaitFrame");
                XrFrameBeginInfo begin{XR_TYPE_FRAME_BEGIN_INFO};
                CheckXr(QueueCall(BeginFrame, session, &begin), "xrBeginFrame");
                std::array<XrView, 2> eyes{{{XR_TYPE_VIEW}, {XR_TYPE_VIEW}}};
                XrViewLocateInfo locate{XR_TYPE_VIEW_LOCATE_INFO};
                locate.viewConfigurationType = XR_VIEW_CONFIGURATION_TYPE_PRIMARY_STEREO;
                locate.displayTime = frame.predictedDisplayTime;
                locate.space = local;
                XrViewState view_state{XR_TYPE_VIEW_STATE};
                uint32_t count{};
                CheckXr(LocateViews(session, &locate, &view_state, 2, &count, eyes.data()),
                        "xrLocateViews");
                // From the change time on, poses are in the new LOCAL space. Say so before they
                // are published, so the jump is not taken for head motion, and so the title
                // counts positions from where the head is now (ResetVrPosition).
                if (recenter_time && frame.predictedDisplayTime >= recenter_time &&
                    !recenter_notified) {
                    if (!panel)
                        Sensor::Instance().NotifyRecenter();
                    recenter_notified = true;
                }
                UpdateInput(frame.predictedDisplayTime, eyes,
                            count == 2 ? view_state.viewStateFlags : 0);
#if defined(SHADPS4_HAS_SCRCPY_CAPTURE_SDK)
                // capture_source xr: this frame's layers are copied as submitted
                // and composed per eye after xrEndFrame.
                const bool capturing = !panel && frame.shouldRender && count == 2 &&
                                       xr_capture->Begin(eye_width, eye_height);
#endif
                {
                std::scoped_lock cinema_lock(mailbox_mutex);
                if (recenter_time && frame.predictedDisplayTime >= recenter_time) {
                    cinema_anchored = false;
                    if (status_scene) status_scene->Recenter();
                    recenter_time = 0;
                }
                if (!cinema_anchored) {
                    const auto head = panel ? error_input.head : Sensor::Instance().Read().hardware.head;
                    if (head.orientation_valid && head.position_valid) {
                        cinema_pose = XrPose(Core::HostRuntime::VrGeometry::CinemaPlacement(head));
                        if (panel) {
                            cinema_pose = XrPose(head);
                            cinema_pose.position = spatial::xr::math::Transform(
                                cinema_pose, {0, 0, -Core::HostRuntime::VrGeometry::CinemaDistance});
                        }
                        cinema_anchored = true;
                    }
                }
                }
                XrFrameEndInfo end{XR_TYPE_FRAME_END_INFO};
                end.displayTime = frame.predictedDisplayTime;
                end.environmentBlendMode = XR_ENVIRONMENT_BLEND_MODE_OPAQUE;
                std::unique_lock lock(mailbox_mutex);
                std::array<XrCompositionLayerQuad, 2> quads{
                    {{XR_TYPE_COMPOSITION_LAYER_QUAD}, {XR_TYPE_COMPOSITION_LAYER_QUAD}}};
                std::array<const XrCompositionLayerBaseHeader*, 7> layers{};
                XrCompositionLayerProjection projection{XR_TYPE_COMPOSITION_LAYER_PROJECTION};
                std::array<XrCompositionLayerProjectionView, 2> pv{
                    {{XR_TYPE_COMPOSITION_LAYER_PROJECTION_VIEW},
                     {XR_TYPE_COMPOSITION_LAYER_PROJECTION_VIEW}}};
                uint32_t layer_count{};
                const auto projection_valid = [&] {
                    return render_eyes[0].orientation_valid && render_eyes[0].position_valid &&
                    render_eyes[1].orientation_valid && render_eyes[1].position_valid &&
                    Core::HostRuntime::VrGeometry::ValidFov(render_fov[0]) &&
                    Core::HostRuntime::VrGeometry::ValidFov(render_fov[1]);
                };
                if (has_frame && stereo && perspective && !projection_valid() && !warned_missing_pose) {
                    LOG_WARNING(Render_Vulkan, "OpenXR PSVR waiting for a valid submitted render pose and eye calibration");
                    warned_missing_pose = true;
                }
                if (frame.shouldRender && has_frame &&
                    (stereo && perspective ? projection_valid() : cinema_anchored)) {
                    // Capture reads the mailbox in the copy's command buffer.
                    bool fresh = mailbox_serial != shown_serial || CopyEveryFrame();
#if defined(SHADPS4_HAS_SCRCPY_CAPTURE_SDK)
                    fresh = fresh || capturing;
#endif
                    if (fresh) {
                        lock.unlock(); // Runtime pacing must not block publication of a guest image.
                        uint32_t index{};
                        XrSwapchainImageAcquireInfo acquire{XR_TYPE_SWAPCHAIN_IMAGE_ACQUIRE_INFO};
                        CheckXr(QueueCall(AcquireSwapchainImage, swapchain, &acquire, &index),
                                "xrAcquireSwapchainImage");
                        XrSwapchainImageWaitInfo image_wait{XR_TYPE_SWAPCHAIN_IMAGE_WAIT_INFO};
                        image_wait.timeout = XR_INFINITE_DURATION;
                        CheckXr(WaitSwapchainImage(swapchain, &image_wait), "xrWaitSwapchainImage");
                        Settle(consumer); // not under mailbox_mutex: Publish must not wait on it
                        lock.lock();
                        BeginCopy(consumer);
                        Barrier(consumer.command, images.at(index).image, VK_IMAGE_LAYOUT_UNDEFINED,
                                VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL);
                        VkImageCopy copy{};
                        copy.srcSubresource =
                            copy.dstSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1};
                        copy.extent = {content_width, content_height, 1}; // layers sample only this
                        VKF(CmdCopyImage)
                        (consumer.command, mailbox, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
                         images[index].image, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &copy);
#if defined(SHADPS4_HAS_SCRCPY_CAPTURE_SDK)
                        if (capturing)
                            xr_capture->CopyScreen(consumer.command, mailbox,
                                                   format == VK_FORMAT_R8G8B8A8_SRGB
                                                       ? VK_FORMAT_R8G8B8A8_UNORM
                                                       : VK_FORMAT_B8G8R8A8_UNORM,
                                                   width, height);
#endif
                        Barrier(consumer.command, images[index].image,
                                VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
                                VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL);
                        FinishCopy(consumer, {}, 0, false);
                        XrSwapchainImageReleaseInfo release{XR_TYPE_SWAPCHAIN_IMAGE_RELEASE_INFO};
                        CheckXr(QueueCall(ReleaseSwapchainImage, swapchain, &release),
                                "xrReleaseSwapchainImage");
                        shown_serial = mailbox_serial;
                        frames_copied.fetch_add(1, std::memory_order_relaxed);
                    } else {
                        // The released image is this mailbox snapshot; the layers below
                        // carry its own render pose/FOV, as when it was copied.
                        frames_repeated.fetch_add(1, std::memory_order_relaxed);
                    }
                    // As in Azahar's rendered_views_: image, render pose and FOV
                    // are one mailbox snapshot. xrLocateViews above only updates
                    // tracking; it does not relabel an already rendered image.
                    // Re-evaluate after reacquiring the mailbox: publication may
                    // have switched between a 2D and perspective image while waiting.
                    if (stereo && perspective && projection_valid()) {
                        for (unsigned i = 0; i < 2; ++i) {
                            pv[i].pose = XrPose(render_eyes[i]);
                            const auto& f = render_fov[i];
                            pv[i].fov = {f[0], f[1], f[3], f[2]};
                            pv[i].subImage = {swapchain,
                                              {{int32_t(i * content_width / 2), 0},
                                               {int32_t(content_width / 2), int32_t(content_height)}},
                                              0};
                        }
                        projection.space = local;
                        projection.viewCount = 2;
                        projection.views = pv.data();
                        layers[0] = reinterpret_cast<XrCompositionLayerBaseHeader*>(&projection);
                        layer_count = 1;
#if defined(SHADPS4_HAS_SCRCPY_CAPTURE_SDK)
                        if (capturing) xr_capture->ScreenProjection(projection);
#endif
                        if (!logged_projection) {
                            LOG_INFO(Render_Vulkan, "OpenXR PSVR Projection Layer: submitted head pose, runtime eye extrinsics/FOV; IPD={:.3f} mm L=({}, {}, {}, {}) R=({}, {}, {}, {})",
                                spatial::xr::math::EyeSeparation(XrPose(render_eyes[0]).position,
                                                                 XrPose(render_eyes[1]).position) * 1000.f,
                                render_fov[0][0], render_fov[0][1], render_fov[0][2], render_fov[0][3],
                                render_fov[1][0], render_fov[1][1], render_fov[1][2], render_fov[1][3]);
                            logged_projection = true;
                        }
                    } else if (!(stereo && perspective) && cinema_anchored) {
                        // Only ordinary games / explicit PSVR Start2d use a cinema plane.
                        layer_count = stereo ? 2 : 1;
                        for (uint32_t i = 0; i < layer_count; ++i) {
                            auto& quad = quads[i];
                            quad.space = local;
                            quad.eyeVisibility =
                                stereo ? (i ? XR_EYE_VISIBILITY_RIGHT : XR_EYE_VISIBILITY_LEFT)
                                       : XR_EYE_VISIBILITY_BOTH;
                            const auto w = content_width / layer_count;
                            quad.subImage = {
                                swapchain, {{int32_t(i * w), 0}, {int32_t(w), int32_t(content_height)}}, 0};
                            quad.pose = cinema_pose;
                            quad.size = {Core::HostRuntime::VrGeometry::CinemaWidth,
                                         Core::HostRuntime::VrGeometry::CinemaHeight(display_aspect)};
                            layers[i] = reinterpret_cast<XrCompositionLayerBaseHeader*>(&quad);
#if defined(SHADPS4_HAS_SCRCPY_CAPTURE_SDK)
                            if (capturing) xr_capture->ScreenQuad(stereo ? 1u << i : 3u, quad);
#endif
                        }
                    }
                    end.layerCount = layer_count;
                    end.layers = layers.data();
                }
                lock.unlock();
                const auto status_config = ReadStatusSettings();
                const bool immersive_frame = status_config.psvr_title || projection.viewCount == 2;
                // Cinema environment: the World behind the game's quad (bottom,
                // opaque layer). Ordinary cinema only, like the PSV scene; a
                // failure leaves the plain cinema running.
                bool cinema_env_drawn = false;
                if (!panel && !immersive_frame && cinema_anchored && frame.shouldRender && count == 2 &&
                    (view_state.viewStateFlags & (XR_VIEW_STATE_POSITION_VALID_BIT | XR_VIEW_STATE_ORIENTATION_VALID_BIT)) ==
                        (XR_VIEW_STATE_POSITION_VALID_BIT | XR_VIEW_STATE_ORIENTATION_VALID_BIT)) {
                    if (++cinema_env_checks % 72 == 0) cinema_env_wanted = CinemaWorld();
                    if (cinema_env && cinema_env->WorldKey() != cinema_env_wanted) cinema_env.reset();
                    if (!cinema_env && cinema_env_wanted != "off" && cinema_env_wanted != cinema_env_failed_key) {
                        try {
                            char scale_text[PROP_VALUE_MAX]{};
                            float scale = __system_property_get("debug.shadps4.xr_cinema_scale", scale_text) > 0
                                ? std::strtof(scale_text, nullptr) : .5f;
                            scale = std::clamp(std::isfinite(scale) ? scale : .5f, .25f, 1.f);
                            const auto cache = Common::FS::GetUserPath(Common::FS::PathType::CacheDir);
                            auto env = std::make_unique<CinemaEnvironment>();
                            env->Create(session, scene_functions, scene_binding,
                                        std::max(64u, uint32_t(std::min(eye_width, 2592u) * scale)),
                                        std::max(64u, uint32_t(std::min(eye_height, 2400u) * scale)),
                                        cache.string(), cinema_env_wanted);
                            cinema_env = std::move(env);
                            LOG_INFO(Render_Vulkan, "OpenXR cinema environment {} created (scale {:.2f})",
                                     cinema_env_wanted, scale);
                        } catch (const std::exception& e) {
                            cinema_env_failed_key = cinema_env_wanted;
                            LOG_ERROR(Render_Vulkan, "OpenXR cinema environment {} failed: {}",
                                      cinema_env_wanted, e.what());
                        }
                    }
#if defined(SHADPS4_HAS_SCRCPY_CAPTURE_SDK)
                    if (cinema_env) xr_capture->Attach(XrCapture::Scene::Environment, cinema_env->SceneLayer());
#endif
                    if (cinema_env && end.layerCount + 1 < layers.size() &&
                        cinema_env->Render(eyes, local, cinema_pose)) {
                        for (uint32_t i = end.layerCount; i > 0; --i) layers[i] = layers[i - 1];
                        layers[0] = reinterpret_cast<const XrCompositionLayerBaseHeader*>(cinema_env->Layer());
                        ++end.layerCount;
                        end.layers = layers.data();
                        cinema_env_drawn = true;
#if defined(SHADPS4_HAS_SCRCPY_CAPTURE_SDK)
                        xr_capture->Submitted(XrCapture::Scene::Environment, cinema_env->SceneLayer(),
                                              *cinema_env->Layer());
#endif
                    }
                }
                if (!panel && !immersive_frame && end.layerCount && status_config.visible &&
                    frame.shouldRender && count == 2 &&
                    (view_state.viewStateFlags & (XR_VIEW_STATE_POSITION_VALID_BIT | XR_VIEW_STATE_ORIENTATION_VALID_BIT)) ==
                        (XR_VIEW_STATE_POSITION_VALID_BIT | XR_VIEW_STATE_ORIENTATION_VALID_BIT)) {
                    // Ordinary cinema only: never allocate the model/GI during a
                    // PSVR title's startup movies or ReprojectionStart2d frames.
                    if (!status_scene) {
                        status_scene = std::make_unique<StatusScene>();
                        const auto cache = Common::FS::GetUserPath(Common::FS::PathType::CacheDir) / "xr-status-shaders";
                        status_scene->Create(session, scene_functions, scene_binding,
                            std::min(eye_width, 2592u), std::min(eye_height, 2400u), cache.string());
                    }
                    spatial::imgui::overlay::StatusSnapshot status;
                    std::optional<spatial::perf::DeviceMetrics> device;
                    { std::scoped_lock status_lock(status_mutex); status = status_snapshot; device = status_device; }
                    status_scene->Place(cinema_env_drawn
                        ? std::optional<XrPosef>(CinemaAuthoredOrigin(cinema_pose)) : std::nullopt);
#if defined(SHADPS4_HAS_SCRCPY_CAPTURE_SDK)
                    xr_capture->Attach(XrCapture::Scene::Status, status_scene->SceneLayer());
#endif
                    if (status_scene->Render(eyes, local, status, device,
                                            state == XR_SESSION_STATE_FOCUSED,
                                            false, eye_width, eye_height)) {
                        layers[end.layerCount++] = reinterpret_cast<const XrCompositionLayerBaseHeader*>(status_scene->Layer());
                        end.layers = layers.data();
#if defined(SHADPS4_HAS_SCRCPY_CAPTURE_SDK)
                        xr_capture->Submitted(XrCapture::Scene::Status, status_scene->SceneLayer(),
                                              *status_scene->Layer());
#endif
                    }
                }
                if (panel && frame.shouldRender && cinema_anchored) {
                    ErrorPanel::Pointer pointer;
                    for (unsigned hand : {1u, 0u}) {
                        const auto& h = error_input.hands[hand];
                        if (!h.active || !h.aim.position_valid || !h.aim.orientation_valid) continue;
                        auto hit = spatial::xr::math::RayCastQuadLayer(
                            spatial::xr::math::MakeRayFromPose(XrPose(h.aim)), cinema_pose,
                            {3.2f, 1.8f});
                        if (!hit.hit) continue;
                        pointer = {(hit.x_offset + .5f) * 1280, (.5f - hit.y_offset) * 720,
                                   -h.stick_y, true, h.trigger > .5f};
                        break;
                    }
                    auto keys = ErrorKeys();
                    int action = panel->Draw(error_left || (keys & 1), error_right || (keys & 2),
                        (keys & 16) || (!pointer.visible && error_up),
                        (keys & 32) || (!pointer.visible && error_down),
                        error_accept || (keys & 4), error_cancel || (keys & 8),
                        foreground && state == XR_SESSION_STATE_FOCUSED, pointer,
                        float(frame.predictedDisplayPeriod) / 1e9f);
                    if (action) error_action = action;
                    auto& quad = quads[0];
                    // Use the standard positive-height quad. Swan compositor
                    // capture confirms this canvas is upright without a Y flip.
                    panel->Renderer().FillQuadLayer(local, false, quad);
                    quad.pose = cinema_pose;
                    layers[0] = reinterpret_cast<XrCompositionLayerBaseHeader*>(&quad);
                    end.layerCount = 1;
                    end.layers = layers.data();
                }
                // Serialize only this layer's release against composition. Its
                // producer uses try_lock, so xrEndFrame can never block guest
                // presentation through a status update. No ImGui on this thread.
                std::unique_lock status_panel_lock(status_panel_mutex);
                XrCompositionLayerQuad status_quad{XR_TYPE_COMPOSITION_LAYER_QUAD};
                if (!panel && immersive_frame && frame.shouldRender && status_config.visible &&
                    status_panel && !status_panel_failed && status_panel->Fill(view, status_quad)) {
                    layers[end.layerCount++] = reinterpret_cast<const XrCompositionLayerBaseHeader*>(&status_quad);
                    end.layers = layers.data();
                }
                // System dialogs and other ImGui layers, on top of everything.
                XrCompositionLayerQuad layers_quad{XR_TYPE_COMPOSITION_LAYER_QUAD};
                if (!panel && frame.shouldRender && layer_panel && !layer_panel_failed &&
                    end.layerCount < layers.size() && layer_panel->Fill(local, layers_quad)) {
                    layers[end.layerCount++] = reinterpret_cast<const XrCompositionLayerBaseHeader*>(&layers_quad);
                    end.layers = layers.data();
                }
                CheckDeviceAlive();
                CheckXr(QueueCall(EndFrame, session, &end), "xrEndFrame");
                status_panel_lock.unlock();
#if defined(SHADPS4_HAS_SCRCPY_CAPTURE_SDK)
                if (capturing) xr_capture->Compose(eyes);
#endif
                if (panel && end.layerCount) ++ui_frames;
                if (panel && end.layerCount && !panel_submitted) {
                    LOG_INFO(Render_Vulkan, "OpenXR ImGui error panel submitted: {}", error_detail);
                    panel_submitted = true;
                }
            }
        } catch (const std::exception& e) {
            { std::scoped_lock lock(failure_mutex); failure = e.what(); }
            failed = true;
            LOG_ERROR(Render_Vulkan, "OpenXR stopped: {}", e.what());
            __android_log_print(ANDROID_LOG_ERROR, "ShadOpenXR", "OpenXR stopped: %s", e.what());
            std::scoped_lock lock(failure_handler_mutex);
            if (failure_handler)
                failure_handler(e.what());
        }
        ReleaseInput();
    }
    void ShutdownSession() {
        if (pump.joinable()) {
            pump.request_stop();
            if (session && RequestExitSession)
                RequestExitSession(session);
            pump.join();
        }
        ReleaseInput();
        if (generation)
            Sensor::Instance().EndOpenXr(generation);
        generation = 0;
        // Session destruction releases runtime-owned swapchain images; all our
        // queue submissions completed before pump.join / Publish returned.
        input.reset();
        gaze_tracker.reset();
        hand_tracker.reset();
        {
            std::scoped_lock lock(status_panel_mutex);
            status_panel.reset();
            layer_panel.reset();
        }
        if (hosting_layers) {
            ImGui::Core::SetExternalLayerHost(false);
            hosting_layers = false;
        }
        std::unique_lock<std::mutex> queue_lock;
        if (vk) {
            try {
                vk->DrainSubmissions();
            } catch (const std::exception&) {
            }
            queue_lock = std::unique_lock(vk->QueueMutex());
            const auto result = VKF(DeviceWaitIdle)(vk->GetDevice());
            if (result != VK_SUCCESS)
                LOG_ERROR(Render_Vulkan, "OpenXR teardown device drain: {}", int(result));
        }
        if (swapchain)
            DestroySwapchain(swapchain);
        if (view)
            DestroySpace(view);
        if (local)
            DestroySpace(local);
        if (session)
            DestroySession(session);
        swapchain = {};
        view = {};
        local = {};
        session = {};
        if (vk) {
            if (mailbox)
                vmaDestroyImage(vk->GetAllocator(), mailbox, allocation);
            for (auto* c : {&producers[0], &producers[1], &producers[2], &consumer}) {
                if (c->fence)
                    VKF(DestroyFence)(vk->GetDevice(), c->fence, nullptr);
                if (c->pool)
                    VKF(DestroyCommandPool)(vk->GetDevice(), c->pool, nullptr);
                *c = {};
            }
        }
        mailbox = {};
        vk = nullptr;
        has_frame = false;
        running = false;
    }
    ~Impl() {
        ShutdownSession();
        if (xr && DestroyInstance)
            DestroyInstance(xr);
        if (activity_ref)
            if (auto env = Jni::getJniEnv())
                env->DeleteGlobalRef(activity_ref);
        if (loader)
            dlclose(loader);
        if (counted) --live_runtimes;
    }
};

Runtime::Runtime() : impl(std::make_unique<Impl>()) {}
Runtime::~Runtime() = default;
std::unique_ptr<Runtime> Runtime::Create() {
    std::scoped_lock creation_lock(runtime_creation_mutex);
    auto result = std::unique_ptr<Runtime>(new Runtime);
    {
        std::scoped_lock lock(activity_mutex);
        if (!activity)
            return {};
        auto env = Jni::getJniEnv();
        if (!env || !Jni::getJavaVM())
            throw std::runtime_error("OpenXR JNI environment unavailable");
        result->impl->activity_ref = env->NewGlobalRef(activity);
        if (!result->impl->activity_ref)
            throw std::runtime_error("OpenXR Activity reference failed");
    }
    result->impl->Init();
    return result;
}
std::array<EyeExtent, 3> Runtime::ProbeOutputExtents(void* environment, void* object,
                                                        DriverLease driver) {
    std::scoped_lock creation_lock(runtime_creation_mutex);
    const auto cached = ReadOutputLimits(driver);
    if (live_runtimes.load() && cached[0].width) return cached;
    if (live_runtimes.load() || !environment || !object || !driver || !driver->entry)
        throw std::runtime_error("XR resolution query unavailable while a runtime is active");
    auto* env = static_cast<JNIEnv*>(environment);
    Runtime runtime;
    runtime.impl->activity_ref = env->NewGlobalRef(static_cast<jobject>(object));
    if (!runtime.impl->activity_ref) throw std::runtime_error("XR query Activity reference failed");
    try { runtime.impl->Init(); }
    catch (...) {
        if (cached[0].width) return cached;
        throw;
    }
    const VkApplicationInfo app{VK_STRUCTURE_TYPE_APPLICATION_INFO, nullptr, "shadPS4 XR query",
                               1, "shadPS4", 1, VK_API_VERSION_1_3};
    const VkInstanceCreateInfo create{VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO, nullptr, 0, &app};
    const auto instance = runtime.CreateInstance(driver->entry, create);
    const auto destroy = reinterpret_cast<PFN_vkDestroyInstance>(driver->entry(instance, "vkDestroyInstance"));
    try {
        const auto properties = reinterpret_cast<PFN_vkGetPhysicalDeviceProperties>(
            driver->entry(instance, "vkGetPhysicalDeviceProperties"));
        if (!properties || !destroy) throw std::runtime_error("XR query Vulkan functions unavailable");
        VkPhysicalDeviceProperties gpu{};
        properties(runtime.PhysicalDevice(instance), &gpu);
        const auto limits = runtime.impl->QueryOutputLimits(gpu);
        SaveOutputLimits(driver, limits);
        std::array<EyeExtent, 3> result;
        for (uint32_t tier = 0; tier < result.size(); ++tier) {
            result[tier] = ChooseOutputExtent(limits[0], limits[1], tier);
            if (!result[tier].width || !result[tier].height)
                throw std::runtime_error("XR query returned invalid dimensions");
        }
        destroy(instance, nullptr);
        return result;
    } catch (...) {
        if (destroy) destroy(instance, nullptr);
        throw;
    }
}
VkInstance Runtime::CreateInstance(PFN_vkGetInstanceProcAddr entry,
                                   const VkInstanceCreateInfo& info) {
    auto version = info.pApplicationInfo->apiVersion;
    auto xr_version = XR_MAKE_VERSION(VK_VERSION_MAJOR(version), VK_VERSION_MINOR(version), 0);
    if (xr_version < impl->requirements.minApiVersionSupported ||
        xr_version > impl->requirements.maxApiVersionSupported)
        throw std::runtime_error("OpenXR runtime does not support shadPS4 Vulkan API version");
    XrVulkanInstanceCreateInfoKHR create{XR_TYPE_VULKAN_INSTANCE_CREATE_INFO_KHR};
    create.systemId = impl->system;
    create.pfnGetInstanceProcAddr = entry;
    create.vulkanCreateInfo = &info;
    VkInstance instance{};
    VkResult result{};
    CheckXr(impl->CreateVulkanInstanceKHR(impl->xr, &create, &instance, &result),
            "xrCreateVulkanInstanceKHR");
    CheckVk(result, "OpenXR Vulkan instance");
    return instance;
}
VkPhysicalDevice Runtime::PhysicalDevice(VkInstance instance) {
    XrVulkanGraphicsDeviceGetInfoKHR info{XR_TYPE_VULKAN_GRAPHICS_DEVICE_GET_INFO_KHR};
    info.systemId = impl->system;
    info.vulkanInstance = instance;
    VkPhysicalDevice physical{};
    CheckXr(impl->GetVulkanGraphicsDevice2KHR(impl->xr, &info, &physical),
            "xrGetVulkanGraphicsDevice2KHR");
    return physical;
}
VkDevice Runtime::CreateDevice(PFN_vkGetInstanceProcAddr entry, VkPhysicalDevice physical,
                               const VkDeviceCreateInfo& info) {
    XrVulkanDeviceCreateInfoKHR create{XR_TYPE_VULKAN_DEVICE_CREATE_INFO_KHR};
    create.systemId = impl->system;
    create.pfnGetInstanceProcAddr = entry;
    create.vulkanPhysicalDevice = physical;
    create.vulkanCreateInfo = &info;
    VkDevice device{};
    VkResult result{};
    CheckXr(impl->CreateVulkanDeviceKHR(impl->xr, &create, &device, &result),
            "xrCreateVulkanDeviceKHR");
    CheckVk(result, "OpenXR Vulkan device");
    return device;
}
void Runtime::Start(const Instance& instance) {
    impl->StartSession(instance);
}
void Runtime::StartError(const Instance& instance, std::string detail) {
    impl->error_detail = std::move(detail);
    impl->StartSession(instance);
}
int Runtime::ErrorAction() const {
    return impl->failed ? -1 : impl->error_action.load();
}
std::string Runtime::ErrorStatus() const {
    std::scoped_lock lock(impl->failure_mutex);
    return "XR error panel: frames=" + std::to_string(impl->ui_frames.load()) +
           " failed=" + std::to_string(impl->failed.load()) + " detail=" + impl->failure;
}
void Runtime::SetFailureHandler(std::function<void(const std::string&)> handler) {
    std::scoped_lock lock(impl->failure_handler_mutex);
    impl->failure_handler = std::move(handler);
}
std::string Runtime::GazeStatus() const {
    std::scoped_lock lock(impl->gaze_mutex);
    return fmt::format("gaze_extension={} gaze_supported={} active={} valid_frames={} fallback_frames={}\n",
        impl->gaze_extension,impl->gaze_supported,impl->gaze_sample.valid,
        impl->gaze_valid_count,impl->gaze_invalid_count);
}
std::array<HostPasses::FoveatedEye,2> Runtime::Foveation(
    const std::array<Sensor::Pose,2>& rendered_eyes,
    const std::array<std::array<float,4>,2>& fov,bool perspective) {
    using namespace spatial::foveation;
    const auto options=HostPasses::GetSpatialOptions();
    spatial::xr::EyeGazeSample sample;uint64_t received, valid, invalid;
    {std::scoped_lock lock(impl->gaze_mutex);sample=impl->gaze_sample;received=impl->gaze_received;
     valid=impl->gaze_valid_count;invalid=impl->gaze_invalid_count;}
    const uint64_t now=NowNs();
    const bool track=options.foveation==Mode::EyeTracked && sample.valid && received &&
                     now>=received && now-received<=100'000'000;
    HostPasses::PublishGazeStatus(impl->gaze_supported,track,valid,invalid);
    std::array<HostPasses::FoveatedEye,2> result{};
    const auto direction=spatial::xr::math::Rotate(sample.pose.orientation,XrVector3f{0,0,-1});
    for(unsigned eye=0;eye<2;++eye) {
        auto& target=result[eye];
        target.enabled=options.foveation!=Mode::Off;
        target.profile=ProfileForLevel(options.level);
        if(perspective && Core::HostRuntime::VrGeometry::ValidFov(fov[eye])) {
            const auto& angles=fov[eye];
            const ViewFrustum frustum{std::tan(angles[0]),std::tan(angles[1]),std::tan(angles[3]),std::tan(angles[2])};
            target.profile=ProfileForLevel(options.level,frustum);
            if(track && rendered_eyes[eye].orientation_valid) {
                const auto view_direction=spatial::xr::math::Rotate(
                    spatial::xr::math::Conjugate(XrPose(rendered_eyes[eye]).orientation),direction);
                const auto ndc=ProjectGazeToViewNdc({view_direction.x,view_direction.y,view_direction.z},frustum);
                target.center=MapViewNdcToAtlasUv(ndc,{0,0,1,1},1,1);
                target.tracked=ndc.valid;
            }
        } else if(track) {
            // A cinema image needs ray/plane intersection, not a headset-FOV UV.
            std::scoped_lock lock(impl->mailbox_mutex);
            if(impl->cinema_anchored) {
                const auto inv=spatial::xr::math::Conjugate(impl->cinema_pose.orientation);
                const auto d=spatial::xr::math::Rotate(inv,direction);
                const auto origin=spatial::xr::math::Rotate(inv,XrVector3f{
                    sample.pose.position.x-impl->cinema_pose.position.x,
                    sample.pose.position.y-impl->cinema_pose.position.y,
                    sample.pose.position.z-impl->cinema_pose.position.z});
                const float t=std::abs(d.z)>1e-5f?-origin.z/d.z:-1.f;
                if(t>0) {
                    const float u=.5f+(origin.x+t*d.x)/Core::HostRuntime::VrGeometry::CinemaWidth;
                    const float v=.5f-(origin.y+t*d.y)/Core::HostRuntime::VrGeometry::CinemaHeight(impl->display_aspect);
                    target.center={std::clamp(u,0.f,1.f),std::clamp(v,0.f,1.f)};
                    target.tracked=std::isfinite(u)&&std::isfinite(v);
                }
            }
        }
    }
    return result;
}
void Runtime::Stop() {
    impl->ShutdownSession();
}
VkExtent2D Runtime::FrameExtent() const {
    return {impl->width, impl->height};
}
bool Runtime::Publish(const Frame& frame, VkFormat format, VkFence done) {
    auto& p = *impl;
    if (p.failed || !p.vk || !p.mailbox || !frame.width || !frame.height)
        return false;
    if (frame.is_hdr)
        throw std::runtime_error("OpenXR cinema requires SDR output; HDR tone mapping is not implemented");
    // The timeline signal must already be enqueued before submitting a wait on the
    // same queue, otherwise the worker could enqueue its producer behind our wait.
    p.vk->DrainSubmissions();
    std::scoped_lock lock(p.mailbox_mutex);
    VkFormatProperties properties{};
    VKF(GetPhysicalDeviceFormatProperties)(p.vk->GetPhysicalDevice(), format, &properties);
    if (!(properties.optimalTilingFeatures & VK_FORMAT_FEATURE_BLIT_SRC_BIT))
        throw std::runtime_error("OpenXR source format cannot be blitted");
    auto& producer = p.producers[p.next_producer];
    p.next_producer = (p.next_producer + 1) % p.producers.size();
    p.BeginCopy(producer); // waits only for the copy three publications ago
    p.Barrier(producer.command, frame.image, VK_IMAGE_LAYOUT_GENERAL,
              VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL);
    p.Barrier(producer.command, p.mailbox,
              p.has_frame ? VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL : VK_IMAGE_LAYOUT_UNDEFINED,
              VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL);
    const auto mailbox_format = p.format == VK_FORMAT_R8G8B8A8_SRGB
        ? VK_FORMAT_R8G8B8A8_UNORM : VK_FORMAT_B8G8R8A8_UNORM;
    // A smaller frame (cinema, aspect-fitted) is stored at its own size: stretching
    // it over the whole SBS extent wrote up to 3.3x the pixels, which the compositor
    // then resampled again for the quad.
    const bool fits = frame.width <= p.width && frame.height <= p.height;
    const uint32_t content_width = fits ? frame.width : p.width;
    const uint32_t content_height = fits ? frame.height : p.height;
    if (fits && format == mailbox_format) {
        VkImageCopy copy{};
        copy.srcSubresource = copy.dstSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1};
        copy.extent = {content_width, content_height, 1};
        VKF(CmdCopyImage)(producer.command, frame.image, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
                         p.mailbox, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &copy);
    } else {
        // A format conversion (1:1 when the frame fits), or a frame larger than
        // the mailbox, filtered down. PSVR composes at FrameExtent().
        VkImageBlit blit{};
        blit.srcSubresource = blit.dstSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1};
        blit.srcOffsets[1] = {int32_t(frame.width), int32_t(frame.height), 1};
        blit.dstOffsets[1] = {int32_t(content_width), int32_t(content_height), 1};
        const auto filter = properties.optimalTilingFeatures & VK_FORMAT_FEATURE_SAMPLED_IMAGE_FILTER_LINEAR_BIT
            ? VK_FILTER_LINEAR : VK_FILTER_NEAREST;
        VKF(CmdBlitImage)(producer.command, frame.image, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
                         p.mailbox, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &blit, filter);
    }
    p.Barrier(producer.command, frame.image, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
              VK_IMAGE_LAYOUT_GENERAL);
    p.Barrier(producer.command, p.mailbox, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
              VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL);
    if (done)
        CheckVk(VKF(ResetFences)(p.vk->GetDevice(), 1, &done), "XR frame fence reset");
    p.FinishCopy(producer, frame.ready_semaphore, frame.ready_tick, !done, done);
    p.stereo = frame.xr_stereo;
    p.render_eyes = frame.xr_render_eyes;
    p.render_fov = frame.xr_render_fov;
    p.perspective = frame.xr_perspective;
    p.immersive = p.stereo && p.perspective;
    p.display_aspect = frame.xr_display_aspect;
    p.content_width = content_width;
    p.content_height = content_height;
    p.has_frame = true;
    ++p.mailbox_serial;
    return true;
}
void Runtime::PublishStatus(spatial::imgui::overlay::StatusSnapshot status,
                            std::optional<spatial::perf::DeviceMetrics> device) {
    auto& p = *impl;
    { std::scoped_lock lock(p.status_mutex);
      p.status_snapshot = status; p.status_device = std::move(device); }
    const auto config = ReadStatusSettings();
    if (!config.visible || !(config.psvr_title || p.immersive) || !p.session || p.failed)
        return;
    std::unique_lock lock(p.status_panel_mutex, std::try_to_lock);
    if (!lock || p.status_panel_failed) return;
    try {
        static thread_local Impl* owner{};
        owner = &p;
        if (!p.status_panel) {
            p.status_panel = std::make_unique<StatusPanel>(config.layout);
            const spatial::xr::SwapchainFunctions functions{
                p.CreateSwapchain, p.DestroySwapchain, p.EnumerateSwapchainFormats, p.EnumerateSwapchainImages,
                +[](XrSwapchain chain, const XrSwapchainImageAcquireInfo* info, uint32_t* index) {
                    return owner->QueueCall(owner->AcquireSwapchainImage, chain, info, index);
                }, p.WaitSwapchainImage,
                +[](XrSwapchain chain, const XrSwapchainImageReleaseInfo* info) {
                    return owner->QueueCall(owner->ReleaseSwapchainImage, chain, info);
                }};
            const spatial::xr::XrImguiVulkanBinding binding{
                .api_version=VK_API_VERSION_1_3, .instance=p.vk->GetInstance(),
                .physical_device=p.vk->GetPhysicalDevice(), .device=p.vk->GetDevice(),
                .queue=p.vk->GetGraphicsQueue(), .queue_family_index=p.vk->GetGraphicsQueueFamilyIndex(),
                .vk_get_instance_proc_addr=VKF(GetInstanceProcAddr), .vk_get_device_proc_addr=VKF(GetDeviceProcAddr),
                .queue_submit_mutex=&p.vk->QueueMutex()};
            if (!p.status_panel->Create(p.session, functions, binding))
                throw std::runtime_error("XR status ImGui swapchain creation failed");
        }
        p.status_panel->Update(status, p.width / 2, p.height, config.theme);
    } catch (const std::exception& e) {
        p.status_panel_failed = true;
        ReportStatusLayer(std::string("PSVR status layer disabled: ") + e.what());
        LOG_ERROR(Render_Vulkan, "Status layer: {}", e.what());
    }
}
void Runtime::UpdateLayers() {
    auto& p = *impl;
    if (!p.hosting_layers || !p.session || p.failed) return;
    // Like the status panel: never wait for the XR pump's composition.
    std::unique_lock lock(p.status_panel_mutex, std::try_to_lock);
    if (!lock || p.layer_panel_failed) return;
    try {
        static thread_local Impl* owner{};
        owner = &p;
        if (!p.layer_panel) {
            p.layer_panel = std::make_unique<LayerPanel>();
            const spatial::xr::SwapchainFunctions functions{
                p.CreateSwapchain, p.DestroySwapchain, p.EnumerateSwapchainFormats, p.EnumerateSwapchainImages,
                +[](XrSwapchain chain, const XrSwapchainImageAcquireInfo* info, uint32_t* index) {
                    return owner->QueueCall(owner->AcquireSwapchainImage, chain, info, index);
                }, p.WaitSwapchainImage,
                +[](XrSwapchain chain, const XrSwapchainImageReleaseInfo* info) {
                    return owner->QueueCall(owner->ReleaseSwapchainImage, chain, info);
                }};
            const spatial::xr::XrImguiVulkanBinding binding{
                .api_version=VK_API_VERSION_1_3, .instance=p.vk->GetInstance(),
                .physical_device=p.vk->GetPhysicalDevice(), .device=p.vk->GetDevice(),
                .queue=p.vk->GetGraphicsQueue(), .queue_family_index=p.vk->GetGraphicsQueueFamilyIndex(),
                .vk_get_instance_proc_addr=VKF(GetInstanceProcAddr), .vk_get_device_proc_addr=VKF(GetDeviceProcAddr),
                .queue_submit_mutex=&p.vk->QueueMutex()};
            if (!p.layer_panel->Create(p.session, functions, binding))
                throw std::runtime_error("XR layer panel swapchain creation failed");
        }
        p.layer_panel->Update(Sensor::Instance().Read().hardware);
    } catch (const std::exception& e) {
        // Dialogs go back to the window frame rather than nowhere.
        p.layer_panel_failed = true;
        ImGui::Core::SetExternalLayerHost(false);
        p.hosting_layers = false;
        LOG_ERROR(Render_Vulkan, "XR layer panel disabled: {}", e.what());
    }
}
} // namespace Vulkan::OpenXr
