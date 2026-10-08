// SPDX-License-Identifier: GPL-2.0-or-later
#include "video_core/renderer_vulkan/vk_driver.h"
#include "turnip_identity.h"

#include <array>
#include <filesystem>
#include <fstream>
#include <mutex>
#include <stdexcept>
#include <vector>
#include <cstdlib>
#include <dlfcn.h>
#include <sys/system_properties.h>
#include <adrenotools/driver.h>
#include <fmt/format.h>
#include <openssl/sha.h>
#include "common/logging/log.h"

namespace Vulkan {
namespace {
constexpr auto DriverFile = "vulkan.ad07xx.so";
constexpr auto DriverSha = "fdd378520022f88b0363dd1f77f6989332730271712621523075fe4eb4de2a09";
constexpr auto MainlineDriverSha = TurnipBuild::Sha256;

void SelectLoader(bool system) {
    static std::mutex mutex;
    static int selected = -1;
    std::scoped_lock lock(mutex);
    const int requested = system ? 1 : 0;
    if (selected != -1 && selected != requested)
        throw std::runtime_error("Changing Vulkan driver requires app restart");
    selected = requested;
}

std::string Directory(const std::string& path) {
    if (!std::filesystem::path(path).is_absolute() || !std::filesystem::is_directory(path))
        throw std::runtime_error("Turnip requires an existing absolute directory: " + path);
    return std::filesystem::canonical(path).string() + "/";
}

std::string VerifyFile(const std::string& path) {
    std::ifstream input(path, std::ios::binary);
    if (!input) throw std::runtime_error("Missing pinned bionic Turnip: " + path);
    SHA256_CTX hash{};
    SHA256_Init(&hash);
    std::array<char, 65536> buffer{};
    while (input.read(buffer.data(), buffer.size()) || input.gcount())
        SHA256_Update(&hash, buffer.data(), input.gcount());
    if (!input.eof()) throw std::runtime_error("Cannot read Turnip ELF");
    std::array<unsigned char, SHA256_DIGEST_LENGTH> digest{};
    SHA256_Final(digest.data(), &hash);
    std::string hex;
    for (auto byte : digest) hex += fmt::format("{:02x}", byte);
    if (hex != DriverSha && hex != MainlineDriverSha)
        throw std::runtime_error("Pinned bionic Turnip SHA256 mismatch");
    return hex;
}

// Turnip sizes the binning pass's visibility streams (VSC) at 16 KiB of primitive and 4 KiB of
// draw data per pipe and grows them only for command buffers recorded after it saw an overflow.
// On A8xx the overflowing pass itself hangs the GPU: the PC stalls on the truncated stream.
// ASTRO BOT's 1440x1536 MSAA scene pass did so about 33 s into Swan sessions (KGSL "Fault id:2",
// BR stopped mid-bin with PC busy, all four snapshots still at the initial sizes); TU_DEBUG
// nobin, sysmem and gmem_warmup each avoided it. gmem_warmup starts the streams at 512 KiB and
// 16 KiB per pipe. Turnip reads TU_DEBUG from the environment before debug.mesa.tu.debug, so the
// flag joins whatever that property holds; debug.shadps4.vsc_warmup=0 leaves TU_DEBUG alone.
void PrepareTurnipDebugFlags() {
    char value[PROP_VALUE_MAX]{};
    if (__system_property_get("debug.shadps4.vsc_warmup", value) > 0 && value[0] == '0')
        return;
    std::string flags;
    if (const char* env = std::getenv("TU_DEBUG"))
        flags = env;
    else if (__system_property_get("debug.mesa.tu.debug", value) > 0)
        flags = value;
    if (flags.find("gmem_warmup") != std::string::npos)
        return;
    if (!flags.empty())
        flags += ',';
    flags += "gmem_warmup";
    setenv("TU_DEBUG", flags.c_str(), 1);
    LOG_INFO(Render_Vulkan, "Turnip TU_DEBUG={} (large initial VSC streams)", flags);
}

template <typename T>
T Entry(PFN_vkGetInstanceProcAddr entry, VkInstance instance, const char* name) {
    auto function = reinterpret_cast<T>(entry(instance, name));
    if (!function) throw std::runtime_error(std::string("Vulkan loader missing entry: ") + name);
    return function;
}

std::string Identify(PFN_vkGetInstanceProcAddr entry, bool turnip,
                     std::string_view sha = "system-image") {
    const VkApplicationInfo app{VK_STRUCTURE_TYPE_APPLICATION_INFO, nullptr, "shadPS4", 1,
                                "shadPS4", 1, VK_API_VERSION_1_3};
    const VkInstanceCreateInfo info{VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO, nullptr, 0, &app};
    VkInstance instance{};
    auto create = Entry<PFN_vkCreateInstance>(entry, nullptr, "vkCreateInstance");
    if (auto result = create(&info, nullptr, &instance); result != VK_SUCCESS)
        throw std::runtime_error(fmt::format("Vulkan vkCreateInstance failed: {}", int(result)));
    struct Cleanup {
        VkInstance instance;
        PFN_vkDestroyInstance destroy;
        ~Cleanup() { destroy(instance, nullptr); }
    } cleanup{instance, Entry<PFN_vkDestroyInstance>(entry, instance, "vkDestroyInstance")};
    auto enumerate = Entry<PFN_vkEnumeratePhysicalDevices>(entry, instance, "vkEnumeratePhysicalDevices");
    uint32_t count{};
    if (enumerate(instance, &count, nullptr) != VK_SUCCESS || count == 0)
        throw std::runtime_error("Vulkan loader exposes no physical device");
    std::vector<VkPhysicalDevice> devices(count);
    if (enumerate(instance, &count, devices.data()) != VK_SUCCESS)
        throw std::runtime_error("Vulkan physical-device enumeration failed");
    if (count == 0)
        throw std::runtime_error("Vulkan physical device disappeared");
    devices.resize(count);
    auto properties = Entry<PFN_vkGetPhysicalDeviceProperties2>(entry, instance, "vkGetPhysicalDeviceProperties2");
    auto features = Entry<PFN_vkGetPhysicalDeviceFeatures>(entry, instance, "vkGetPhysicalDeviceFeatures");
    // All devices must satisfy the profile, so subsequent GPU selection cannot
    // accidentally select an unverified device or a system-driver fallback.
    std::string identity;
    for (auto device : devices) {
        VkPhysicalDeviceDriverProperties driver{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_DRIVER_PROPERTIES};
        VkPhysicalDeviceProperties2 props{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_PROPERTIES_2, &driver};
        VkPhysicalDeviceFeatures caps{};
        properties(device, &props);
        features(device, &caps);
        if (turnip && (driver.driverID != VK_DRIVER_ID_MESA_TURNIP || !caps.shaderInt64 ||
            props.properties.apiVersion < VK_API_VERSION_1_3))
            throw std::runtime_error(fmt::format("Turnip profile rejected: driverID={} shaderInt64={} api={}",
                int(driver.driverID), caps.shaderInt64, props.properties.apiVersion));
        identity += fmt::format("source={} device={} driver={} info={} shaderInt64={} api={} sha256={} ",
            turnip ? "turnip" : "system",
            props.properties.deviceName, driver.driverName, driver.driverInfo,
            caps.shaderInt64, props.properties.apiVersion, sha);
    }
    return identity;
}
} // namespace

DriverLease LoadAndroidTurnip(const std::string& hook_directory, const std::string& driver_directory) {
    const auto hooks = Directory(hook_directory);
    const auto files = Directory(driver_directory);
    const auto sha = VerifyFile(files + DriverFile);
    for (auto name : {"libhook_impl.so", "libmain_hook.so", "libfile_redirect_hook.so", "libgsl_alloc_hook.so"})
        if (!std::filesystem::is_regular_file(hooks + name))
            throw std::runtime_error(std::string("Missing adrenotools hook: ") + name);
    SelectLoader(false);

    // adrenotools owns namespaces/HookImplParams for process lifetime and offers
    // no namespace teardown. Load at most once per process; do not leak one
    // namespace per game restart. A failed native load also requires app restart.
    static std::mutex mutex;
    static bool attempted{};
    static std::string selected;
    static std::string failure;
    static DriverLease pinned;
    std::scoped_lock lock(mutex);
    const auto key = hooks + "\n" + files;
    if (attempted) {
        if (selected != key) throw std::runtime_error("Turnip profile change requires app restart");
        if (!pinned) throw std::runtime_error(failure);
        return pinned;
    }
    attempted = true;
    selected = key;
    try {
        PrepareTurnipDebugFlags();
        void* handle = adrenotools_open_libvulkan(RTLD_NOW, ADRENOTOOLS_DRIVER_CUSTOM, nullptr,
            hooks.c_str(), files.c_str(), DriverFile, nullptr, nullptr);
        if (!handle) throw std::runtime_error("adrenotools could not load pinned bionic Turnip");
        auto entry = reinterpret_cast<PFN_vkGetInstanceProcAddr>(dlsym(handle, "vkGetInstanceProcAddr"));
        if (!entry) throw std::runtime_error("Turnip loader has no vkGetInstanceProcAddr");
        // Deliberately keep handle mapped, matching the namespace/hook lifetime.
        pinned = std::make_shared<Driver>(Driver{entry, Identify(entry, true, sha)});
        return pinned;
    } catch (const std::exception& e) {
        failure = e.what();
        throw;
    }
}

DriverLease LoadAndroidSystemDriver() {
    SelectLoader(true);
    // Keep the system loader mapped for the process lifetime as well. This path
    // does not call adrenotools or install any custom ICD redirection.
    static const DriverLease system = [] {
        void* handle = dlopen("libvulkan.so", RTLD_NOW | RTLD_LOCAL);
        if (!handle) throw std::runtime_error("Cannot open Android system Vulkan loader");
        auto entry = reinterpret_cast<PFN_vkGetInstanceProcAddr>(dlsym(handle, "vkGetInstanceProcAddr"));
        if (!entry) throw std::runtime_error("System Vulkan loader has no vkGetInstanceProcAddr");
        return std::make_shared<Driver>(Driver{entry, Identify(entry, false)});
    }();
    return system;
}
} // namespace Vulkan
