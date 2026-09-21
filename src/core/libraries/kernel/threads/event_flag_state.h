// SPDX-FileCopyrightText: Copyright 2024-2026 shadPS4 Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once
#include <condition_variable>
#include <list>
#include <mutex>
#include <stop_token>
#include <string>
#include "common/types.h"
#include "core/libraries/kernel/orbis_error.h"

namespace Libraries::Kernel {
// Shared desktop / checked guest bridge state. No guest pointers or host TLS.
// A cancellation token wakes parked HLE owners before the session joins them.
class EventFlagState {
public:
    enum class ClearMode { None, All, Bits };
    enum class WaitMode { And, Or };
    enum class ThreadMode { Single, Multi };
    enum class QueueMode { Fifo, ThreadPrio };

    struct Waiter {
        std::condition_variable_any cv{};

        u64 bits{};
        WaitMode wait_mode{};
        ClearMode clear_mode{};

        u64 result = 0;

        bool ready = false;
        bool canceled = false;
        bool deleted = false;
        bool timed_out = false;

        int priority = 0;
    };

    EventFlagState(const std::string& name, ThreadMode thread_mode, QueueMode queue_mode, u64 bits)
        : m_name(name), m_thread_mode(thread_mode), m_queue_mode(queue_mode), m_bits(bits) {}

    ~EventFlagState() {
        Delete();
        std::unique_lock lock{m_mutex};
        m_destroy_cv.wait(lock, [&] { return m_waiters.empty(); });
    }

    void Delete() {
        std::unique_lock lock{m_mutex};
        m_deleted = true;

        for (auto* w : m_waiters) {
            w->deleted = true;
            w->cv.notify_one();
        }
    }

    bool ConditionMet(const Waiter& waiter) const {
        return (waiter.wait_mode == WaitMode::And) ? (m_bits & waiter.bits) == waiter.bits
                                                   : (m_bits & waiter.bits) != 0;
    }

    void ApplyClear(const Waiter& waiter) {
        switch (waiter.clear_mode) {
        case ClearMode::None:
            break;
        case ClearMode::All:
            m_bits = 0;
            break;
        case ClearMode::Bits:
            m_bits &= ~waiter.bits;
            break;
        default:
            break;
        }
    }

    void AddWaiter(Waiter& waiter) {
        if (m_queue_mode == QueueMode::Fifo) {
            m_waiters.push_back(&waiter);
            return;
        }

        auto it = m_waiters.begin();
        while (it != m_waiters.end() && (*it)->priority >= waiter.priority) {
            ++it;
        }
        m_waiters.insert(it, &waiter);
    }

    void RemoveWaiter(Waiter& waiter) {
        m_waiters.remove(&waiter);
        if (m_deleted && m_waiters.empty()) {
            m_destroy_cv.notify_one();
        }
    }

    int Wait(u64 bits, WaitMode wait_mode, ClearMode clear_mode, u64* result, u32* ptr_micros,
             int priority = 0, std::stop_token cancel = {}, bool* result_written = nullptr) {
        std::unique_lock lock{m_mutex};
        if (result_written) *result_written = false;

        if (cancel.stop_requested()) return ORBIS_KERNEL_ERROR_ECANCELED;
        if (m_deleted) {
            return ORBIS_KERNEL_ERROR_EACCES;
        }

        if (m_thread_mode == ThreadMode::Single && !m_waiters.empty()) {
            return ORBIS_KERNEL_ERROR_EPERM;
        }

        Waiter waiter{
            .bits = bits,
            .wait_mode = wait_mode,
            .clear_mode = clear_mode,
            .priority = priority,
        };

        if (ConditionMet(waiter)) {
            if (result) {
                *result = m_bits;
                if (result_written) *result_written = true;
            }

            ApplyClear(waiter);
            return ORBIS_OK;
        }

        AddWaiter(waiter);

        u32 timeout = 0;
        bool infinite = ptr_micros == nullptr;

        if (!infinite)
            timeout = *ptr_micros;

        auto wake = [&] {
            return waiter.ready || waiter.canceled || waiter.deleted || waiter.timed_out;
        };

        if (infinite) {
            waiter.cv.wait(lock, cancel, wake);
        } else {
            if (!waiter.cv.wait_for(lock, cancel, std::chrono::microseconds(timeout), wake) &&
                !cancel.stop_requested()) {

                RemoveWaiter(waiter);

                if (result) {
                    *result = m_bits;
                    if (result_written) *result_written = true;
                }

                *ptr_micros = 0;
                return ORBIS_KERNEL_ERROR_ETIMEDOUT;
            }
        }

        RemoveWaiter(waiter);

        if (result) {
            *result = waiter.result;
            if (result_written) *result_written = true;
        }

        if (waiter.canceled || cancel.stop_requested())
            return ORBIS_KERNEL_ERROR_ECANCELED;

        if (waiter.deleted)
            return ORBIS_KERNEL_ERROR_EACCES;

        if (waiter.timed_out)
            return ORBIS_KERNEL_ERROR_ETIMEDOUT;

        return ORBIS_OK;
    }

    int Poll(u64 bits, WaitMode wait_mode, ClearMode clear_mode, u64* result,
             bool* result_written = nullptr) {
        u32 micros = 0;
        auto ret = Wait(bits, wait_mode, clear_mode, result, &micros, 0, {}, result_written);
        if (ret == ORBIS_KERNEL_ERROR_ETIMEDOUT) {
            // Poll returns EBUSY instead.
            ret = ORBIS_KERNEL_ERROR_EBUSY;
        }
        return ret;
    }

    void Set(u64 bits) {
        std::unique_lock lock{m_mutex};

        m_bits |= bits;

        for (auto it : m_waiters) {
            Waiter* waiter = it;

            if (waiter->ready || waiter->canceled || waiter->deleted || !ConditionMet(*waiter)) {
                continue;
            }

            waiter->result = m_bits;
            ApplyClear(*waiter);

            waiter->ready = true;
            waiter->cv.notify_one();
        }
    }

    void Clear(u64 bits) {
        std::scoped_lock lock(m_mutex);
        m_bits &= bits;
    }

    void Cancel(u64 pattern, int* num_waiters) {
        std::scoped_lock lock(m_mutex);

        if (num_waiters)
            *num_waiters = static_cast<int>(m_waiters.size());

        m_bits = pattern;

        for (Waiter* waiter : m_waiters) {
            waiter->result = pattern;
            waiter->canceled = true;
            waiter->cv.notify_one();
        }
    }

    size_t Waiting() {
        std::lock_guard lock(m_mutex);
        return m_waiters.size();
    }

private:
    std::mutex m_mutex;

    std::list<Waiter*> m_waiters;
    std::condition_variable m_destroy_cv;

    std::string m_name;

    ThreadMode m_thread_mode;
    QueueMode m_queue_mode;
    bool m_deleted = false;

    u64 m_bits = 0;
};

} // namespace Libraries::Kernel
