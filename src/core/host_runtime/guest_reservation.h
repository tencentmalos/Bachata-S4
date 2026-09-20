// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once

#include <charconv>
#include <istream>
#include <span>
#include <string>
#include <vector>
#include "core/guest_cpu/api/address_space.h"

namespace Core::HostRuntime {
// Android versions place ART's compressed-reference heap at different low
// addresses. Exclude observed host mappings from the proposed guest segments.
// This is only a plan: GuestAddressSpace still acquires each segment with an
// advisory mmap and checks its exact address, so a racing host map fails closed.
inline GuestCpu::Result<std::vector<GuestCpu::GuestRange>> ExcludeHostMappings(
    std::span<const GuestCpu::GuestRange> candidates, std::istream& maps) {
    using namespace GuestCpu;
    std::vector<GuestRange> result(candidates.begin(), candidates.end());
    std::string line;
    std::uint64_t previous_end{};
    unsigned count{};
    while (std::getline(maps, line)) {
        const auto dash = line.find('-');
        const auto end = line.find(' ');
        std::uint64_t lo{}, hi{};
        bool valid = dash != std::string::npos && end != std::string::npos && dash < end;
        if (valid) {
            const auto a = std::from_chars(line.data(), line.data() + dash, lo, 16);
            const auto b = std::from_chars(line.data() + dash + 1, line.data() + end, hi, 16);
            valid = a.ec == std::errc{} && a.ptr == line.data() + dash &&
                    b.ec == std::errc{} && b.ptr == line.data() + end &&
                    lo >= previous_end && lo <= hi;
        }
        if (!valid || ++count > 16384)
            return MakeError(ErrorCategory::InvalidArgument, "GuestReservation",
                             "invalid host mapping snapshot");
        previous_end = hi;
        // Some Android kernels expose an empty executable linker mapping.
        // It occupies no address and must not invalidate the snapshot.
        if (lo == hi) continue;
        std::vector<GuestRange> remaining;
        for (const auto range : result) {
            const auto start = range.base.value;
            const auto stop = range.End();
            if (hi <= start || lo >= stop) {
                remaining.push_back(range);
            } else {
                if (lo > start) remaining.push_back({{start}, lo - start});
                if (hi < stop) remaining.push_back({{hi}, stop - hi});
            }
        }
        result.swap(remaining);
    }
    if (maps.bad() || !maps.eof() || count == 0 || result.empty())
        return MakeError(ErrorCategory::OutOfMemory, "GuestReservation",
                         "host mapping snapshot unavailable or guest segments occupied");
    return result;
}
} // namespace Core::HostRuntime
