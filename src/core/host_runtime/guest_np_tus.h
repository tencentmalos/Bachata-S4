// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once

#include <algorithm>
#include <array>
#include <string_view>
#include "common/types.h"
#include "core/libraries/np/np_offline_identity.h"
#include "core/libraries/np/np_tus/np_tus.h"
#include "core/guest_cpu/api/address_space.h"
#include "core/libraries/kernel/orbis_error.h"

namespace Core::HostRuntime {

inline constexpr std::array<std::string_view, 12> NpTusOfflineNids{
    "1n-dGukBgnY", "lBtrk+7lk14", "H3uq7x0sZOI", "3bh2aBvvmvM",
    "CcIH40dYS88", "2eq1bMwgZYo", "t7b6dmpQNiI", "hYPJFWzFPjA",
    "6GKDdRCFx8c", "KMlHj+tgfdQ", "-SUR+UoLS6c", "DS2yu3Sjj1o"};

inline bool IsNpTusOfflineNid(std::string_view nid) {
    return std::ranges::find(NpTusOfflineNids, nid) != NpTusOfflineNids.end();
}

inline bool AdmitsNpTusOffline(std::string_view nid, std::string_view suffix, bool offline) {
    return offline && IsNpTusOfflineNid(nid) &&
           suffix == "#libSceNpTus#1#libSceNpTus#Function";
}

inline u32 DispatchNpTusOffline(std::string_view nid, const std::array<u64, 6>& args, s32 sdk) {
    using namespace Libraries::Np;
    // Desktop's tuning functions do not create a worker or an online request.
    if (nid == "6GKDdRCFx8c") return NpTus::sceNpTusSetThreadParam();
    if (nid == "KMlHj+tgfdQ") return NpTus::sceNpTusSetTimeout();
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

// Desktop validates TSS options before resolving the request. Offline creation
// never allocated a request, so no output buffer is touched or retained.
inline u32 DispatchNpTssGetDataOffline(GuestCpu::GuestAddressSpace& space,
                                      const std::array<u64, 6>& a, u64 option) {
    using namespace Libraries::Np;
    using namespace Libraries::Np::NpTus;
    if (!a[2]) return ORBIS_NP_COMMUNITY_ERROR_INSUFFICIENT_ARGUMENT;
    if (a[3] != sizeof(OrbisNpTssDataStatus)) return ORBIS_NP_COMMUNITY_ERROR_INVALID_ALIGNMENT;
    if (s32(a[1]) < 0 || s32(a[1]) > 15) return ORBIS_NP_COMMUNITY_ERROR_INVALID_ARGUMENT;
    if (option) {
        struct Options { u64 size, offset, last_byte, condition; } o{};
        static_assert(sizeof(o) == sizeof(OrbisNpTssGetDataOptParam));
        if (!space.ReadData({option}, std::as_writable_bytes(std::span{&o, 1})))
            return u32(ORBIS_KERNEL_ERROR_EFAULT);
        if (o.size != sizeof(o)) return ORBIS_NP_COMMUNITY_ERROR_INVALID_ALIGNMENT;
        for (auto address : {o.offset, o.last_byte}) {
            u64 value{};
            if (address && !space.ReadData({address}, std::as_writable_bytes(std::span{&value, 1})))
                return u32(ORBIS_KERNEL_ERROR_EFAULT);
        }
        OrbisNpTssIfModifiedSinceParam condition{};
        if (o.condition && !space.ReadData({o.condition}, std::as_writable_bytes(std::span{&condition, 1})))
            return u32(ORBIS_KERNEL_ERROR_EFAULT);
    }
    return ORBIS_NP_COMMUNITY_ERROR_INVALID_ID;
}

} // namespace Core::HostRuntime
