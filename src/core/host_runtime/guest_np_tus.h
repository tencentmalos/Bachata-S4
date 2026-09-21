// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once

#include <algorithm>
#include <array>
#include <string_view>
#include "common/types.h"
#include "core/libraries/np/np_offline_identity.h"

namespace Core::HostRuntime {

inline constexpr std::array<std::string_view, 8> NpTusOfflineNids{
    "1n-dGukBgnY", "lBtrk+7lk14", "H3uq7x0sZOI", "3bh2aBvvmvM",
    "CcIH40dYS88", "2eq1bMwgZYo", "t7b6dmpQNiI", "hYPJFWzFPjA"};

inline bool IsNpTusOfflineNid(std::string_view nid) {
    return std::ranges::find(NpTusOfflineNids, nid) != NpTusOfflineNids.end();
}

inline bool AdmitsNpTusOffline(std::string_view nid, std::string_view suffix, bool offline) {
    return offline && IsNpTusOfflineNid(nid) &&
           suffix == "#libSceNpTus#1#libSceNpTus#Function";
}

inline u32 DispatchNpTusOffline(std::string_view nid, const std::array<u64, 6>& args, s32 sdk) {
    using namespace Libraries::Np;
    if (nid == "1n-dGukBgnY" || nid == "lBtrk+7lk14") {
        // Desktop Tus/Tss CreateA first calls GetNpId with a local output. Use
        // the shared SDK-specific identity policy, before validating the label.
        OrbisNpId identity{};
        return NpManager::Offline::Identity(s32(args[1]), &identity, sdk);
    }
    // Modern offline creation never allocates a context. Legacy identity-based
    // creation, storage requests and stubbed tuning APIs are not admitted.
    // Like desktop, failed request lookup leaves Poll/Wait output untouched.
    if (IsNpTusOfflineNid(nid))
        return ORBIS_NP_COMMUNITY_ERROR_INVALID_ID;
    return ORBIS_NP_COMMUNITY_ERROR_INVALID_ARGUMENT;
}

} // namespace Core::HostRuntime
