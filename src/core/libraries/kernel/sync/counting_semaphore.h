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

/// POSIX sem_t, shared by the desktop kernel and the Android host runtime.
///
/// The count is an atomic word: sem_trywait and an uncontended sem_wait/sem_post never take a
/// lock. Blocked waiters queue in FIFO order; a post wakes exactly one of them, after releasing
/// the lock. A woken waiter competes for the token like any other thread; one that loses it
/// queues again. A selected waiter that times out or is interrupted without taking the token
/// passes the wake on, so a post is never lost.
class CountingSemaphore {
public:
    static constexpr u32 MaxValue = 0x7FFFFFFF;

    explicit CountingSemaphore(u32 value_) : value{value_} {}

    bool TryWait() {
        u32 current = value.load(std::memory_order_acquire);
        while (current > 0) {
            if (value.compare_exchange_weak(current, current - 1, std::memory_order_acquire,
                                            std::memory_order_acquire)) {
                return true;
            }
        }
        return false;
    }

    /// sem_post: 0, or POSIX_EOVERFLOW at the maximum count.
    int Post() {
        u32 current = value.load(std::memory_order_relaxed);
        do {
            if (current >= MaxValue) {
                return POSIX_EOVERFLOW;
            }
        } while (!value.compare_exchange_weak(current, current + 1, std::memory_order_seq_cst,
                                              std::memory_order_relaxed));
        // A waiter registers before re-reading the count and this reads the waiter count after
        // publishing the token: either the waiter sees the token or it is seen here.
        if (waiting.load(std::memory_order_seq_cst) != 0) {
            WakeOne();
        }
        return 0;
    }

    /// Blocks until a token is taken (0), the wait times out (POSIX_ETIMEDOUT) or it is
    /// interrupted (POSIX_EINTR). See WaitSlot.h for the platform hooks.
    template <class Platform>
    int Wait(Platform& platform) {
        for (;;) {
            if (TryWait()) {
                return 0;
            }
            Entry entry{platform.Slot()};
            {
                [[maybe_unused]] auto critical = platform.Critical();
                std::scoped_lock lock{mutex};
                queue.push_back(&entry);
                waiting.fetch_add(1, std::memory_order_seq_cst);
                if (TryWait()) {
                    Dequeue(entry);
                    return 0;
                }
                if (!platform.BeforePark()) {
                    Dequeue(entry);
                    return POSIX_EINTR;
                }
            }
            const ParkResult parked = platform.Park();
            bool selected;
            {
                [[maybe_unused]] auto critical = platform.Critical();
                std::scoped_lock lock{mutex};
                selected = entry.selected;
                if (!selected) {
                    Dequeue(entry);
                }
            }
            platform.AfterPark();
            if (parked == ParkResult::Woken) {
                continue;
            }
            // A post racing the timeout wins while its token is still there.
            if (parked == ParkResult::TimedOut && TryWait()) {
                return 0;
            }
            if (selected && value.load(std::memory_order_acquire) != 0) {
                [[maybe_unused]] auto critical = platform.Critical();
                WakeOne();
            }
            return parked == ParkResult::TimedOut ? POSIX_ETIMEDOUT : POSIX_EINTR;
        }
    }

    u32 Value() const {
        return value.load(std::memory_order_acquire);
    }

    size_t Waiting() const {
        return waiting.load(std::memory_order_acquire);
    }

private:
    struct Entry {
        std::shared_ptr<WaitSlot> slot;
        bool selected{}; ///< Removed from the queue by a post; guarded by the lock.
    };

    void Dequeue(Entry& entry) {
        const auto it = std::ranges::find(queue, &entry);
        if (it != queue.end()) {
            queue.erase(it);
            waiting.fetch_sub(1, std::memory_order_seq_cst);
        }
    }

    void WakeOne() {
        std::shared_ptr<WaitSlot> wake;
        {
            std::scoped_lock lock{mutex};
            if (queue.empty()) {
                return;
            }
            Entry* entry = queue.front();
            queue.erase(queue.begin());
            waiting.fetch_sub(1, std::memory_order_seq_cst);
            entry->selected = true;
            wake = entry->slot;
        }
        wake->Unpark();
    }

    std::atomic<u32> value;
    std::atomic<u32> waiting{0};
    std::mutex mutex;
    std::vector<Entry*> queue;
};

} // namespace Libraries::Kernel::Sync
