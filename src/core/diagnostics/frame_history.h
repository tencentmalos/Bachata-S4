// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once
#include <algorithm>
#include <array>
#include <cstdint>

namespace Core::Diagnostics {
// Owned by one Presenter/thread. Successful presents only; never sample ImGui FPS.
class FrameHistory {
public:
    struct Sample { uint64_t end_ns{}; float milliseconds{}; };
    static constexpr size_t Capacity = 1024;
    void Record(uint64_t now) {
        if (!started) { first = previous = now; started = true; return; }
        if (now <= previous) return;
        samples[next] = {now, static_cast<float>((now - previous) / 1e6)};
        next = (next + 1) % Capacity;
        count = std::min(count + 1, Capacity);
        previous = now;
    }
    double Fps(uint64_t now) const {
        if (!started || now <= first) return 0;
        constexpr uint64_t Second = 1000000000;
        const auto duration = std::min(now - first, Second);
        size_t observed{};
        for (size_t i = 0; i < count; ++i) {
            const auto end = At(i).end_ns;
            if (end <= now && now - end < duration) ++observed;
        }
        return observed * 1e9 / duration;
    }
    size_t Count() const { return count; }
    const Sample& At(size_t ordered_index) const {
        return samples[(next + Capacity - count + ordered_index) % Capacity];
    }
private:
    std::array<Sample, Capacity> samples{};
    size_t next{}, count{};
    uint64_t first{}, previous{};
    bool started{};
};
} // namespace Core::Diagnostics
