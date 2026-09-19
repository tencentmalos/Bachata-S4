// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once
#include <chrono>

namespace Libraries::VideoOut {
// Optional UI redraws must not amplify vblank catch-up after a slow present.
// Guest flips, vblank events and buffer-label retirement are not rate limited.
class RedrawPacer {
public:
    using Clock = std::chrono::steady_clock;
    explicit RedrawPacer(std::chrono::nanoseconds period) : period(period) {}
    bool TryAcquire(Clock::time_point now, std::chrono::nanoseconds time_credit) {
        if (time_credit < -period || now < next) return false;
        next = now + period;
        return true;
    }
private:
    const std::chrono::nanoseconds period;
    Clock::time_point next{};
};
} // namespace Libraries::VideoOut
