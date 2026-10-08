// SPDX-FileCopyrightText: Copyright 2024 shadPS4 Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#pragma once

#include <chrono>
#include <condition_variable>
#include <mutex>
#include <optional>
#include <stop_token>
#include <string>
#include <vector>
#include <boost/asio/steady_timer.hpp>

#include <unordered_map>
#include "common/rdtsc.h"
#include "common/types.h"

namespace Core::Loader {
class SymbolsResolver;
}

namespace Libraries::Kernel {

class EqueueInternal;
struct EqueueEvent;

struct OrbisKernelBintime {
    s64 sec;
    s64 frac;
};

using OrbisKernelUseconds = u32;
using OrbisKernelEqueue = s64;

struct OrbisKernelEvent {
    enum Filter : s16 {
        None = 0,
        Read = -1,
        Write = -2,
        Aio = -3,
        Vnode = -4,
        Proc = -5,
        Signal = -6,
        Timer = -7,
        Fs = -9,
        Lio = -10,
        User = -11,
        Polling = -12,
        VideoOut = -13,
        GraphicsCore = -14,
        HrTimer = -15,
    };

    enum Flags : u16 {
        Add = 1u,
        Delete = 2u,
        Enable = 4u,
        Disable = 8u,
        OneShot = 0x10u,
        Clear = 0x20u,
        Receipt = 0x40u,
        Dispatch = 0x80u,
        Flag1 = 0x2000u,
        System = 0xf000u,
    };

    u64 ident = 0;                /* identifier for this event */
    Filter filter = Filter::None; /* filter for event */
    u16 flags = 0;
    u32 fflags = 0;
    u64 data = 0;
    void* udata = nullptr; /* opaque user data identifier */
};

struct OrbisVideoOutEventHint {
    u64 event_id : 8;
    u64 video_id : 8;
    u64 flip_arg : 48;
};

struct OrbisVideoOutEventData {
    u64 time : 12;
    u64 count : 4;
    u64 flip_arg : 48;
};

struct EqueueEvent {
    OrbisKernelEvent event;
    void* data = nullptr;
    std::chrono::steady_clock::time_point time_added;
    std::chrono::nanoseconds timer_interval;
    std::unique_ptr<boost::asio::steady_timer> timer;

    void Clear() {
        is_triggered = false;
        event.fflags = 0;
        event.data = 0;
    }

    void Trigger(void* data) {
        is_triggered = true;
        event.data = reinterpret_cast<uintptr_t>(data);
    }

    void TriggerUser(void* data) {
        is_triggered = true;
        event.udata = data;
    }

    void TriggerTimer() {
        is_triggered = true;
        event.data++;
    }

    void TriggerDisplay(void* data) {
        is_triggered = true;
        if (data != nullptr) {
            auto event_data = std::bit_cast<OrbisVideoOutEventData>(event.data);
            auto event_hint_raw = reinterpret_cast<u64>(data);
            auto event_hint = static_cast<OrbisVideoOutEventHint>(event_hint_raw);
            if (event_hint.event_id == event.ident && event.ident != 0xfe) {
                auto time = Common::FencedRDTSC();
                auto counter = event_data.count;
                if (counter != 0xf) {
                    counter++;
                }
                event.data =
                    (time & 0xfff) | (counter << 0xc) | (event_hint_raw & 0xffffffffffff0000);
            }
        }
    }

    bool IsTriggered() const {
        return is_triggered;
    }

    bool operator==(const EqueueEvent& ev) const {
        return ev.event.ident == event.ident && ev.event.filter == event.filter;
    }

private:
    bool is_triggered = false;
};

enum class EqueueWaitResult {
    Ready,       // triggered events or expired small timers are pending
    TimedOut,    // the deadline passed first
    Interrupted, // the stop token was requested
    Closed,      // the queue was closed (deleted)
};

class EqueueInternal {
    struct SmallTimer {
        OrbisKernelEvent event;
        std::chrono::steady_clock::time_point added;
        std::chrono::nanoseconds interval;
    };

public:
    /// Small timers expire at sub-millisecond precision the condition variable cannot give: a
    /// waiter sleeps until this long before the expiry, then yields.
    static constexpr std::chrono::microseconds SmallTimerSpin{200};

    explicit EqueueInternal(OrbisKernelEqueue handle, std::string_view name)
        : m_handle(handle), m_name(name) {}

    std::string_view GetName() const {
        return m_name;
    }

    bool AddEvent(EqueueEvent& event);
    bool ScheduleEvent(u64 id, s16 filter,
                       void (*callback)(OrbisKernelEqueue, const OrbisKernelEvent&));
    bool RemoveEvent(u64 id, s16 filter);
    /// Guest wait (desktop): waits and consumes; timo null waits forever, *timo == 0 polls.
    int WaitForEvents(OrbisKernelEvent* ev, int num, const OrbisKernelUseconds* timo);
    bool TriggerEvent(u64 ident, s16 filter, void* trigger_data);

    /// Android HLE wait (host_runtime/guest_graphics_hle.cpp), which polls: consumes up to `num`
    /// triggered events, small timers excluded.
    int GetTriggeredEvents(OrbisKernelEvent* ev, int num);

    /// Desktop wait: waits until events can be taken, without consuming them.
    EqueueWaitResult WaitReady(std::optional<std::chrono::steady_clock::time_point> deadline,
                               std::stop_token stop);
    /// Consumes up to `num` triggered events and expired small timers.
    int TakeTriggered(OrbisKernelEvent* ev, int num);
    /// Wakes every waiter with EqueueWaitResult::Closed; no event is taken afterwards.
    void Close();

    bool AddSmallTimer(EqueueEvent& event);
    // Session HR timers share the deadline queue, avoiding desktop-global asio
    // callbacks. The copied event never retains a native or guest time pointer.
    bool AddHRTimer(u64 id, std::chrono::nanoseconds interval, void* udata);
    bool HasSmallTimer() {
        std::scoped_lock lock{m_mutex};
        return !m_small_timers.empty();
    }
    bool RemoveSmallTimer(u64 id) {
        if (HasSmallTimer()) {
            std::scoped_lock lock{m_mutex};
            ++m_small_timer_generation;
            return m_small_timers.erase(id) > 0;
        }
        return false;
    }

    bool EventExists(u64 id, s16 filter);

private:
    /// Under m_mutex: whether anything can be taken at `now`; the earliest pending small timer
    /// expiry is lowered into *next_timer.
    bool HasReadyLocked(std::chrono::steady_clock::time_point now,
                        std::chrono::steady_clock::time_point* next_timer) const;
    int TakeTriggeredLocked(OrbisKernelEvent* ev, int num);
    int TakeEventsLocked(OrbisKernelEvent* ev, int num);

    OrbisKernelEqueue m_handle;
    std::string m_name;
    std::mutex m_mutex;
    std::vector<EqueueEvent> m_events;
    std::condition_variable_any m_cond;
    std::unordered_map<u64, SmallTimer> m_small_timers;
    bool m_closed{};
    u64 m_small_timer_generation{}; // bumped when small timers are added or removed
};

class SessionEqueues {
public:
    virtual ~SessionEqueues() = default;
    virtual EqueueInternal* Find(OrbisKernelEqueue handle) = 0;
};
void BindSessionEqueues(SessionEqueues* queues);
EqueueInternal* GetEqueue(OrbisKernelEqueue eq);
u64 PS4_SYSV_ABI sceKernelGetEventData(const OrbisKernelEvent* ev);
u64 PS4_SYSV_ABI sceKernelGetEventId(const OrbisKernelEvent* ev);
int PS4_SYSV_ABI sceKernelGetEventFilter(const OrbisKernelEvent* ev);
void* PS4_SYSV_ABI sceKernelGetEventUserData(const OrbisKernelEvent* ev);

void RegisterEventQueue(Core::Loader::SymbolsResolver* sym);

} // namespace Libraries::Kernel
