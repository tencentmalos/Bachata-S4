// SPDX-FileCopyrightText: Copyright 2026 shadPS4 Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#pragma once

#include "common/types.h"

#ifdef _WIN64

// Thin wrappers over the Win32 kernel-object calls used by the sync primitives, so their
// headers do not pull <windows.h> (and its macros) into every translation unit.
namespace Libraries::Kernel::Win32 {

using Handle = void*;

// Values of the Win32 constants of the same meaning; checked against <windows.h> in
// win32_wait.cpp.
inline constexpr u32 Infinite = 0xFFFFFFFFu;
inline constexpr u32 WaitObject0 = 0x00000000u;
inline constexpr u32 WaitIoCompletion = 0x000000C0u;
inline constexpr u32 WaitTimeout = 0x00000102u;

u32 WaitForObject(Handle handle, u32 milliseconds, bool alertable);
Handle CreateMutexObject();
bool ReleaseMutexObject(Handle handle);
Handle CreateSemaphoreObject(s32 initial_count, s32 maximum_count);
bool ReleaseSemaphoreObject(Handle handle);
void CloseObject(Handle handle);
u32 LastError();

} // namespace Libraries::Kernel::Win32

#endif
