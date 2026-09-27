// SPDX-FileCopyrightText: Copyright 2026 shadPS4 Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#pragma once

#include <memory>
#include <stop_token>

#include "core/libraries/kernel/sync/parker.h"

namespace Libraries::Kernel::Sync {

/// How a queued waiter is woken. The shared algorithms select a waiter under their object lock
/// and call Unpark after releasing it, through shared ownership, so the waiter may already have
/// left (timed out) when the call arrives; an Unpark it does not consume is harmless.
///
/// Platforms supply the implementation: the Android host runtime and non-cancellable desktop
/// waits park on a Parker; desktop pthread cancellation points park on the thread's own wake
/// semaphore, which pthread_cancel also releases.
class WaitSlot {
public:
    virtual ~WaitSlot() = default;
    virtual void Unpark() = 0;
};

/// A WaitSlot over a Parker: sleeps with an optional deadline and stop token.
class ParkerSlot final : public WaitSlot {
public:
    explicit ParkerSlot(std::stop_token stop = {}) : parker{std::move(stop)} {}
    void Unpark() override {
        parker.Unpark();
    }
    ParkResult Park(Deadline deadline) {
        return parker.Park(deadline);
    }
    bool StopRequested() const {
        return parker.StopRequested();
    }

private:
    Parker parker;
};

/// The waiting side of a wait: the platform hooks the shared algorithms call.
///
///  - `Slot()`: the slot queued for this wait (the same one for every round of one wait).
///  - `Critical()`: an RAII guard held around every section under the object lock (desktop
///    defers asynchronous cancellation there; a thread must not exit holding the lock).
///  - `BeforePark()`: called under the object lock once queued; false aborts the wait as
///    interrupted (desktop: a pending cancellation at the cancellation point).
///  - `Park()`: sleeps until the slot is unparked, the deadline, or an interruption.
///  - `AfterPark()`: called outside the lock after every park (desktop: act on cancellation).
struct ParkerWait {
    explicit ParkerWait(std::stop_token stop_, Deadline deadline_ = {})
        : stop{std::move(stop_)}, deadline{deadline_} {}

    /// Allocated only when the wait actually has to queue.
    std::shared_ptr<WaitSlot> Slot() {
        if (!slot) {
            slot = std::make_shared<ParkerSlot>(stop);
        }
        return slot;
    }
    struct NoCritical {};
    NoCritical Critical() const {
        return {};
    }
    bool BeforePark() const {
        return !stop.stop_requested();
    }
    ParkResult Park() {
        return slot->Park(deadline);
    }
    void AfterPark() const {}

    std::stop_token stop;
    Deadline deadline;
    std::shared_ptr<ParkerSlot> slot;
};

} // namespace Libraries::Kernel::Sync
