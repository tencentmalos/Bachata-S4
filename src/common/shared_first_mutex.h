// SPDX-FileCopyrightText: Copyright 2025 shadPS4 Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#pragma once

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstdint>
#include <thread>

#include "common/profiler.h"

namespace Common {

// Like std::shared_mutex, but reader has priority over writer: a reader enters whenever no
// writer holds the lock, even while writers wait (a thread may take a shared lock again while it
// already holds one).
//
// One atomic word: the writer bit and the reader count. Uncontended shared and exclusive
// lock/unlock are one atomic each and never touch another lock; only a thread that has to wait
// sleeps on the word (WaitOnAddress / futex through std::atomic::wait), and only then is the
// wait recorded under `wait_scope`.
class SharedFirstMutex {
public:
    SharedFirstMutex() = default;
    explicit SharedFirstMutex(const char* wait_scope_) : wait_scope{wait_scope_} {}
    SharedFirstMutex(const SharedFirstMutex&) = delete;
    SharedFirstMutex& operator=(const SharedFirstMutex&) = delete;

    void lock() {
        if (!try_lock()) {
            LockSlow();
        }
    }

    bool try_lock() {
        u32 expected = 0;
        return state.compare_exchange_strong(expected, Writer, std::memory_order_acquire,
                                             std::memory_order_relaxed);
    }

    template <typename Clock, typename Duration>
    bool try_lock_until(const std::chrono::time_point<Clock, Duration>& abs_time) {
        return TimedWait(abs_time, [this] { return try_lock(); });
    }

    void unlock() {
        write_epoch.fetch_add(1, std::memory_order_relaxed);
        state.fetch_and(~Writer, std::memory_order_seq_cst);
        WakeWaiters();
    }

    /// Number of exclusive sections completed. Stable while the caller holds a shared lock, so
    /// state a reader derived under an earlier shared lock is still valid if this is unchanged.
    std::uint64_t WriteEpoch() const {
        return write_epoch.load(std::memory_order_relaxed);
    }

    void lock_shared() {
        if (!try_lock_shared()) {
            LockSharedSlow();
        }
    }

    bool try_lock_shared() {
        u32 current = state.load(std::memory_order_relaxed);
        while ((current & Writer) == 0) {
            if (state.compare_exchange_weak(current, current + 1, std::memory_order_acquire,
                                            std::memory_order_relaxed)) {
                return true;
            }
        }
        return false;
    }

    template <typename Clock, typename Duration>
    bool try_lock_shared_until(const std::chrono::time_point<Clock, Duration>& abs_time) {
        return TimedWait(abs_time, [this] { return try_lock_shared(); });
    }

    void unlock_shared() {
        if (state.fetch_sub(1, std::memory_order_seq_cst) == 1) {
            WakeWaiters();
        }
    }

private:
    using u32 = std::uint32_t;
    static constexpr u32 Writer = 0x80000000u;
    static constexpr int SpinCount = 64;

    static void Pause() {
#if defined(__x86_64__) || defined(_M_X64)
        __builtin_ia32_pause();
#else
        std::this_thread::yield();
#endif
    }

    // A waiter registers before it re-reads the word; the releasing side changes the word before
    // it reads the waiter count. With both sequentially consistent, either the waiter sees the
    // release or the releaser sees the waiter and wakes it.
    template <typename Acquire, typename Blocked>
    void Wait(Acquire&& acquire, Blocked&& blocked) {
        for (int spin = 0; spin < SpinCount; ++spin) {
            Pause();
            if (acquire()) {
                return;
            }
        }
        Common::Profiler::Scope scope{wait_scope};
        for (;;) {
            waiters.fetch_add(1, std::memory_order_seq_cst);
            const u32 current = state.load(std::memory_order_seq_cst);
            if (blocked(current)) {
                state.wait(current, std::memory_order_seq_cst);
            }
            waiters.fetch_sub(1, std::memory_order_relaxed);
            if (acquire()) {
                return;
            }
        }
    }

    void LockSlow() {
        Wait([this] { return try_lock(); }, [](u32 current) { return current != 0; });
    }

    void LockSharedSlow() {
        Wait([this] { return try_lock_shared(); },
             [](u32 current) { return (current & Writer) != 0; });
    }

    void WakeWaiters() {
        if (waiters.load(std::memory_order_seq_cst) != 0) {
            state.notify_all();
        }
    }

    // Timed acquisition is rare (guest timed rwlocks): poll with short sleeps up to the deadline.
    template <typename Clock, typename Duration, typename Acquire>
    bool TimedWait(const std::chrono::time_point<Clock, Duration>& abs_time, Acquire&& acquire) {
        if (acquire()) {
            return true;
        }
        Common::Profiler::Scope scope{wait_scope};
        for (int round = 0;; ++round) {
            if (acquire()) {
                return true;
            }
            const auto now = Clock::now();
            if (now >= abs_time) {
                return false;
            }
            if (round < SpinCount) {
                Pause();
            } else {
                const auto remaining =
                    std::chrono::duration_cast<std::chrono::microseconds>(abs_time - now);
                std::this_thread::sleep_for(
                    std::min<std::chrono::microseconds>(remaining, std::chrono::microseconds{200}));
            }
        }
    }

    std::atomic<u32> state{0};
    std::atomic<u32> waiters{0};
    std::atomic<std::uint64_t> write_epoch{0};
    const char* wait_scope = "Lock.SharedFirstWait";
};

} // namespace Common
