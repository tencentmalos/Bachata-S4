// SPDX-FileCopyrightText: Copyright 2026 shadPS4 Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#pragma once

#include <chrono>
#include <memory>

#include "common/assert.h"
#include "core/libraries/kernel/sync/wait_slot.h"
#include "core/libraries/kernel/threads/pthread.h"

namespace Libraries::Kernel {

/// Defers asynchronous cancellation of `thread` while held: a thread must not exit while it
/// holds a synchronization object's internal lock.
class ScopedPthreadCritical {
public:
    explicit ScopedPthreadCritical(Pthread* thread_) : thread{thread_} {
        if (thread != nullptr) {
            thread->critical_count.fetch_add(1, std::memory_order_acq_rel);
        }
    }
    ScopedPthreadCritical(const ScopedPthreadCritical&) = delete;
    ScopedPthreadCritical& operator=(const ScopedPthreadCritical&) = delete;

    ~ScopedPthreadCritical() {
        if (thread == nullptr) {
            return;
        }
        const int previous = thread->critical_count.fetch_sub(1, std::memory_order_acq_rel);
        ASSERT(previous > 0);
        if (previous == 1) {
            PthreadCancelInterrupt();
        }
    }

private:
    Pthread* thread;
};

/// Wakes a thread parked at a cancellation point: pthread_cancel releases the same semaphore.
class ThreadWaitSlot final : public Sync::WaitSlot {
public:
    explicit ThreadWaitSlot(Pthread* thread_) : thread{thread_} {}
    void Unpark() override {
        thread->wake_sema.release();
    }
    Pthread* Thread() const {
        return thread;
    }

private:
    Pthread* thread;
};

/// The desktop hooks of a shared wait at a pthread cancellation point (sync/wait_slot.h): the
/// thread's wake predicate is drained and cancellation checked each time it queues, it sleeps
/// on its wake semaphore, and relative timeouts count from the first sleep.
class CancellationPointWait {
public:
    CancellationPointWait(Pthread* curthread_, const OrbisKernelTimespec* abstime_, u64 usec_,
                          ClockId clock_id_ = ClockId::Realtime)
        : curthread{curthread_}, abstime{abstime_}, usec{usec_}, clock_id{clock_id_},
          slot{std::make_shared<ThreadWaitSlot>(curthread_)} {}

    std::shared_ptr<Sync::WaitSlot> Slot() const {
        return slot;
    }
    ScopedPthreadCritical Critical() const {
        return ScopedPthreadCritical{curthread};
    }
    // The host equivalent of entering a cancellation-point umtx wait: resetting the predicate
    // and publishing the waiter are serialized with the waker.
    bool BeforePark() const {
        curthread->cancel_point = true;
        curthread->ClearWake();
        if (curthread->ShouldCancel()) {
            curthread->cancel_point = false;
            return false;
        }
        return true;
    }
    Sync::ParkResult Park() {
        bool woke = false;
        if (abstime != THR_RELTIME) {
            woke = curthread->Sleep(abstime, 0, clock_id);
        } else if (first_sleep) {
            const auto now = std::chrono::steady_clock::now();
            const auto max_relative = std::chrono::duration_cast<std::chrono::microseconds>(
                                          std::chrono::steady_clock::time_point::max() - now)
                                          .count();
            reltime_deadline = usec > static_cast<u64>(max_relative)
                                   ? std::chrono::steady_clock::time_point::max()
                                   : now + std::chrono::microseconds(usec);
            first_sleep = false;
            woke = curthread->Sleep(THR_RELTIME, usec);
        } else if (const auto current = std::chrono::steady_clock::now();
                   current < reltime_deadline) {
            const auto remaining =
                std::chrono::ceil<std::chrono::microseconds>(reltime_deadline - current);
            woke = curthread->Sleep(THR_RELTIME, static_cast<u64>(remaining.count()));
        }
        return woke ? Sync::ParkResult::Woken : Sync::ParkResult::TimedOut;
    }
    void AfterPark() const {
        curthread->cancel_point = false;
    }

protected:
    Pthread* curthread;

private:
    const OrbisKernelTimespec* abstime;
    u64 usec;
    ClockId clock_id;
    std::shared_ptr<ThreadWaitSlot> slot;
    std::chrono::steady_clock::time_point reltime_deadline{};
    bool first_sleep = true;
};

} // namespace Libraries::Kernel
