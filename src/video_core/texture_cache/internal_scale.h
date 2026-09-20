// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once

#include <algorithm>
#include <bit>
#include <cstdint>

namespace VideoCore {

// Guest layouts always keep their original dimensions. Only host allocations use this policy.
struct InternalScale {
    uint32_t eighths = 8;

    static constexpr InternalScale FromPercent(float percent) {
        return {percent == 25 ? 2u : percent == 37.5f ? 3u :
                percent == 50 ? 4u : percent == 75 ? 6u : 8u};
    }
    constexpr uint32_t MipDrop() const { return eighths == 2 ? 2 : eighths == 4 ? 1 : 0; }
    constexpr uint32_t Size(uint32_t size) const {
        return std::max(1u, static_cast<uint32_t>(uint64_t{size} * eighths / 8));
    }
    constexpr float Factor() const { return float(eighths) / 8.f; }
    constexpr uint32_t Levels(uint32_t width, uint32_t height, uint32_t levels) const {
        return std::min<uint32_t>(levels, std::bit_width(std::max(Size(width), Size(height))));
    }
};

} // namespace VideoCore
