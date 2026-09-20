// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once

#include "common/types.h"

namespace Common {

// Match FEX SMALLTSCSCALE: scale both guest ticks and their advertised frequency.
// Host deadlines, profiler timestamps and conversions to seconds stay unscaled.
class FexTscScale final {
public:
    static constexpr u64 MinimumFrequency = 1'000'000'000;

    explicit constexpr FexTscScale(u64 host_frequency) : frequency{host_frequency} {
        while (frequency && frequency < MinimumFrequency) {
            frequency <<= 1;
            ++shift;
        }
    }

    constexpr u64 ToGuest(u64 host_ticks) const {
        return host_ticks << shift;
    }
    constexpr u64 Frequency() const {
        return frequency;
    }
    constexpr unsigned Shift() const {
        return shift;
    }

private:
    u64 frequency;
    unsigned shift{};
};

} // namespace Common
