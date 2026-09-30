// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once
#include <algorithm>
#include <array>
#include <cmath>
#include <string_view>

namespace Vulkan::OpenXr {
// PCH-1000 PS-key states, per Sony's partnames/power/charge manuals. Blink periods
// are presentation choices: Sony describes fast/slow blinking, not exact timing.
enum class PsvIndicator { Off, Running, Standby, Charging, ChargeLow, Notification };
inline constexpr std::array<std::string_view, 6> PsvIndicatorNames{
    "off", "running", "standby", "charging", "charge_low", "notification"};
struct PsvIndicatorLight {
    std::array<float, 3> colour{};
    float brightness{};
};
inline PsvIndicatorLight IndicatorLight(PsvIndicator state, double seconds) {
    constexpr std::array<float, 3> blue{.005f, .18f, 1.f}, orange{1.f, .18f, .006f};
    switch (state) {
    case PsvIndicator::Running: return {blue, 1};
    case PsvIndicator::Charging: return {orange, 1};
    case PsvIndicator::Standby:
    case PsvIndicator::ChargeLow:
        return {state == PsvIndicator::Standby ? blue : orange,
                std::fmod(std::max(seconds, 0.0), .4) < .2 ? 1.f : 0.f};
    case PsvIndicator::Notification: {
        const auto phase = std::fmod(std::max(seconds, 0.0), 2.4) / 2.4;
        const auto wave = .5 - .5 * std::cos(phase * 6.283185307179586);
        return {blue, static_cast<float>(wave * wave)};
    }
    default: return {};
    }
}
} // namespace Vulkan::OpenXr
