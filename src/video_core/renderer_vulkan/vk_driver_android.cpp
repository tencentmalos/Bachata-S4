// SPDX-License-Identifier: GPL-2.0-or-later
#include "video_core/renderer_vulkan/vk_driver.h"

#include <array>
#include <filesystem>
#include <fstream>
#include <mutex>
#include <stdexcept>
#include <vector>
#include <dlfcn.h>
#include <adrenotools/driver.h>
#include <fmt/format.h>
#include <openssl/sha.h>

namespace Vulkan {
namespace {
constexpr auto DriverFile = "vulkan.ad07xx.so";
constexpr auto DriverSha = "fdd378520022f88b0363dd1f77f6989332730271712621523075fe4eb4de2a09";

std::string Directory(const std::string& path) {
    if (!std::filesystem::path(path).is_absolute() || !std::filesystem::is_directory(path))
        throw std::runtime_error("Turnip requires an existing absolute directory: " + path);
    return std::filesystem::canonical(path).string() + "/";
}

void VerifyFile(const std::string& path) {
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
    if (hex != DriverSha) throw std::runtime_error("Pinned bionic Turnip SHA256 mismatch");
}

template <typename T>
T Entry(PFN_vkGetInstanceProcAddr entry, VkInstance instance, const char* name) {
    auto function = reinterpret_cast<T>(entry(instance, name));
    if (!function) throw std::runtime_error(std::string("Turnip missing entry: ") + name);
    return function;
}

std::string Identify(PFN_vkGetInstanceProcAddr entry) {
    const VkApplicationInfo app{VK_STRUCTURE_TYPE_APPLICATION_INFO, nullptr, "shadPS4", 1,
                                "shadPS4", 1, VK_API_VERSION_1_3};
    const VkInstanceCreateInfo info{VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO, nullptr, 0, &app};
    VkInstance instance{};
    auto create = Entry<PFN_vkCreateInstance>(entry, nullptr, "vkCreateInstance");
    if (auto result = create(&info, nullptr, &instance); result != VK_SUCCESS)
        throw std::runtime_error(fmt::format("Turnip vkCreateInstance failed: {}", int(result)));
    struct Cleanup {
        VkInstance instance;
        PFN_vkDestroyInstance destroy;
        ~Cleanup() { destroy(instance, nullptr); }
    } cleanup{instance, Entry<PFN_vkDestroyInstance>(entry, instance, "vkDestroyInstance")};
    auto enumerate = Entry<PFN_vkEnumeratePhysicalDevices>(entry, instance, "vkEnumeratePhysicalDevices");
    uint32_t count{};
    if (enumerate(instance, &count, nullptr) != VK_SUCCESS || count == 0)
        throw std::runtime_error("Turnip exposes no physical device");
    std::vector<VkPhysicalDevice> devices(count);
    if (enumerate(instance, &count, devices.data()) != VK_SUCCESS)
        throw std::runtime_error("Turnip physical-device enumeration failed");
    if (count == 0)
        throw std::runtime_error("Turnip physical device disappeared");
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
        if (driver.driverID != VK_DRIVER_ID_MESA_TURNIP || !caps.shaderInt64 ||
            props.properties.apiVersion < VK_API_VERSION_1_3)
            throw std::runtime_error(fmt::format("Turnip profile rejected: driverID={} shaderInt64={} api={}",
                int(driver.driverID), caps.shaderInt64, props.properties.apiVersion));
        identity += fmt::format("device={} driver={} info={} shaderInt64=1 api={} sha256={} ",
            props.properties.deviceName, driver.driverName, driver.driverInfo,
            props.properties.apiVersion, DriverSha);
    }
    return identity;
}
} // namespace

DriverLease LoadAndroidTurnip(const std::string& hook_directory, const std::string& driver_directory) {
    const auto hooks = Directory(hook_directory);
    const auto files = Directory(driver_directory);
    VerifyFile(files + DriverFile);
    for (auto name : {"libhook_impl.so", "libmain_hook.so", "libfile_redirect_hook.so", "libgsl_alloc_hook.so"})
        if (!std::filesystem::is_regular_file(hooks + name))
            throw std::runtime_error(std::string("Missing adrenotools hook: ") + name);

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
        void* handle = adrenotools_open_libvulkan(RTLD_NOW, ADRENOTOOLS_DRIVER_CUSTOM, nullptr,
            hooks.c_str(), files.c_str(), DriverFile, nullptr, nullptr);
        if (!handle) throw std::runtime_error("adrenotools could not load pinned bionic Turnip");
        auto entry = reinterpret_cast<PFN_vkGetInstanceProcAddr>(dlsym(handle, "vkGetInstanceProcAddr"));
        if (!entry) throw std::runtime_error("Turnip loader has no vkGetInstanceProcAddr");
        // Deliberately keep handle mapped, matching the namespace/hook lifetime.
        pinned = std::make_shared<Driver>(Driver{entry, Identify(entry)});
        return pinned;
    } catch (const std::exception& e) {
        failure = e.what();
        throw;
    }
}
} // namespace Vulkan
