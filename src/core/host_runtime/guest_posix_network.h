// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once
#include <bit>
#include <chrono>
#include <functional>
#include <limits>
#include "core/host_runtime/guest_network.h"
#include "core/libraries/kernel/posix_error.h"

namespace Core::HostRuntime {
inline constexpr std::string_view PosixNetworkNids[]{
    "3e+4Iv7IJ8U", "KuOmgKoqCdY", "XVL8So3QJUk", "TXFFFiNldU8", "RenI1lL1WFk",
    "6O8EwYOgH9Y", "4n51s0zEf0c", "pxnCmagrtao", "Ez8xjo9UF4E", "lUk6wrGXyMw",
    "fZOeZIOEmLw", "oBr313PppNE", "fFxGkxF2bVo", "TUuiYS2kE8s", "TU-d9PfIHPM",
    "5jRCs2axtr4", "8nY19bKoiZk", "T8fER+tIGgk", "hI7oVeOluPM", "aNeavPDNKzA"};
inline bool IsPosixNetworkNid(std::string_view nid) {
    return std::ranges::find(PosixNetworkNids, nid) != std::end(PosixNetworkNids);
}
class GuestPosixNetwork {
public:
    using Failure = GuestSockets::Failure;
    using Ready = GuestSockets::FileReady;
    static u64 Dispatch(GuestNetwork& network, GuestCpu::GuestAddressSpace& space,
                        std::string_view nid, const std::array<u64, 6>& a, u64 posix_errno,
                        const Failure& fail, const Ready& ready, std::stop_token stop = {}) {
        if (nid == "4n51s0zEf0c" || nid == "5jRCs2axtr4")
            return network.Dispatch(space, nid == "4n51s0zEf0c" ? "8Kcp5d-q1Uo" : "9vA2aW+CHuA", a,
                                    posix_errno, stop);
        return network.Sockets().Dispatch(space, nid, a, fail, ready, stop);
    }
};
} // namespace Core::HostRuntime
