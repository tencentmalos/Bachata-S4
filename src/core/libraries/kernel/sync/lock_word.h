// SPDX-FileCopyrightText: Copyright 2026 shadPS4 Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#pragma once

#include <algorithm>
#include <atomic>
#include <chrono>
#include <optional>
#include <thread>

#include "common/types.h"

#if defined(_WIN64)
#include "core/libraries/kernel/sync/win32_wait.h"
#elif defined(__linux__)
#include <cerrno>
#include <climits>
#include <ctime>
#include <linux/futex.h>
#include <sys/syscall.h>
#include <unistd.h>
#endif

namespace Libraries::Kernel::Sync {

/// The three-state mutex word shared by every mutex implementation: the desktop kernel
/// (LockWord below), the Android guest mutex prefix (host_runtime/guest_mutex.h, whose
/// SHAD_SYNC_STATE_* values must match) and the app-shipped guest fast path
/// (guest/runtime/sync/mutex.c). Lockers exchange to Contended when the word is taken and sleep
/// while it stays Contended; an unlock that finds Contended wakes one sleeper.
namespace LockWordState {
inline constexpr u32 Free = 0;
inline constexpr u32 Held = 1;
inline constexpr u32 Contended = 2;
} // namespace LockWordState

/// Sleeps while the 32-bit word equals `expected`, until woken, the timeout or spuriously.
/// `timeout` of nullopt waits without a limit. Callers recheck their condition.
inline void WaitOnWord(std::atomic<u32>& word, u32 expected,
                       std::optional<std::chrono::nanoseconds> timeout) {
#if defined(_WIN64)
    u32 milliseconds = Win32::Infinite;
    if (timeout) {
        const auto ms = std::chrono::ceil<std::chrono::milliseconds>(*timeout).count();
        milliseconds = static_cast<u32>(std::clamp<s64>(ms, 0, s64(Win32::Infinite) - 1));
    }
    Win32::WaitOnAddress32(&word, expected, milliseconds);
#elif defined(__linux__)
    timespec ts{};
    timespec* ts_ptr = nullptr;
    if (timeout) {
        const auto ns = std::max<s64>(timeout->count(), 0);
        ts.tv_sec = static_cast<time_t>(ns / 1'000'000'000);
        ts.tv_nsec = static_cast<long>(ns % 1'000'000'000);
        ts_ptr = &ts;
    }
    const int saved_errno = errno;
    syscall(SYS_futex, &word, FUTEX_WAIT_PRIVATE, expected, ts_ptr, nullptr, 0);
    errno = saved_errno;
#else
    if (!timeout) {
        word.wait(expected, std::memory_order_relaxed);
    } else if (word.load(std::memory_order_relaxed) == expected) {
        // No timed address wait here: sleep a short slice and let the caller recheck.
        std::this_thread::sleep_for(std::min(*timeout, std::chrono::nanoseconds{100'000}));
    }
#endif
}

inline void WakeOneOnWord(std::atomic<u32>& word) {
#if defined(_WIN64)
    Win32::WakeAddressSingle(&word);
#elif defined(__linux__)
    const int saved_errno = errno;
    syscall(SYS_futex, &word, FUTEX_WAKE_PRIVATE, 1, nullptr, nullptr, 0);
    errno = saved_errno;
#else
    word.notify_one();
#endif
}

/// A host mutex over the shared word protocol: uncontended lock and unlock are one atomic each,
/// contended lockers sleep on the word, and timed locks are supported.
class LockWord {
public:
    LockWord() = default;
    LockWord(const LockWord&) = delete;
    LockWord& operator=(const LockWord&) = delete;

    bool try_lock() {
        u32 expected = LockWordState::Free;
        return state.load(std::memory_order_relaxed) == LockWordState::Free &&
               state.compare_exchange_strong(expected, LockWordState::Held,
                                             std::memory_order_acquire, std::memory_order_relaxed);
    }

    void lock() {
        if (try_lock()) {
            return;
        }
        while (state.exchange(LockWordState::Contended, std::memory_order_acquire) !=
               LockWordState::Free) {
            WaitOnWord(state, LockWordState::Contended, std::nullopt);
        }
    }

    template <class Clock, class Duration>
    bool try_lock_until(const std::chrono::time_point<Clock, Duration>& abs_time) {
        if (try_lock()) {
            return true;
        }
        for (;;) {
            if (state.exchange(LockWordState::Contended, std::memory_order_acquire) ==
                LockWordState::Free) {
                return true;
            }
            const auto now = Clock::now();
            if (abs_time <= now) {
                return false;
            }
            WaitOnWord(state, LockWordState::Contended,
                       std::chrono::duration_cast<std::chrono::nanoseconds>(abs_time - now));
        }
    }

    template <class Rep, class Period>
    bool try_lock_for(const std::chrono::duration<Rep, Period>& rel_time) {
        const auto now = std::chrono::steady_clock::now();
        const auto max_rel = std::chrono::steady_clock::time_point::max() - now;
        const auto rel = std::chrono::duration_cast<std::chrono::steady_clock::duration>(rel_time);
        return try_lock_until(rel <= std::chrono::steady_clock::duration::zero() ? now
                              : rel < max_rel ? now + rel
                                              : std::chrono::steady_clock::time_point::max());
    }

    void unlock() {
        if (state.exchange(LockWordState::Free, std::memory_order_release) ==
            LockWordState::Contended) {
            WakeOneOnWord(state);
        }
    }

private:
    std::atomic<u32> state{LockWordState::Free};
};

} // namespace Libraries::Kernel::Sync
