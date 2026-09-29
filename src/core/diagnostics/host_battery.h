// SPDX-FileCopyrightText: Copyright 2026 shadPS4 Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once

#include <cstdint>
#include <optional>

namespace Core::Diagnostics {

/// Battery values supplied by the platform frontend. Android apps may not read
/// /sys/class/power_supply (SELinux), so the app reads BatteryManager and publishes the result
/// here for the status HUD. A field the platform cannot report stays empty rather than 0.
struct HostBattery {
    std::optional<float> level_percent;
    /// Sign as reported by the platform; consumers use the magnitude.
    std::optional<std::int64_t> current_microamps;
    std::optional<std::int64_t> voltage_microvolts;
    std::optional<std::int64_t> charge_microamp_hours;
    std::optional<float> temperature_celsius;
    std::optional<bool> charging;
};

/// Publishes the latest reading; nullopt withdraws it (readers fall back to their own sources).
/// Thread-safe, process-wide.
void PublishHostBattery(std::optional<HostBattery> battery);

/// Copies the published reading into `battery` and returns true when it changed since the
/// caller's `generation` (updated in place); returns false otherwise.
bool ReadHostBattery(std::uint64_t& generation, std::optional<HostBattery>& battery);

} // namespace Core::Diagnostics
