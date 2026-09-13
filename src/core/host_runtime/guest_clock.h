// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once

#include <cerrno>
#include <limits>
#include <sys/resource.h>
#include <time.h>
#include "common/native_clock.h"
#include "core/libraries/kernel/posix_error.h"
#include "core/libraries/kernel/time.h"

namespace Core::HostRuntime {
// Session-owned clock state. No desktop RegisterTime call or native pthread TLS
// is required; callers translate the returned Orbis errno into their ABI.
class GuestClock final {
public:
    Common::NativeClock ticks;
    const u64 origin = ticks.GetUptime();

    int Read(u32 id, Libraries::Kernel::OrbisKernelTimespec& out, bool resolution) const {
        using namespace Libraries::Kernel;
        if (id == ORBIS_CLOCK_PROCTIME) {
            const u64 us = resolution ? 0 : ticks.GetTimeUS(origin);
            out = {static_cast<s64>(us / 1000000),
                   resolution ? 1000 : static_cast<s64>(us % 1000000) * 1000};
            return 0;
        }
        if (id == ORBIS_CLOCK_VIRTUAL || id == ORBIS_CLOCK_PROF) {
            if (resolution) {
                out = {0, 1000};
                return 0;
            }
            rusage ru{};
            if (::getrusage(RUSAGE_SELF, &ru))
                return POSIX_EFAULT;
            const auto& t = id == ORBIS_CLOCK_VIRTUAL ? ru.ru_utime : ru.ru_stime;
            out = {t.tv_sec, t.tv_usec * 1000};
            return 0;
        }
        clockid_t native;
        switch (id) {
        case ORBIS_CLOCK_REALTIME:
        case ORBIS_CLOCK_REALTIME_PRECISE:
        case ORBIS_CLOCK_REALTIME_FAST:
        case ORBIS_CLOCK_SECOND:
            native = CLOCK_REALTIME;
            break;
        case ORBIS_CLOCK_UPTIME:
        case ORBIS_CLOCK_UPTIME_PRECISE:
        case ORBIS_CLOCK_UPTIME_FAST:
        case ORBIS_CLOCK_MONOTONIC:
        case ORBIS_CLOCK_MONOTONIC_PRECISE:
        case ORBIS_CLOCK_MONOTONIC_FAST:
            native = CLOCK_MONOTONIC;
            break;
        case ORBIS_CLOCK_THREAD_CPUTIME_ID:
            native = CLOCK_THREAD_CPUTIME_ID;
            break;
        // Network clocks require a provider; do not silently substitute a local clock.
        default:
            return POSIX_EINVAL;
        }
        timespec ts{};
        if (resolution ? ::clock_getres(native, &ts) : ::clock_gettime(native, &ts))
            return errno == EINVAL ? POSIX_EINVAL : POSIX_EFAULT;
        out = {ts.tv_sec, ts.tv_nsec};
        return 0;
    }

    static bool Duration(const Libraries::Kernel::OrbisKernelTimespec& ts,
                         std::chrono::nanoseconds& result) {
        if (ts.tv_sec < 0 || ts.tv_nsec < 0 || ts.tv_nsec >= 1000000000 ||
            ts.tv_sec > (std::numeric_limits<s64>::max() - ts.tv_nsec) / 1000000000)
            return false;
        result = std::chrono::nanoseconds(ts.tv_sec * 1000000000 + ts.tv_nsec);
        return true;
    }
};
} // namespace Core::HostRuntime
