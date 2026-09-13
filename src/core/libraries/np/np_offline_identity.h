// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once
#include "common/elf_info.h"
#include "core/libraries/np/np_manager.h"

namespace Libraries::Np::NpManager::Offline {
// Shared desktop/guest offline policy. No process-global clients, callbacks,
// credentials or request ids. `sdk` is the compiled SDK, as in RegisterLib.
inline bool InvalidUser(s32 user) {
    return user == UserService::ORBIS_USER_SERVICE_USER_ID_INVALID;
}
inline bool ModernSdk(s32 sdk, u32 boundary) {
    return sdk < 0 || u32(sdk) >= boundary;
}
inline s32 AccountId(s32 user, u64* output) {
    if (!output || InvalidUser(user))
        return ORBIS_NP_ERROR_INVALID_ARGUMENT;
    *output = 0;
    return ORBIS_NP_ERROR_SIGNED_OUT;
}
inline s32 Identity(s32 user, void* output, s32 sdk) {
    if (InvalidUser(user))
        return ModernSdk(sdk, Common::ElfInfo::FW_900) ? ORBIS_NP_ERROR_INVALID_ARGUMENT
                                                       : ORBIS_NP_ERROR_USER_NOT_FOUND;
    return output ? ORBIS_NP_ERROR_SIGNED_OUT : ORBIS_NP_ERROR_INVALID_ARGUMENT;
}
inline s32 State(s32 user, OrbisNpState* output, s32 sdk) {
    if (!output || (InvalidUser(user) && ModernSdk(sdk, Common::ElfInfo::FW_900)))
        return ORBIS_NP_ERROR_INVALID_ARGUMENT;
    *output = OrbisNpState::SignedOut;
    return 0;
}
inline s32 Reachability(s32 user, OrbisNpReachabilityState* output, s32 sdk) {
    if (!output || (InvalidUser(user) && ModernSdk(sdk, Common::ElfInfo::FW_400)))
        return ORBIS_NP_ERROR_INVALID_ARGUMENT;
    *output = OrbisNpReachabilityState::Unavailable;
    return 0;
}
inline s32 Presence(s32 user, OrbisNpGamePresenseStatus* output) {
    if (!output || InvalidUser(user))
        return ORBIS_NP_ERROR_INVALID_ARGUMENT;
    *output = OrbisNpGamePresenseStatus::Offline;
    return 0;
}
inline s32 UserByAccount(u64 account, s32* output) {
    return account && output ? ORBIS_NP_ERROR_SIGNED_OUT : ORBIS_NP_ERROR_INVALID_ARGUMENT;
}
inline s32 UserByOnline(const OrbisNpOnlineId* online, s32* output) {
    return online && output ? ORBIS_NP_ERROR_SIGNED_OUT : ORBIS_NP_ERROR_INVALID_ARGUMENT;
}
inline s32 SignedUp(s32 user, bool* output, s32 sdk, bool found, bool configured) {
    if (!output)
        return ORBIS_NP_ERROR_INVALID_ARGUMENT;
    *output = false;
    if (InvalidUser(user) && ModernSdk(sdk, Common::ElfInfo::FW_900))
        return ORBIS_NP_ERROR_INVALID_ARGUMENT;
    if (!found)
        return ORBIS_NP_ERROR_USER_NOT_FOUND;
    *output = configured;
    return 0;
}
} // namespace Libraries::Np::NpManager::Offline
