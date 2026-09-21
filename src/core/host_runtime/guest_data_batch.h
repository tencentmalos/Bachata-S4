// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once
#include <algorithm>
#include "core/guest_cpu/api/address_space.h"

namespace Core::HostRuntime {
// Snapshot contiguous virtual ranges across mapping boundaries, then pin the
// entire batch atomically against those generations. Never pin a partial set
// while waiting for another mapping to retire.
class GuestDataBatch {
    std::vector<GuestCpu::GuestAddressSpace::DataRequest> requests;
    std::vector<GuestCpu::GuestAddressSpace::MappingIdentity> identities;

public:
    bool Add(GuestCpu::GuestAddressSpace& space, GuestCpu::GuestRange range,
             GuestCpu::GuestPermission permission) {
        if (!range.size || range.base.value > UINT64_MAX - range.size)
            return false;
        for (std::uint64_t cursor = range.base.value; cursor < range.End();) {
            auto mapping = space.Query({cursor});
            if (!mapping)
                return false;
            const auto end = std::min(range.End(), mapping.Value().range.End());
            if (end <= cursor)
                return false;
            requests.push_back({{{cursor}, end - cursor}, permission});
            identities.push_back({cursor, end, mapping.Value().mapping_generation});
            cursor = end;
        }
        return true;
    }
    auto Acquire(GuestCpu::GuestAddressSpace& space) {
        for (size_t i = 0; i < requests.size(); ++i)
            requests[i].identities = {&identities[i], 1};
        return space.AcquireDataBatch(requests);
    }
};
} // namespace Core::HostRuntime
