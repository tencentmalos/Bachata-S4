// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once

#include <memory>
#include <string>
#include "video_core/renderer_vulkan/vk_common.h"

namespace Vulkan {
// The loader and its namespace outlive every instance/device using this entry.
// Android has no implicit system-driver fallback.
struct Driver final {
    PFN_vkGetInstanceProcAddr entry{};
    std::string identity;
};
using DriverLease = std::shared_ptr<const Driver>;

#if defined(__ANDROID__)
DriverLease LoadAndroidTurnip(const std::string& hook_directory,
                              const std::string& driver_directory);
#endif
} // namespace Vulkan
