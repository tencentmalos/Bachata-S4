// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once

#include "core/guest_cpu/api/address_space.h"
#include "core/host_runtime/guest_clock.h"

namespace Core::HostRuntime {

// Return an Orbis POSIX errno; the caller owns POSIX vs sce return encoding.
// Keep CLOCK_REALTIME live (including clock corrections and suspend). No TSC
// approximation or cached guest pointer survives a call.
inline int GuestGettimeofday(GuestCpu::GuestAddressSpace& space, const GuestClock& clock,
                            u64 timeval_address, u64 timezone_address) {
    using namespace GuestCpu;
    using namespace Libraries::Kernel;
    // The overwhelmingly common timeval-only path needs no separate ValidateRange:
    // WriteData admits, validates and pins the destination atomically, and retains
    // the mapping through write notifications. Validate-then-Write duplicated the
    // mapping lookup/lock and still could not protect against a concurrent remap.
    // For the optional two-output form, retain validation of BOTH before writing.
    if (timezone_address &&
        ((timeval_address &&
          !space.ValidateRange({{timeval_address}, sizeof(OrbisKernelTimeval)},
                               GuestPermission::Write)) ||
         !space.ValidateRange({{timezone_address}, sizeof(OrbisKernelTimezone)},
                              GuestPermission::Write))) {
        return POSIX_EFAULT;
    }
    OrbisKernelTimespec ts{};
    if (const int error = clock.Read(ORBIS_CLOCK_REALTIME, ts, false))
        return error;
    if (timeval_address) {
        const OrbisKernelTimeval tv{ts.tv_sec, ts.tv_nsec / 1000};
        if (!space.WriteData({timeval_address}, std::as_bytes(std::span{&tv, 1})))
            return POSIX_EFAULT;
    }
    if (timezone_address) {
        const OrbisKernelTimezone tz{}; // Session timezone profile is UTC.
        if (!space.WriteData({timezone_address}, std::as_bytes(std::span{&tz, 1})))
            return POSIX_EFAULT;
    }
    return 0;
}

} // namespace Core::HostRuntime
