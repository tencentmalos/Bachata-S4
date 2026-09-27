// SPDX-FileCopyrightText: Copyright 2024 shadPS4 Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#pragma once

#include <algorithm>
#include <atomic>
#include <chrono>

#include "common/assert.h"
#include "common/types.h"

#ifdef _WIN64
#include "core/libraries/kernel/sync/win32_wait.h"
#elif defined(__APPLE__)
#include <dispatch/dispatch.h>
#else
#include <semaphore>
#endif

namespace Libraries::Kernel {

template <s64 max>
class Semaphore {
public:
    Semaphore(s32 initialCount)
#if defined(_WIN64)
        : count{initialCount}
#elif !defined(__APPLE__)
        : sem{initialCount}
#endif
    {
#ifdef _WIN64
        ASSERT_MSG(initialCount >= 0 && initialCount <= max, "Invalid semaphore count {}",
                   initialCount);
#elif defined(__APPLE__)
        sem = dispatch_semaphore_create(initialCount);
        ASSERT_MSG(sem != nullptr, "Failed to create dispatch semaphore");
#endif
    }

    ~Semaphore() {
#ifdef _WIN64
        if (const Win32::Handle handle = sem.load(std::memory_order_acquire)) {
            Win32::CloseObject(handle);
        }
#elif defined(__APPLE__)
        dispatch_release(sem);
#endif
    }

    void release() {
#ifdef _WIN64
        s32 previous = count.load(std::memory_order_relaxed);
        do {
            ASSERT_MSG(previous < max, "Semaphore released past its maximum {}", max);
        } while (!count.compare_exchange_weak(previous, previous + 1, std::memory_order_acq_rel,
                                              std::memory_order_relaxed));
        if (previous < 0) {
            // A thread sleeps (or is about to) for this permit: hand it over through the kernel.
            ASSERT_MSG(Win32::ReleaseSemaphoreObject(Handle()),
                       "Failed to release Win32 semaphore: {}", Win32::LastError());
        }
#elif defined(__APPLE__)
        dispatch_semaphore_signal(sem);
#else
        sem.release();
#endif
    }

    void acquire() {
#ifdef _WIN64
        if (count.fetch_sub(1, std::memory_order_acq_rel) > 0) {
            return;
        }
        const Win32::Handle handle = Handle();
        for (;;) {
            u64 res = Win32::WaitForObject(handle, Win32::Infinite, true);
            if (res == Win32::WaitObject0) {
                return;
            }
            ASSERT_MSG(res == Win32::WaitIoCompletion,
                       "Unexpected Win32 semaphore wait result {:#x}: {}", res, Win32::LastError());
        }
#elif defined(__APPLE__)
        for (;;) {
            const auto res = dispatch_semaphore_wait(sem, DISPATCH_TIME_FOREVER);
            if (res == 0) {
                return;
            }
        }
#else
        sem.acquire();
#endif
    }

    bool try_acquire() {
#ifdef _WIN64
        return TakeAvailable();
#elif defined(__APPLE__)
        return dispatch_semaphore_wait(sem, DISPATCH_TIME_NOW) == 0;
#else
        return sem.try_acquire();
#endif
    }

    // Consume a pending permit without entering an alertable wait. Windows needs a dedicated
    // non-alertable call; the native Darwin and standard C++ semaphore calls already behave so.
    bool try_acquire_pending() {
#ifdef _WIN64
        return TakeAvailable();
#elif defined(__APPLE__)
        return dispatch_semaphore_wait(sem, DISPATCH_TIME_NOW) == 0;
#else
        return sem.try_acquire();
#endif
    }

    template <class Rep, class Period>
    bool try_acquire_for(const std::chrono::duration<Rep, Period>& rel_time) {
#ifdef _WIN64
        using Clock = std::chrono::steady_clock;
        if (rel_time <= std::chrono::duration<Rep, Period>::zero()) {
            return try_acquire_pending();
        }

        const auto now = Clock::now();
        const auto clock_duration = std::chrono::duration_cast<Clock::duration>(rel_time);
        const auto deadline = clock_duration < Clock::time_point::max() - now
                                  ? now + clock_duration
                                  : Clock::time_point::max();
        if (count.fetch_sub(1, std::memory_order_acq_rel) > 0) {
            return true;
        }
        const Win32::Handle handle = Handle();
        for (;;) {
            const auto current = Clock::now();
            if (current < deadline) {
                const auto remaining_ms =
                    std::chrono::ceil<std::chrono::milliseconds>(deadline - current);
                constexpr auto MaxFiniteWait = static_cast<s64>(Win32::Infinite) - 1;
                const u32 timeout_ms =
                    static_cast<u32>(std::min<s64>(remaining_ms.count(), MaxFiniteWait));
                const u32 res = Win32::WaitForObject(handle, timeout_ms, true);
                if (res == Win32::WaitObject0) {
                    return true;
                }
                if (res == Win32::WaitIoCompletion || res == Win32::WaitTimeout) {
                    continue;
                }
            }
            // Deadline passed (or the wait failed): leave the sleepers, unless a release already
            // counted this thread; then its kernel token is on the way and belongs to us.
            s32 current_count = count.load(std::memory_order_acquire);
            while (current_count < 0) {
                if (count.compare_exchange_weak(current_count, current_count + 1,
                                                std::memory_order_acq_rel,
                                                std::memory_order_acquire)) {
                    return false;
                }
            }
            while (Win32::WaitForObject(handle, Win32::Infinite, false) != Win32::WaitObject0) {
            }
            return true;
        }
#elif defined(__APPLE__)
        const auto rel_time_ns = std::chrono::ceil<std::chrono::nanoseconds>(rel_time);
        const auto timeout = dispatch_time(DISPATCH_TIME_NOW, rel_time_ns.count());
        return dispatch_semaphore_wait(sem, timeout) == 0;
#else
        return sem.try_acquire_for(rel_time);
#endif
    }

    template <class Clock, class Duration>
    bool try_acquire_until(const std::chrono::time_point<Clock, Duration>& abs_time) {
        const auto current = Clock::now();
        if (current >= abs_time) {
            return try_acquire_pending();
        }
        return try_acquire_for(abs_time - current);
    }

private:
#ifdef _WIN64
    bool TakeAvailable() {
        s32 current = count.load(std::memory_order_acquire);
        while (current > 0) {
            if (count.compare_exchange_weak(current, current - 1, std::memory_order_acq_rel,
                                            std::memory_order_acquire)) {
                return true;
            }
        }
        return false;
    }

    // Created by the first wait that has to sleep: most semaphores never need it.
    Win32::Handle Handle() {
        Win32::Handle handle = sem.load(std::memory_order_acquire);
        if (handle != nullptr) {
            return handle;
        }
        const Win32::Handle created = Win32::CreateSemaphoreObject(0, 0x7FFFFFFF);
        ASSERT_MSG(created != nullptr, "Failed to create Win32 semaphore: {}", Win32::LastError());
        if (sem.compare_exchange_strong(handle, created, std::memory_order_acq_rel,
                                        std::memory_order_acquire)) {
            return created;
        }
        Win32::CloseObject(created);
        return handle;
    }

    // Permits when positive; minus the threads sleeping (or about to) for one when negative.
    // Taking and returning permits never enters the kernel; the kernel semaphore only carries a
    // release to a sleeping thread, and its wait stays alertable for guest APCs.
    std::atomic<s32> count;
    std::atomic<Win32::Handle> sem{nullptr};
#elif defined(__APPLE__)
    dispatch_semaphore_t sem;
#else
    std::counting_semaphore<max> sem;
#endif
};

using BinarySemaphore = Semaphore<1>;
// A thread can receive one normal synchronization wake and one cancellation wake concurrently.
using WakeSemaphore = Semaphore<2>;
using CountingSemaphore = Semaphore<0x7FFFFFFF /*ORBIS_KERNEL_SEM_VALUE_MAX*/>;

} // namespace Libraries::Kernel
