// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once

#include <algorithm>
#include <bit>
#include <cstdint>

namespace VideoCore {

// Guest layouts always keep their original dimensions. Only host allocations use this policy.
struct InternalScale {
    uint32_t quarters = 4;

    static constexpr InternalScale FromPercent(uint32_t percent) {
        return {percent == 50 ? 2u : percent == 75 ? 3u : 4u};
    }
    constexpr uint32_t Size(uint32_t size) const {
        return std::max(1u, static_cast<uint32_t>(uint64_t{size} * quarters / 4));
    }
    constexpr float Factor() const { return float(quarters) / 4.f; }
    constexpr uint32_t Levels(uint32_t width, uint32_t height, uint32_t levels) const {
        return std::min<uint32_t>(levels, std::bit_width(std::max(Size(width), Size(height))));
    }
};

} // namespace VideoCore
