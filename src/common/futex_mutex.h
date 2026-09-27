// SPDX-FileCopyrightText: Copyright 2026 shadPS4 Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#pragma once

#if defined(__linux__) || defined(_WIN32)
#include <atomic>
#include <cstdint>
#if defined(__linux__)
#include <cerrno>
#include <linux/futex.h>
#include <sys/syscall.h>
#include <unistd.h>
#endif

namespace Common {

// A non-recursive mutex for GPU page-fault tracking: uncontended lock/unlock is one atomic,
// a contended waiter sleeps in the kernel instead of spinning while the owner copies memory or
// changes page protection. On Linux the slow path is a raw futex (no library waiter table,
// allocation, or hidden shared lock); on Windows it is WaitOnAddress through std::atomic::wait,
// which MSVC's STL maps onto it directly for 4-byte atomics. It may be entered by a fault
// handler only when the interrupted thread does not own it; the renderer accesses guest
// backing through its unprotected alias.
class FutexMutex {
public:
    FutexMutex() = default;
    FutexMutex(const FutexMutex&) = delete;
    FutexMutex& operator=(const FutexMutex&) = delete;

    void lock() noexcept {
        if (try_lock()) {
            return;
        }
#if defined(__linux__)
        const int saved_errno = errno;
#endif
        while (state.exchange(Contended, std::memory_order_acquire) != Unlocked) {
            // EAGAIN (unlock before wait), EINTR, and spurious wakeups all retry
            // the acquire. Unlock publishes before waking a single contender.
#if defined(__linux__)
            syscall(SYS_futex, &state, FUTEX_WAIT_PRIVATE, Contended, nullptr, nullptr, 0);
#else
            state.wait(Contended, std::memory_order_relaxed);
#endif
        }
#if defined(__linux__)
        errno = saved_errno;
#endif
    }

    [[nodiscard]] bool try_lock() noexcept {
        std::uint32_t expected = Unlocked;
        return state.compare_exchange_strong(expected, Locked, std::memory_order_acquire,
                                             std::memory_order_relaxed);
    }

    void unlock() noexcept {
        if (state.exchange(Unlocked, std::memory_order_release) == Contended) {
#if defined(__linux__)
            const int saved_errno = errno;
            syscall(SYS_futex, &state, FUTEX_WAKE_PRIVATE, 1, nullptr, nullptr, 0);
            errno = saved_errno;
#else
            state.notify_one();
#endif
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
