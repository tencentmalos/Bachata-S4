// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once
#include <algorithm>
#include <cstdint>

namespace Vulkan::OpenXr {
struct EyeExtent {
    uint32_t width{}, height{};
};
// Pixel density may differ by axis; OpenXR FOV defines projection, not the buffer aspect ratio.
// The caller intersects both view limits, SBS swapchain limits and Vulkan limits.
inline EyeExtent ChooseOutputExtent(EyeExtent recommended, EyeExtent limit, uint32_t tier) {
    if (!recommended.width || !recommended.height || !limit.width || !limit.height || tier > 2)
        return {};
    const auto dimension = [tier](uint32_t suggested, uint32_t maximum) {
        const auto base = std::min(suggested, maximum);
        return tier == 2 ? maximum : tier == 1 ? base + (maximum - base) / 2 : base;
    };
    return {dimension(recommended.width, limit.width), dimension(recommended.height, limit.height)};
}
} // namespace Vulkan::OpenXr
