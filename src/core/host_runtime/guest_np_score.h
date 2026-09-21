// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once

#include <algorithm>
#include <array>
#include <string_view>
#include "common/types.h"
#include "core/libraries/np/np_error.h"

namespace Core::HostRuntime {

// The modern offline entry point cannot create a title context: desktop checks
// PSN sign-in before the service label. Consequently this provider owns no title
// contexts or requests. Legacy identity-based creation and online operations
// remain outside its contract; never publish it for an online session.
inline constexpr std::array<std::string_view, 8> NpScoreOfflineNids{
    "GWnWQNXZH5M", "G0pE+RNCwfk", "gW8qyjYrUbk", "dK8-SgYf6r4",
    "1i7kmKbX6hk", "m1DfNRstkSQ", "fqk8SC63p1U", "bygbKdHmjn4"};

inline bool IsNpScoreOfflineNid(std::string_view nid) {
    return std::ranges::find(NpScoreOfflineNids, nid) != NpScoreOfflineNids.end();
}

inline bool AdmitsNpScoreOffline(std::string_view nid, std::string_view suffix, bool offline) {
    return offline && IsNpScoreOfflineNid(nid) &&
           suffix == "#libSceNpScore#1#libSceNpScore#Function";
}

inline u32 DispatchNpScoreOffline(std::string_view nid, const std::array<u64, 6>& args) {
    using namespace Libraries::Np;
    if (nid == "GWnWQNXZH5M")
        return ORBIS_NP_ERROR_SIGNED_OUT;
    if (nid == "bygbKdHmjn4" && s32(args[1]) < 0)
        return ORBIS_NP_COMMUNITY_ERROR_INVALID_ARGUMENT;
    // Delete/CreateRequest/Abort/Poll/Wait perform the same failed lookup as
    // desktop. In particular Poll/Wait neither write guest output nor report a
    // completed request. Invalid IDs take precedence over the output pointer.
    if (IsNpScoreOfflineNid(nid))
        return ORBIS_NP_COMMUNITY_ERROR_INVALID_ID;
    return ORBIS_NP_COMMUNITY_ERROR_INVALID_ARGUMENT;
}

} // namespace Core::HostRuntime
