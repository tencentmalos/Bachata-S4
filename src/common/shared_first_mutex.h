// SPDX-FileCopyrightText: Copyright 2025 shadPS4 Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#pragma once

#include <algorithm>
#include <array>
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
// spins briefly and then sleeps on the word (WaitOnAddress / futex through std::atomic::wait).
// The whole wait is recorded under `wait_scope` and in the wait statistics.
class SharedFirstMutex {
public:
    using u32 = std::uint32_t;
    using u64 = std::uint64_t;

    // How a waiter spins before it sleeps (ARM64; x86 always uses the pause instruction):
    // Syscall: 64 sched_yield calls (std::this_thread::yield), the original behaviour.
    // Pause: 64 yield instructions, i.e. sleeps almost at once.
    // Timed: yield instructions for up to spin_us microseconds.
    enum class SpinMode : u32 { Syscall, Pause, Timed };
    static inline std::atomic<SpinMode> spin_mode{SpinMode::Syscall};
    static inline std::atomic<u32> spin_us{20};

    // Waits are counted per waiter class: 0 for other threads, 1 for the thread that set
    // wait_class = 1 (the GPU command processor).
    static inline thread_local u32 wait_class = 0;
    static constexpr u32 WaitClasses = 2;
    // Wait duration buckets: < 10 us, < 100 us, < 1 ms, >= 1 ms.
    static constexpr u32 WaitBuckets = 4;
    struct WaitCounters {
        std::atomic<u64> waits{}, parked{}, wait_ns{}, max_wait_ns{};
        std::array<std::atomic<u64>, WaitBuckets> buckets{};
    };
    struct WaitStats {
        std::array<WaitCounters, WaitClasses> shared{};
        std::array<WaitCounters, WaitClasses> exclusive{};
        // Outermost shared hold times (a thread's nested shared locks count as one hold), per
        // holder class with the same buckets. Recorded only while hold_tracking is on, and only
        // for the instance constructed with track_holds.
        std::array<WaitCounters, WaitClasses> shared_holds{};
    };
    static inline std::atomic<bool> hold_tracking{false};
    /// Switches hold tracking. Each switch starts a new period: a hold acquired in an earlier
    /// period is not recorded, and a thread's slot left over from one is reset.
    static void SetHoldTracking(bool on) {
        hold_period.fetch_add(1, std::memory_order_relaxed);
        hold_tracking.store(on, std::memory_order_relaxed);
    }

    SharedFirstMutex() = default;
    explicit SharedFirstMutex(const char* wait_scope_, bool track_holds_ = false)
        : wait_scope{wait_scope_}, track_holds{track_holds_} {}
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
                if (track_holds) {
                    NoteSharedAcquired();
                }
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
        if (track_holds) {
            NoteSharedReleased();
        }
        if (state.fetch_sub(1, std::memory_order_seq_cst) == 1) {
            WakeWaiters();
        }
    }

    const WaitStats& Stats() const {
        return stats;
    }
    void ResetStats() {
        for (auto* group : {&stats.shared, &stats.exclusive, &stats.shared_holds}) {
            for (auto& counters : *group) {
                counters.waits = 0;
                counters.parked = 0;
                counters.wait_ns = 0;
                counters.max_wait_ns = 0;
                for (auto& bucket : counters.buckets) {
                    bucket = 0;
                }
            }
        }
    }

private:
    using Clock = std::chrono::steady_clock;
    static constexpr u32 Writer = 0x80000000u;
    static constexpr int SpinCount = 64;

    static void Pause([[maybe_unused]] SpinMode mode) {
#if defined(__x86_64__) || defined(_M_X64)
        __builtin_ia32_pause();
#elif defined(__aarch64__)
        if (mode == SpinMode::Syscall) {
            std::this_thread::yield();
        } else {
            __asm__ volatile("yield" ::: "memory");
        }
#else
        std::this_thread::yield();
#endif
    }

    // True when `acquire` succeeded while spinning.
    template <typename Acquire>
    static bool Spin(Acquire& acquire) {
        const SpinMode mode = spin_mode.load(std::memory_order_relaxed);
        if (mode != SpinMode::Timed) {
            for (int spin = 0; spin < SpinCount; ++spin) {
                Pause(mode);
                if (acquire()) {
                    return true;
                }
            }
            return false;
        }
        const auto deadline =
            Clock::now() + std::chrono::microseconds{spin_us.load(std::memory_order_relaxed)};
        for (;;) {
            for (int spin = 0; spin < SpinCount; ++spin) {
                Pause(mode);
                if (acquire()) {
                    return true;
                }
            }
            if (Clock::now() >= deadline) {
                return false;
            }
        }
    }

    // A waiter registers before it re-reads the word; the releasing side changes the word before
    // it reads the waiter count. With both sequentially consistent, either the waiter sees the
    // release or the releaser sees the waiter and wakes it.
    template <typename Acquire, typename Blocked>
    void Wait(bool exclusive, Acquire&& acquire, Blocked&& blocked) {
        Common::Profiler::Scope scope{wait_scope};
        const auto start = Clock::now();
        bool parked = false;
        if (!Spin(acquire)) {
            parked = true;
            for (;;) {
                waiters.fetch_add(1, std::memory_order_seq_cst);
                const u32 current = state.load(std::memory_order_seq_cst);
                if (blocked(current)) {
                    state.wait(current, std::memory_order_seq_cst);
                }
                waiters.fetch_sub(1, std::memory_order_relaxed);
                if (acquire()) {
                    break;
                }
            }
        }
        const u64 ns = static_cast<u64>(
            std::chrono::duration_cast<std::chrono::nanoseconds>(Clock::now() - start).count());
        Record((exclusive ? stats.exclusive : stats.shared)[wait_class & 1], ns, parked);
    }

    static void Record(WaitCounters& counters, u64 ns, bool parked) {
        counters.waits.fetch_add(1, std::memory_order_relaxed);
        counters.parked.fetch_add(parked ? 1 : 0, std::memory_order_relaxed);
        counters.wait_ns.fetch_add(ns, std::memory_order_relaxed);
        const u32 bucket = ns < 10'000 ? 0 : ns < 100'000 ? 1 : ns < 1'000'000 ? 2 : 3;
        counters.buckets[bucket].fetch_add(1, std::memory_order_relaxed);
        u64 max = counters.max_wait_ns.load(std::memory_order_relaxed);
        while (ns > max &&
               !counters.max_wait_ns.compare_exchange_weak(max, ns, std::memory_order_relaxed)) {
        }
    }

    // Only one instance tracks holds, so one per-thread slot is enough. The slot is touched only
    // while tracking is on: in a dlopen'd library every thread_local access is a TLS descriptor
    // call, and this instance is the memory manager's lock, taken for every guest read. A hold
    // that started before tracking was switched on is not recorded.
    struct HoldSlot {
        u32 depth;
        u32 period;
        bool timed;
        Clock::time_point start;
    };
    static inline thread_local HoldSlot hold_slot{};
    static inline std::atomic<u32> hold_period{1};

    static void NoteSharedAcquired() {
        if (!hold_tracking.load(std::memory_order_relaxed)) {
            return;
        }
        const u32 period = hold_period.load(std::memory_order_relaxed);
        if (hold_slot.period != period) {
            hold_slot = {.depth = 0, .period = period, .timed = false};
        }
        if (hold_slot.depth++ == 0) {
            hold_slot.timed = true;
            hold_slot.start = Clock::now();
        }
    }

    void NoteSharedReleased() {
        if (!hold_tracking.load(std::memory_order_relaxed) ||
            hold_slot.period != hold_period.load(std::memory_order_relaxed) ||
            hold_slot.depth == 0 || --hold_slot.depth != 0 || !hold_slot.timed) {
            return;
        }
        hold_slot.timed = false;
        const u64 ns = static_cast<u64>(std::chrono::duration_cast<std::chrono::nanoseconds>(
                                            Clock::now() - hold_slot.start)
                                            .count());
        Record(stats.shared_holds[wait_class & 1], ns, false);
    }

    void LockSlow() {
        Wait(true, [this] { return try_lock(); }, [](u32 current) { return current != 0; });
    }

    void LockSharedSlow() {
        Wait(false, [this] { return try_lock_shared(); },
             [](u32 current) { return (current & Writer) != 0; });
    }

    void WakeWaiters() {
        if (waiters.load(std::memory_order_seq_cst) != 0) {
            state.notify_all();
        }
    }

    // Timed acquisition is rare (guest timed rwlocks): poll with short sleeps up to the deadline.
    template <typename Clock_, typename Duration, typename Acquire>
    bool TimedWait(const std::chrono::time_point<Clock_, Duration>& abs_time, Acquire&& acquire) {
        if (acquire()) {
            return true;
        }
        Common::Profiler::Scope scope{wait_scope};
        const SpinMode mode = spin_mode.load(std::memory_order_relaxed);
        for (int round = 0;; ++round) {
            if (acquire()) {
                return true;
            }
            const auto now = Clock_::now();
            if (now >= abs_time) {
                return false;
            }
            if (round < SpinCount) {
                Pause(mode);
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
    WaitStats stats{};
    const char* wait_scope = "Lock.SharedFirstWait";
    const bool track_holds = false;
};

} // namespace Common
