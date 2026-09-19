// SPDX-FileCopyrightText: Copyright 2026 shadPS4 Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#pragma once

#if defined(__linux__)
#include <atomic>
#include <cerrno>
#include <cstdint>
#include <linux/futex.h>
#include <sys/syscall.h>
#include <unistd.h>

namespace Common {

// A non-recursive Linux mutex for GPU page-fault tracking. Unlike atomic::wait,
// the slow path has no library waiter table, allocation, or hidden shared lock.
// It may be entered by a fault handler only when the interrupted thread does not
// own it; the renderer accesses guest backing through its unprotected alias.
class FutexMutex {
public:
    FutexMutex() = default;
    FutexMutex(const FutexMutex&) = delete;
    FutexMutex& operator=(const FutexMutex&) = delete;

    void lock() noexcept {
        if (try_lock()) {
            return;
        }
        const int saved_errno = errno;
        while (state.exchange(Contended, std::memory_order_acquire) != Unlocked) {
            // EAGAIN (unlock before wait), EINTR, and spurious wakeups all retry
            // the acquire. Unlock publishes before waking a single contender.
            syscall(SYS_futex, &state, FUTEX_WAIT_PRIVATE, Contended, nullptr, nullptr, 0);
        }
        errno = saved_errno;
    }

    [[nodiscard]] bool try_lock() noexcept {
        std::uint32_t expected = Unlocked;
        return state.compare_exchange_strong(expected, Locked, std::memory_order_acquire,
                                             std::memory_order_relaxed);
    }

    void unlock() noexcept {
        if (state.exchange(Unlocked, std::memory_order_release) == Contended) {
            const int saved_errno = errno;
            syscall(SYS_futex, &state, FUTEX_WAKE_PRIVATE, 1, nullptr, nullptr, 0);
            errno = saved_errno;
        }
    }

private:
    static constexpr std::uint32_t Unlocked = 0, Locked = 1, Contended = 2;
    static_assert(std::atomic<std::uint32_t>::is_always_lock_free);
    static_assert(sizeof(std::atomic<std::uint32_t>) == sizeof(std::uint32_t));
    alignas(4) std::atomic<std::uint32_t> state{Unlocked};
};

} // namespace Common
#endif
