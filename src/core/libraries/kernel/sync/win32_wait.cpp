// SPDX-FileCopyrightText: Copyright 2026 shadPS4 Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#include "core/libraries/kernel/sync/win32_wait.h"

#ifdef _WIN64

#include <windows.h>

namespace Libraries::Kernel::Win32 {

static_assert(Infinite == INFINITE);
static_assert(WaitObject0 == WAIT_OBJECT_0);
static_assert(WaitIoCompletion == WAIT_IO_COMPLETION);
static_assert(WaitTimeout == WAIT_TIMEOUT);

u32 WaitForObject(Handle handle, u32 milliseconds, bool alertable) {
    return WaitForSingleObjectEx(handle, milliseconds, alertable);
}

Handle CreateMutexObject() {
    return CreateMutexW(nullptr, FALSE, nullptr);
}

bool ReleaseMutexObject(Handle handle) {
    return ReleaseMutex(handle) != 0;
}

Handle CreateSemaphoreObject(s32 initial_count, s32 maximum_count) {
    return CreateSemaphoreW(nullptr, initial_count, maximum_count, nullptr);
}

bool ReleaseSemaphoreObject(Handle handle) {
    return ReleaseSemaphore(handle, 1, nullptr) != 0;
}

void CloseObject(Handle handle) {
    CloseHandle(handle);
}

u32 LastError() {
    return GetLastError();
}

} // namespace Libraries::Kernel::Win32

#endif
