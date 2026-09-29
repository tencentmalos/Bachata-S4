// SPDX-FileCopyrightText: Copyright 2026 shadPS4 Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#include <mutex>
#include "core/diagnostics/host_battery.h"

namespace Core::Diagnostics {
namespace {
std::mutex battery_mutex;
std::optional<HostBattery> battery_value;
std::uint64_t battery_generation{};
} // namespace

void PublishHostBattery(std::optional<HostBattery> battery) {
    std::scoped_lock lock{battery_mutex};
    battery_value = std::move(battery);
    ++battery_generation;
}

bool ReadHostBattery(std::uint64_t& generation, std::optional<HostBattery>& battery) {
    std::scoped_lock lock{battery_mutex};
    if (generation == battery_generation) {
        return false;
    }
    generation = battery_generation;
    battery = battery_value;
    return true;
}

} // namespace Core::Diagnostics
