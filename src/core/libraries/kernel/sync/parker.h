// SPDX-FileCopyrightText: Copyright 2026 shadPS4 Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#pragma once

#include <chrono>
#include <condition_variable>
#include <mutex>
#include <optional>
#include <stop_token>

namespace Libraries::Kernel::Sync {

enum class ParkResult {
    Woken,       ///< Unpark was called.
    TimedOut,    ///< The deadline passed first.
    Interrupted, ///< The stop token was requested first (session cancellation).
};

using Deadline = std::optional<std::chrono::steady_clock::time_point>;

/// The wake slot of one waiting thread. A waker selects the waiter under the object's lock,
/// releases that lock and only then calls Unpark, so the woken thread never contends with the
/// waker for the object lock; the sleep itself uses the slot's own lock. An Unpark that comes
/// before Park is kept for it; the Park that returns Woken consumes it.
///
/// Shared by the desktop kernel and the Android host runtime: the only platform difference is
/// how a wait is cancelled, which is the optional stop token (the Android session stop). Desktop
/// guest signals and pthread cancellation arrive as special user APCs / signals and need no
/// cooperation from the wait.
class Parker {
public:
    explicit Parker(std::stop_token stop_ = {}) : stop{std::move(stop_)} {}
    Parker(const Parker&) = delete;
    Parker& operator=(const Parker&) = delete;

    ParkResult Park(Deadline deadline) {
        // Registered before taking the slot lock: a callback for an already requested stop runs
        // synchronously and takes that lock.
        std::optional<std::stop_callback<Notify>> on_stop;
        if (stop.stop_possible()) {
            on_stop.emplace(stop, Notify{this});
        }
        std::unique_lock lock{mutex};
        const auto ready = [this] { return woken || stop.stop_requested(); };
        if (deadline) {
            cv.wait_until(lock, *deadline, ready);
        } else {
            cv.wait(lock, ready);
        }
        if (woken) {
            // The wake is consumed: a waiter that loses the race for what it was woken for and
            // parks again sleeps until the next Unpark instead of returning at once.
            woken = false;
            return ParkResult::Woken;
        }
        return stop.stop_requested() ? ParkResult::Interrupted : ParkResult::TimedOut;
    }

    void Unpark() {
        {
            std::scoped_lock lock{mutex};
            woken = true;
        }
        cv.notify_one();
    }

    bool StopRequested() const {
        return stop.stop_requested();
    }

private:
    struct Notify {
        Parker* parker;
        void operator()() const {
            { std::scoped_lock lock{parker->mutex}; }
            parker->cv.notify_all();
        }
    };

    std::mutex mutex;
    std::condition_variable cv;
    bool woken{};
    std::stop_token stop;
};

} // namespace Libraries::Kernel::Sync
