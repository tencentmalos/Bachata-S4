// SPDX-FileCopyrightText: Copyright 2026 shadPS4 Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#pragma once

#include <algorithm>
#include <chrono>
#include <list>
#include <memory>
#include <mutex>
#include <stop_token>
#include <string>
#include <vector>

#include "common/types.h"
#include "core/libraries/kernel/orbis_error.h"
#include "core/libraries/kernel/sync/parker.h"

namespace Libraries::Kernel::Sync {

/// Orbis kernel semaphore (sceKernelCreateSema family), shared by the desktop kernel and the
/// Android host runtime. The platform layers only map guest handles and arguments; token
/// accounting, waiter order and the wake protocol live here once.
///
/// Waiters queue in FIFO order, or by thread priority (lower value first, FIFO among equals).
/// A signal hands tokens to every waiter in queue order whose need is satisfiable, skipping the
/// others. Each waiter parks on its own slot; completion is decided under the object lock and
/// the wake is issued after that lock is released.
class KernelSemaphore {
public:
    struct State {
        s32 value;
        s32 initial;
        s32 maximum;
        bool fifo;
        size_t waiters;
    };

    KernelSemaphore(std::string name_, s32 initial_, s32 maximum_, bool fifo_)
        : value{initial_}, initial{initial_}, maximum{maximum_}, fifo{fifo_},
          name{std::move(name_)} {}

    const std::string& Name() const {
        return name;
    }

    /// sceKernelWaitSema. `timeout_us` (optional) is the budget in and, when the thread had to
    /// wait (`*parked` set), the time left out.
    s32 Wait(s32 need, u32 priority, u32* timeout_us, std::stop_token stop = {},
             bool* parked = nullptr) {
        if (parked) {
            *parked = false;
        }
        std::unique_lock lock{mutex};
        if (need < 1 || need > maximum) {
            return ORBIS_KERNEL_ERROR_EINVAL;
        }
        if (stop.stop_requested()) {
            return ORBIS_KERNEL_ERROR_EINTR;
        }
        if (value >= need) {
            value -= need;
            return ORBIS_OK;
        }
        if (timeout_us && *timeout_us == 0) {
            return ORBIS_KERNEL_ERROR_ETIMEDOUT;
        }
        auto waiter = std::make_shared<Waiter>(need, priority, stop);
        auto at = waiters.end();
        if (!fifo) {
            at = std::ranges::find_if(waiters,
                                      [&](const auto& other) { return other->priority > priority; });
        }
        waiters.insert(at, waiter);
        const auto start = std::chrono::steady_clock::now();
        const Deadline deadline =
            timeout_us ? Deadline{start + std::chrono::microseconds(*timeout_us)} : std::nullopt;
        lock.unlock();

        waiter->parker.Park(deadline);
        if (parked) {
            *parked = true;
        }

        lock.lock();
        s32 result;
        if (waiter->done) {
            // A selection that raced with the timeout or a stop already consumed tokens for us.
            result = waiter->result;
        } else {
            waiters.remove(waiter);
            result = waiter->parker.StopRequested() ? ORBIS_KERNEL_ERROR_EINTR
                                                    : ORBIS_KERNEL_ERROR_ETIMEDOUT;
        }
        lock.unlock();
        if (timeout_us) {
            const auto elapsed = std::chrono::duration_cast<std::chrono::microseconds>(
                                     std::chrono::steady_clock::now() - start)
                                     .count();
            *timeout_us = result == ORBIS_OK
                              ? static_cast<u32>(std::clamp<s64>(s64(*timeout_us) - elapsed, 0,
                                                                 s64(*timeout_us)))
                              : 0;
        }
        return result;
    }

    /// sceKernelPollSema.
    s32 Poll(s32 need) {
        std::scoped_lock lock{mutex};
        if (need < 1 || need > maximum) {
            return ORBIS_KERNEL_ERROR_EINVAL;
        }
        if (value < need) {
            return ORBIS_KERNEL_ERROR_EBUSY;
        }
        value -= need;
        return ORBIS_OK;
    }

    /// sceKernelSignalSema.
    s32 Signal(s32 count) {
        Wakes wakes;
        {
            std::scoped_lock lock{mutex};
            if (count < 1 || count > maximum - value) {
                return ORBIS_KERNEL_ERROR_EINVAL;
            }
            value += count;
            for (auto it = waiters.begin(); it != waiters.end();) {
                auto& waiter = *it;
                if (waiter->need > value) {
                    ++it;
                    continue;
                }
                value -= waiter->need;
                Complete(waiter, ORBIS_OK, wakes);
                it = waiters.erase(it);
            }
        }
        Release(wakes);
        return ORBIS_OK;
    }

    /// sceKernelCancelSema: every waiter fails with ECANCELED and the count is reset.
    s32 Cancel(s32 set_count, s32* num_waiters) {
        Wakes wakes;
        {
            std::scoped_lock lock{mutex};
            if (set_count > maximum) {
                return ORBIS_KERNEL_ERROR_EINVAL;
            }
            if (num_waiters) {
                *num_waiters = static_cast<s32>(waiters.size());
            }
            for (auto& waiter : waiters) {
                Complete(waiter, ORBIS_KERNEL_ERROR_ECANCELED, wakes);
            }
            waiters.clear();
            value = set_count < 0 ? initial : set_count;
        }
        Release(wakes);
        return ORBIS_OK;
    }

    /// sceKernelDeleteSema: every waiter fails with EACCES. The caller unpublishes the id.
    void Delete() {
        Wakes wakes;
        {
            std::scoped_lock lock{mutex};
            for (auto& waiter : waiters) {
                Complete(waiter, ORBIS_KERNEL_ERROR_EACCES, wakes);
            }
            waiters.clear();
        }
        Release(wakes);
    }

    /// Raises the count to at least `count` (platform lifecycle bridges).
    void RaiseTo(s32 count) {
        std::scoped_lock lock{mutex};
        value = std::max(value, std::min(count, maximum));
    }

    size_t Waiting() const {
        std::scoped_lock lock{mutex};
        return waiters.size();
    }

    State Snapshot() const {
        std::scoped_lock lock{mutex};
        return {value, initial, maximum, fifo, waiters.size()};
    }

private:
    struct Waiter {
        Waiter(s32 need_, u32 priority_, std::stop_token stop)
            : need{need_}, priority{priority_}, parker{std::move(stop)} {}
        s32 need;
        u32 priority;
        Parker parker;
        bool done{};   ///< Guarded by the semaphore lock.
        s32 result{}; ///< Guarded by the semaphore lock.
    };
    using Wakes = std::vector<std::shared_ptr<Waiter>>;

    static void Complete(const std::shared_ptr<Waiter>& waiter, s32 result, Wakes& wakes) {
        waiter->done = true;
        waiter->result = result;
        wakes.push_back(waiter);
    }

    static void Release(const Wakes& wakes) {
        for (const auto& waiter : wakes) {
            waiter->parker.Unpark();
        }
    }

    mutable std::mutex mutex;
    s32 value;
    s32 initial;
    s32 maximum;
    bool fifo;
    std::string name;
    std::list<std::shared_ptr<Waiter>> waiters;
};

} // namespace Libraries::Kernel::Sync
