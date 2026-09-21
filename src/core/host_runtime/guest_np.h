// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once
#include <algorithm>
#include <array>
#include <cstring>
#include <map>
#include <string_view>
#include "core/guest_cpu/api/address_space.h"
#include "core/libraries/np/np_offline_identity.h"
#include "core/host_runtime/guest_np_control.h"

namespace Core::HostRuntime {
inline constexpr std::string_view NpOfflineNids[]{
    "rbknaUjpqWo", "p-o74CnoNzY", "XDncXQIJUSk", "eQH7nWPcAgc", "e-ZuhGEoeC4",
    "oPO9U42YpgI", "VgYczPGB5ss", "F6E4ycq9Dbg", "Oad3rvY-NJQ", "a8R9-75u4iM",
    "IPb1hd1wAGc", "3Zl8BePTh9Y", "JELHf4xPufo", "A2CQ3kgSopQ", "Ec63y59l9tw"};
inline bool IsNpOfflineNid(std::string_view nid) {
    return IsNpControlNid(nid) || std::ranges::find(NpOfflineNids, nid) != std::end(NpOfflineNids);
}
inline bool AdmitsNpOffline(std::string_view nid, std::string_view suffix, bool offline) {
    if (!offline || !IsNpOfflineNid(nid))
        return false;
    return suffix == "#libSceNpManager#1#libSceNpManager#Function" ||
           ((nid == "JELHf4xPufo" || nid == "0c7HbXRKUt4" || nid == "YIvqqvJyjEc") &&
            suffix == "#libSceNpManagerForToolkit#1#libSceNpManager#Function") ||
           ((nid == "a8R9-75u4iM" || nid == "IPb1hd1wAGc") &&
            suffix == "#libSceNpManagerCompat#1#libSceNpManager#Function");
}
// Session snapshot of only local user ids / signup bits and compiled SDK. No
// credentials or native NP clients/callbacks. Online is rejected by Bind().
class GuestNpOffline {
public:
    GuestNpControl control;
    GuestNpOffline(s32 sdk, std::map<s32, bool> signup) : sdk(sdk), signup(std::move(signup)) {}
    u32 Dispatch(GuestCpu::GuestAddressSpace& space, std::string_view nid,
                 const std::array<u64, 6>& a) {
        if (IsNpControlNid(nid)) return control.Dispatch(space,nid,a);
        using namespace GuestCpu;
        using namespace Libraries::Np;
        using namespace Libraries::Np::NpManager;
        static_assert(sizeof(OrbisNpId) == 36 && sizeof(OrbisNpOnlineId) == 20 &&
                      sizeof(OrbisNpState) == 4 && sizeof(OrbisNpReachabilityState) == 4 &&
                      sizeof(OrbisNpGamePresenseStatus) == 4 && sizeof(bool) == 1);
        if (nid == "A2CQ3kgSopQ") {
            // Desktop validates this local policy; it does not start an NP request.
            struct Restriction { u64 size; s8 age; u8 pad[3]; s32 count; u64 entries; } value{};
            static_assert(sizeof(value) == sizeof(OrbisNpContentRestriction));
            if (!space.ReadData(GuestAddress{a[0]}, std::as_writable_bytes(std::span{&value, 1})))
                return u32(ORBIS_NP_ERROR_INVALID_ARGUMENT);
            if (value.size != sizeof(value)) return u32(ORBIS_NP_ERROR_INVALID_SIZE);
            if (value.age < 0 || value.count < 0 || value.count > 256)
                return u32(ORBIS_NP_ERROR_INVALID_ARGUMENT);
            if (value.count && !space.ValidateRange({GuestAddress{value.entries},
                    u64(value.count) * sizeof(OrbisNpAgeRestriction)}, GuestPermission::Read))
                return u32(ORBIS_NP_ERROR_INVALID_ARGUMENT);
            return 0;
        }
        if (nid == "Ec63y59l9tw") {
            // Never log or retain the guest title secret; desktop only checks inputs.
            const std::array<GuestAddressSpace::DataRequest, 2> requests{
                {{{GuestAddress{a[0]}, sizeof(OrbisNpTitleId)}, GuestPermission::Read},
                 {{GuestAddress{a[1]}, sizeof(OrbisNpTitleSecret)}, GuestPermission::Read}}};
            return space.AcquireDataBatch(requests) ? 0 : u32(ORBIS_NP_ERROR_INVALID_ARGUMENT);
        }
        if (nid == "3Zl8BePTh9Y" || nid == "JELHf4xPufo")
            return 0; // Empty NP queue; registrations/producers are not admitted.
        if (!IsNpOfflineNid(nid))
            return u32(ORBIS_NP_ERROR_INVALID_ARGUMENT);
        if ((nid == "p-o74CnoNzY" || nid == "XDncXQIJUSk") && Offline::InvalidUser(s32(a[0])))
            return u32(Offline::Identity(s32(a[0]), nullptr, sdk));
        size_t bytes = nid == "p-o74CnoNzY"                           ? sizeof(OrbisNpId)
                       : nid == "XDncXQIJUSk"                         ? sizeof(OrbisNpOnlineId)
                       : nid == "rbknaUjpqWo" || nid == "a8R9-75u4iM" ? sizeof(u64)
                       : nid == "Oad3rvY-NJQ"                         ? sizeof(bool)
                                                                      : sizeof(u32);
        // Pin the full output even when offline leaves it unchanged. No host
        // native pointers escape, no partial writes on invalid/cross-page spans.
        auto pin = space.AcquireDataSpan({GuestAddress{a[1]}, bytes}, true);
        if (!pin)
            return u32(ORBIS_NP_ERROR_INVALID_ARGUMENT);
        auto put = [&](const auto& value) {
            std::memcpy(pin.Value().WritableBytes().data(), &value, sizeof(value));
        };
        OrbisNpOnlineId online{};
        if (nid == "F6E4ycq9Dbg" || nid == "a8R9-75u4iM" || nid == "IPb1hd1wAGc") {
            if (!a[0] ||
                !space.ReadData(GuestAddress{a[0]}, std::as_writable_bytes(std::span{&online, 1})))
                return u32(ORBIS_NP_ERROR_INVALID_ARGUMENT);
        }
        const s32 user = s32(a[0]);
        if (nid == "rbknaUjpqWo") {
            u64 value{};
            const auto result = Offline::AccountId(user, &value);
            if (result == ORBIS_NP_ERROR_SIGNED_OUT)
                put(value);
            return u32(result);
        }
        if (nid == "p-o74CnoNzY" || nid == "XDncXQIJUSk")
            return u32(Offline::Identity(user, pin.Value().WritableBytes().data(), sdk));
        if (nid == "eQH7nWPcAgc") {
            OrbisNpState value{};
            const auto result = Offline::State(user, &value, sdk);
            if (!result)
                put(value);
            return u32(result);
        }
        if (nid == "e-ZuhGEoeC4") {
            OrbisNpReachabilityState value{};
            const auto result = Offline::Reachability(user, &value, sdk);
            if (!result)
                put(value);
            return u32(result);
        }
        if (nid == "oPO9U42YpgI") {
            OrbisNpGamePresenseStatus value{};
            const auto result = Offline::Presence(user, &value);
            if (!result)
                put(value);
            return u32(result);
        }
        if (nid == "VgYczPGB5ss" || nid == "F6E4ycq9Dbg") {
            s32 value{}; // Both offline calls deliberately preserve guest output.
            return u32(nid == "VgYczPGB5ss" ? Offline::UserByAccount(a[0], &value)
                                            : Offline::UserByOnline(&online, &value));
        }
        if (nid == "Oad3rvY-NJQ") {
            const auto it = signup.find(user);
            bool value{};
            const auto result = Offline::SignedUp(user, &value, sdk, it != signup.end(),
                                                  it != signup.end() && it->second);
            put(value); // Desktop clears the byte even for an invalid/missing user.
            return u32(result);
        }
        if (nid == "a8R9-75u4iM") {
            // Fresh offline sessions have no signed-in NP online-id mapping.
            put(u64{0});
            return u32(ORBIS_NP_ERROR_USER_NOT_FOUND);
        }
        if (nid == "IPb1hd1wAGc") {
            put(OrbisNpGamePresenseStatus::Offline);
            return 0;
        }
        return u32(ORBIS_NP_ERROR_INVALID_ARGUMENT);
    }

private:
    s32 sdk;
    std::map<s32, bool> signup;
};
} // namespace Core::HostRuntime
