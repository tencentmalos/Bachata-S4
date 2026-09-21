// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once
#include <algorithm>
#include <array>
#include <mutex>
#include "core/guest_cpu/api/address_space.h"
#include "core/libraries/np/np_error.h"
#include "core/libraries/np/np_offline_identity.h"
namespace Core::HostRuntime {
inline constexpr std::string_view NpUtilityNids[]{
    "W6iWw8aUQtA", "M5Jyo9TKYPI", // utility lifecycle
    "6p9jvljuvsw", "r9BgI0PfJZg", "iCq5xW5KQW4", "IEB+vgVoQbw",
    "t0P5z5yuFPA", "PYFS1H70bDs", "rAOOqDAxBIk", "Fa4dVWgmffk",
    "87ivWj5yKzg", "ur5SShyG0dk", "Jj4mkpFO2gE", "1dMndqL-QgE",
    "jktww3yJXnc", "BYIZGKm6bO4", "kvdMF48mB3Y", "pLr1fEQS1z8"};
inline bool IsNpUtilityNid(std::string_view nid) {
    return std::ranges::find(NpUtilityNids, nid) != std::end(NpUtilityNids);
}
// Checked offline negative capabilities. No signed-in NP identity exists, so
// no word-filter title/request or network bandwidth test is created. Module
// startup initializes the local utility domain, just as the firmware DT_INIT.
class GuestNpUtility {
    std::mutex mutex;
    bool initialized{true};
    s32 sdk;
public:
    explicit GuestNpUtility(s32 sdk) : sdk(sdk) {}
    u32 Dispatch(GuestCpu::GuestAddressSpace& space, std::string_view nid,
                 const std::array<u64, 6>& a) {
        using namespace GuestCpu;
        std::lock_guard lock(mutex);
        auto valid = [&](u64 at, u64 bytes, GuestPermission permission) {
            return at && space.ValidateRange({{at}, bytes}, permission);
        };
        if (nid == "W6iWw8aUQtA") { initialized = true; return 0; }
        if (nid == "M5Jyo9TKYPI") { initialized = false; return 0; }
        if (nid == "jktww3yJXnc") {
            struct Param { u64 size; u64 affinity; s32 priority; u32 padding; std::array<u32,8> reserved; } p{};
            static_assert(sizeof(p) == 56);
            if (!a[0] || !space.ReadData({a[0]}, std::as_writable_bytes(std::span{&p,1})))
                return u32(ORBIS_NP_BANDWIDTH_TEST_ERROR_INVALID_ARGUMENT);
            if (sdk >= 0x02500000 && std::ranges::any_of(p.reserved, [](u32 v) { return v != 0; }))
                return u32(ORBIS_NP_BANDWIDTH_TEST_ERROR_INVALID_ARGUMENT);
            if (p.size != 56) return u32(ORBIS_NP_BANDWIDTH_TEST_ERROR_INVALID_SIZE);
            return u32(ORBIS_NP_BANDWIDTH_TEST_ERROR_CONTEXT_NOT_AVAILABLE);
        }
        if (nid == "BYIZGKm6bO4" || nid == "pLr1fEQS1z8") {
            // No bandwidth test was started; preserve caller output on error.
            if (!a[0]) return u32(ORBIS_NP_BANDWIDTH_TEST_ERROR_INVALID_ARGUMENT);
            return u32(ORBIS_NP_BANDWIDTH_TEST_ERROR_NOT_INITIALIZED);
        }
        if (nid == "kvdMF48mB3Y") return u32(ORBIS_NP_BANDWIDTH_TEST_ERROR_NOT_INITIALIZED);
        if (!initialized) return u32(ORBIS_NP_COMMUNITY_ERROR_NOT_INITIALIZED);
        if (nid == "6p9jvljuvsw")
            {
            Libraries::Np::OrbisNpId local{};
            return u32(Libraries::Np::NpManager::Offline::Identity(s32(a[0]), &local, sdk));
        }
        if (nid == "r9BgI0PfJZg") {
            if (!valid(a[0], sizeof(Libraries::Np::OrbisNpId), GuestPermission::Read))
                return u32(ORBIS_NP_COMMUNITY_ERROR_INSUFFICIENT_ARGUMENT);
            return u32(ORBIS_NP_COMMUNITY_ERROR_NO_LOGIN);
        }
        if (nid == "Jj4mkpFO2gE" || nid == "1dMndqL-QgE") {
            const bool sanitize = nid == "Jj4mkpFO2gE";
            if (!a[1] || (sanitize && !a[2])) return u32(ORBIS_NP_COMMUNITY_ERROR_INSUFFICIENT_ARGUMENT);
            // Censor's third parameter is a reserved option, not an output.
            if (a[sanitize ? 3 : 2]) return u32(ORBIS_NP_COMMUNITY_ERROR_INVALID_ARGUMENT);
            bool terminated{};
            for (u64 i = 0; i <= 1024; ++i) {
                char ch{};
                if (a[1] > UINT64_MAX-i || !space.ReadData({a[1]+i}, std::as_writable_bytes(std::span{&ch,1})))
                    return u32(ORBIS_NP_COMMUNITY_ERROR_INVALID_ARGUMENT);
                if (!ch) { terminated = true; break; }
            }
            if (!terminated) return u32(ORBIS_NP_COMMUNITY_ERROR_INVALID_ARGUMENT);
        }
        if (nid == "IEB+vgVoQbw") {
            struct Param { u64 size, affinity; s32 priority; u32 padding; } p{};
            if (!a[1] || !space.ReadData({a[1]}, std::as_writable_bytes(std::span{&p,1})) || p.size != 24)
                return u32(ORBIS_NP_COMMUNITY_ERROR_INVALID_ARGUMENT);
        }
        // Every id is invalid in this signed-out domain. Never copy unfiltered
        // input into a successful sanitization result or fabricate request ids.
        return u32(ORBIS_NP_COMMUNITY_ERROR_INVALID_ID);
    }
};
}
