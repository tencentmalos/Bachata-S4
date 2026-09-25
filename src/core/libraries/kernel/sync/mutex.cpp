// SPDX-FileCopyrightText: Copyright 2024 shadPS4 Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#include "mutex.h"

#include "common/assert.h"

namespace Libraries::Kernel {

TimedMutex::TimedMutex() {
#ifdef _WIN64
    mtx = Win32::CreateMutexObject();
    ASSERT(mtx);
#endif
}

TimedMutex::~TimedMutex() {
#ifdef _WIN64
    Win32::CloseObject(mtx);
#endif
}

void TimedMutex::lock() {
#ifdef _WIN64
    for (;;) {
        u64 res = Win32::WaitForObject(mtx, Win32::Infinite, true);
        if (res == Win32::WaitObject0) {
            return;
        }
    }
#else
    mtx.lock();
#endif
}

bool TimedMutex::try_lock() {
#ifdef _WIN64
    return Win32::WaitForObject(mtx, 0, true) == Win32::WaitObject0;
#else
    return mtx.try_lock();
#endif
}

void TimedMutex::unlock() {
#ifdef _WIN64
    Win32::ReleaseMutexObject(mtx);
#else
    mtx.unlock();
#endif
}

} // namespace Libraries::Kernel
