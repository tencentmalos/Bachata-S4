// SPDX-FileCopyrightText: Copyright 2025 shadPS4 Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#pragma once

#include "common/types.h"

namespace Core::Loader {
class SymbolsResolver;
}

namespace Libraries::Kernel {

static constexpr u64 ORBIS_DBG_MAX_NAME_LENGTH = 256;

struct OrbisModuleInfoForUnwind {
    u64 st_size;
    std::array<char, ORBIS_DBG_MAX_NAME_LENGTH> name;
    VAddr eh_frame_hdr_addr;
    VAddr eh_frame_addr;
    u64 eh_frame_size;
    VAddr seg0_addr;
    u64 seg0_size;
};

s32 PS4_SYSV_ABI sceKernelIsNeoMode();

s32 PS4_SYSV_ABI sceKernelGetCompiledSdkVersion(s32* ver);

// Process identity scalars used during guest libc/runtime startup. Declared so
// the Android production runtime can dispatch them through the same real
// implementations the desktop core uses, instead of re-deriving the values.
s32 PS4_SYSV_ABI sceKernelIsInSandbox();
s32 PS4_SYSV_ABI sceKernelHasNeoMode();
s32 PS4_SYSV_ABI sceKernelIsDevkit();
s32 PS4_SYSV_ABI sceKernelIsProspero();
s32 PS4_SYSV_ABI sceKernelIsCEX();
s32 PS4_SYSV_ABI sceKernelGetMainSocId();
s32 PS4_SYSV_ABI sceKernelGetCpumode();
s32 PS4_SYSV_ABI sceKernelGetCurrentCpu();

s32 PS4_SYSV_ABI sceKernelGetModuleInfoForUnwind(VAddr addr, s32 flags,
                                                 OrbisModuleInfoForUnwind* info);

void RegisterProcess(Core::Loader::SymbolsResolver* sym);

} // namespace Libraries::Kernel
