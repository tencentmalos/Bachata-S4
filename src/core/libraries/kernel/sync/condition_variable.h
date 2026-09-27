// SPDX-FileCopyrightText: Copyright 2026 shadPS4 Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#pragma once

#include <algorithm>
#include <atomic>
#include <memory>
#include <mutex>
#include <vector>

#include "common/types.h"
#include "core/libraries/kernel/posix_error.h"
#include "core/libraries/kernel/sync/wait_slot.h"

namespace Libraries::Kernel::Sync {

/// pthread_cond_t, shared by the desktop kernel and the Android host runtime.
///
/// Waiters queue in FIFO order under the condition's own lock; releasing the caller's mutex
/// happens under that same lock, so a notification can never fall between the unlock and the
/// enqueue. A signal selects exactly one waiter (optionally a given owner), a broadcast all of
/// them; the selected waiters are woken after the lock is released, or handed to the platform
/// to wake later (desktop defers the wake until the signaller unlocks the waiter's mutex).
/// A waiter counts against the condition until it has reacquired its mutex, so destroy refuses
/// a condition whose waiters are still on their way out.
class ConditionVariable {
public:
    /// One waiting thread, as seen by the notifier.
    struct Waiter {
        std::shared_ptr<WaitSlot> slot;
        u64 owner;           ///< Thread identity, for targeted signals.
        const void* context; ///< Platform data (desktop: the mutex being waited with).
        bool notified{};     ///< Guarded by the condition lock.
    };

    /// Platform hooks (in addition to those of wait_slot.h):
    ///  - `Owner()`, `Context()`: identify the waiter.
    ///  - `ReleaseMutex()`: under the condition lock; releases the caller's mutex, or returns an
    ///    error (EPERM when it is not the owner) and the wait does not start.
    ///  - `ReacquireMutex()`: outside the lock once the wait ended; its error is returned.
    /// Returns 0 (notified or spurious), POSIX_ETIMEDOUT, POSIX_EINTR, or the mutex error.
    template <class Platform>
    int Wait(Platform& platform) {
        auto waiter =
            std::make_shared<Waiter>(Waiter{platform.Slot(), platform.Owner(), platform.Context()});
        bool interrupted = false;
        {
            [[maybe_unused]] auto critical = platform.Critical();
            std::scoped_lock lock{mutex};
            if (const int error = platform.ReleaseMutex()) {
                return error;
            }
            queue.push_back(waiter);
            queued.fetch_add(1, std::memory_order_seq_cst);
            interrupted = !platform.BeforePark();
            if (interrupted) {
                Dequeue(waiter);
            }
            ++reacquiring;
        }
        ParkResult parked = ParkResult::Interrupted;
        bool notified = false;
        if (!interrupted) {
            for (;;) {
                parked = platform.Park();
                [[maybe_unused]] auto critical = platform.Critical();
                std::scoped_lock lock{mutex};
                if (waiter->notified) {
                    notified = true;
                    break;
                }
                if (parked != ParkResult::Woken) {
                    Dequeue(waiter);
                    break;
                }
                // Spurious wake: sleep again unless the platform stops the wait.
                if (!platform.BeforePark()) {
                    Dequeue(waiter);
                    parked = ParkResult::Interrupted;
                    break;
                }
            }
        }
        platform.AfterPark();
        const int reacquired = platform.ReacquireMutex();
        {
            [[maybe_unused]] auto critical = platform.Critical();
            std::scoped_lock lock{mutex};
            --reacquiring;
        }
        if (reacquired) {
            return reacquired;
        }
        if (notified || parked == ParkResult::Woken) {
            return 0;
        }
        return parked == ParkResult::TimedOut ? POSIX_ETIMEDOUT : POSIX_EINTR;
    }

    /// Wakes the oldest waiter, or the oldest waiter of `target_owner` when non-zero. `defer`
    /// (const Waiter&) -> bool may keep the wake for the platform to issue later. Returns
    /// whether a waiter was selected.
    template <class Defer>
    bool Signal(u64 target_owner, Defer&& defer) {
        if (target_owner == 0 && queued.load(std::memory_order_acquire) == 0) {
            return false;
        }
        std::shared_ptr<WaitSlot> wake;
        {
            std::scoped_lock lock{mutex};
            const auto it = std::ranges::find_if(queue, [&](const auto& waiter) {
                return target_owner == 0 || waiter->owner == target_owner;
            });
            if (it == queue.end()) {
                return false;
            }
            auto waiter = *it;
            queue.erase(it);
            queued.fetch_sub(1, std::memory_order_seq_cst);
            waiter->notified = true;
            if (!defer(*waiter)) {
                wake = waiter->slot;
            }
        }
        if (wake) {
            wake->Unpark();
        }
        return true;
    }

    bool Signal(u64 target_owner = 0) {
        return Signal(target_owner, [](const Waiter&) { return false; });
    }

    /// Wakes every waiter; `defer` as for Signal.
    template <class Defer>
    void Broadcast(Defer&& defer) {
        if (queued.load(std::memory_order_acquire) == 0) {
            return;
        }
        std::vector<std::shared_ptr<WaitSlot>> wakes;
        {
            std::scoped_lock lock{mutex};
            for (auto& waiter : queue) {
                waiter->notified = true;
                if (!defer(*waiter)) {
                    wakes.push_back(waiter->slot);
                }
            }
            queued.fetch_sub(static_cast<u32>(queue.size()), std::memory_order_seq_cst);
            queue.clear();
        }
        for (const auto& slot : wakes) {
            slot->Unpark();
        }
    }

    void Broadcast() {
        Broadcast([](const Waiter&) { return false; });
    }

    /// Queued waiters plus those still reacquiring their mutex.
    bool Busy() const {
        std::scoped_lock lock{mutex};
        return !queue.empty() || reacquiring != 0;
    }

    /// Waiters that left the queue (notified, timed out or interrupted) and have not yet
    /// reacquired their mutex.
    u32 Reacquiring() const {
        std::scoped_lock lock{mutex};
        return reacquiring - static_cast<u32>(queue.size());
    }

    u32 Queued() const {
        return queued.load(std::memory_order_acquire);
    }

private:
    void Dequeue(const std::shared_ptr<Waiter>& waiter) {
        const auto it = std::ranges::find(queue, waiter);
        if (it != queue.end()) {
            queue.erase(it);
            queued.fetch_sub(1, std::memory_order_seq_cst);
        }
    }

    mutable std::mutex mutex;
    std::vector<std::shared_ptr<Waiter>> queue;
    std::atomic<u32> queued{0};
    u32 reacquiring{}; ///< Waiters between ReleaseMutex and the end of ReacquireMutex.
};

} // namespace Libraries::Kernel::Sync
