// SPDX-FileCopyrightText: Copyright 2026 shadPS4 Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#pragma once

#include "common/types.h"
#include "core/libraries/np/np_types2.h"

namespace Core::Loader {
class SymbolsResolver;
}

namespace Libraries::Np::NpMatching2 {

int PS4_SYSV_ABI sceNpMatching2Initialize(OrbisNpMatching2InitializeParameter* param);
int PS4_SYSV_ABI sceNpMatching2Terminate();
int PS4_SYSV_ABI sceNpMatching2CreateContext(const OrbisNpMatching2CreateContextParameter* param,
                                             OrbisNpMatching2ContextId* ctxId);
int PS4_SYSV_ABI sceNpMatching2CreateContextA(const OrbisNpMatching2CreateContextParameterA* param,
                                              OrbisNpMatching2ContextId* ctxId);

// Console-confirmed result with no network connection. Shared with offline hosts.
inline constexpr s32 OfflineContextStartResult = static_cast<s32>(0x804101e2);
// Shared local control path; offline guest bridges have no network event producer.
int Initialize(OrbisNpMatching2InitializeParameter* param, bool start_dispatcher);
int PS4_SYSV_ABI sceNpMatching2ContextStart(OrbisNpMatching2ContextId ctxId, u64 timeout);
int PS4_SYSV_ABI sceNpMatching2ContextStop(OrbisNpMatching2ContextId ctxId);
int PS4_SYSV_ABI sceNpMatching2DestroyContext(OrbisNpMatching2ContextId ctxId);
int PS4_SYSV_ABI sceNpMatching2AbortContextStart(OrbisNpMatching2ContextId ctxId);
int PS4_SYSV_ABI sceNpMatching2GetServerId(OrbisNpMatching2ContextId ctxId, OrbisNpMatching2ServerId* serverId);

void RegisterLib(Core::Loader::SymbolsResolver* sym);
} // namespace Libraries::Np::NpMatching2
