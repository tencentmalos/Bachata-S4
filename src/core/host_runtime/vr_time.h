// SPDX-FileCopyrightText: Copyright 2026 shadPS4 Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#pragma once

#include <chrono>
#include <cstdint>

namespace Core::HostRuntime::VrTime {

// Host VR samples carry steady-clock (CLOCK_MONOTONIC) nanoseconds: OpenXR times converted with
// xrConvertTimeToTimespecTimeKHR, Android sensor events converted from CLOCK_BOOTTIME on arrival.
// Titles compare VrTracker and Move timestamps with sceVrTrackerGetTime and
// sceKernelGetProcessTime, process microseconds, so a sample keeps its age (or, for a predicted
// pose, its lead) relative to now in that clock.
inline std::uint64_t SteadyNowNs() {
    return static_cast<std::uint64_t>(std::chrono::duration_cast<std::chrono::nanoseconds>(
                                          std::chrono::steady_clock::now().time_since_epoch())
                                          .count());
}

inline std::uint64_t SteadyNsToProcessUs(std::uint64_t sample_ns, std::uint64_t steady_now_ns,
                                         std::uint64_t process_now_us) {
    if (sample_ns == 0)
        return process_now_us;
    if (sample_ns <= steady_now_ns) {
        const std::uint64_t age_us = (steady_now_ns - sample_ns) / 1000;
        return age_us < process_now_us ? process_now_us - age_us : 0;
    }
    return process_now_us + (sample_ns - steady_now_ns) / 1000;
}

} // namespace Core::HostRuntime::VrTime
