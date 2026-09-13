// SPDX-FileCopyrightText: Copyright 2026 shadPS4 Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#pragma once

#include "common/types.h"
#include "core/libraries/kernel/process.h"

namespace Libraries::SysModule {

extern bool g_need_scelibc, g_need_fios2;

s32 getModuleHandle(s32 id, s32* handle);
bool shouldHideName(const char* module_name);
bool isDebugModule(s32 id);
bool validateModuleId(s32 id);
s32 loadModuleInternal(s32 index, s32 argc, const void* argv, s32* res_out);
s32 loadModule(s32 id, s32 argc, const void* argv, s32* res_out);
s32 unloadModule(s32 id, s32 argc, const void* argv, s32* res_out, bool is_internal);
s32 preloadModulesForLibkernel();

// Pure lookup of a public/internal sysmodule id in the static module table.
// Returns false for an unknown id. On success reports the bare module library
// name (no extension) and whether the module lives under /app0/sce_module
// (IsGame). Used by the Android production runtime to resolve provider
// readiness without reproducing the desktop global-singleton load path.
bool LookupSysmodule(u32 id, const char** name_out, bool* is_game_out);

} // namespace Libraries::SysModule