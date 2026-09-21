// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once
#include <algorithm>
#include <condition_variable>
#include <stdexcept>
#include <vector>
#include <deque>
#include <thread>
#include "core/guest_cpu/api/address_space.h"
#include "core/host_runtime/guest_callback_owner.h"
#include "core/libraries/system/userservice.h"
#include "core/libraries/system/userservice_error.h"
namespace Core::HostRuntime {
// A separate subscriber to local login events; it never consumes GetEvent's
// queue or creates an online account. Module text stays pinned for the session.
class GuestUserCallbacks {
    using Event = Libraries::UserService::OrbisUserServiceEvent;
    struct Slot { u64 function{}, argument{}, revision{}; };
    struct Delivery { Slot slot; Event event; };
    Core::GuestCpu::GuestAddressSpace& space;
    GuestCallbackOwner owner;
    std::mutex mutex;
    std::condition_variable changed;
    std::vector<Slot> slots;
    std::deque<Delivery> queue;
    std::vector<Event> initial_events;
    std::thread worker;
    u64 revision{}, executing{};
    bool initialized{}, stopping{};
    void Work() {
        bool attached{};
        u64 scratch{};
        try {
            for (;;) {
                Delivery d;
                {
                    std::unique_lock lock(mutex);
                    changed.wait(lock, [&] { return stopping || !queue.empty(); });
                    if (stopping) break;
                    d = queue.front(); queue.pop_front();
                    if (!std::ranges::any_of(slots, [&](auto& s) { return s.revision == d.slot.revision; })) continue;
                    executing = d.slot.revision;
                }
                if (!attached) { scratch = owner.begin(d.slot.function); attached = true; }
                bool run;
                { std::lock_guard lock(mutex); run = !stopping && initialized; }
                if (run) {
                    if (!space.WriteData({scratch}, std::as_bytes(std::span{&d.event, 1})))
                        throw std::runtime_error("UserService callback event publication failed");
                    owner.invalidate(scratch, sizeof(d.event));
                    const std::array<u64, 2> args{scratch, d.slot.argument};
                    owner.call(d.slot.function, args);
                }
                { std::lock_guard lock(mutex); executing = 0; }
                changed.notify_all();
            }
        } catch (const std::exception& error) {
            bool cancelled;
            { std::lock_guard lock(mutex); cancelled = stopping; stopping = true; }
            if (!cancelled && owner.fail) owner.fail(error.what());
            // The production owner records a failed guest call and cancels the
            // session. Never continue delivering later callbacks after failure.
            owner.cancel();
        }
        if (attached) owner.end();
        { std::lock_guard lock(mutex); executing = 0; }
        changed.notify_all();
    }
public:
    GuestUserCallbacks(Core::GuestCpu::GuestAddressSpace& space, GuestCallbackOwner owner)
        : space(space), owner(std::move(owner)) {}
    ~GuestUserCallbacks() { RequestStop(); if (worker.joinable()) worker.join(); }
    void Initialize(const Libraries::UserService::OrbisUserServiceLoginUserIdList& users) {
        std::lock_guard lock(mutex);
        initialized = true; initial_events.clear();
        for (s32 user : users.user_id) if (user >= 0)
            initial_events.push_back({Libraries::UserService::OrbisUserServiceEventType::Login, user});
    }
    void Terminate() {
        std::unique_lock lock(mutex);
        initialized = false; slots.clear(); queue.clear(); initial_events.clear();
        if (worker.get_id() != std::this_thread::get_id())
            changed.wait(lock, [&] { return !executing; });
    }
    void RequestStop() {
        { std::lock_guard lock(mutex); stopping = true; queue.clear(); }
        owner.cancel(); changed.notify_all();
    }
    u32 Dispatch(bool remove, u64 function, u64 argument) {
        using namespace Core::GuestCpu;
        if (!function || !space.ValidateRange({{function}, 1}, GuestPermission::Execute))
            return u32(ORBIS_USER_SERVICE_ERROR_INVALID_ARGUMENT);
        std::unique_lock lock(mutex);
        if (!initialized || stopping) return u32(ORBIS_USER_SERVICE_ERROR_NOT_INITIALIZED);
        auto found = std::ranges::find_if(slots, [&](auto& s) { return s.function == function && s.argument == argument; });
        if (remove) {
            if (found == slots.end()) return u32(ORBIS_USER_SERVICE_ERROR_INVALID_ARGUMENT);
            const auto old = found->revision; slots.erase(found);
            std::erase_if(queue, [&](auto& d) { return d.slot.revision == old; });
            if (worker.get_id() != std::this_thread::get_id())
                changed.wait(lock, [&] { return executing != old; });
            return 0;
        }
        if (found != slots.end()) return u32(ORBIS_USER_SERVICE_ERROR_INVALID_ARGUMENT);
        if (slots.size() >= 8) return u32(ORBIS_USER_SERVICE_ERROR_NO_MEMORY);
        const Slot slot{function, argument, ++revision};
        const auto old_size = queue.size();
        try {
            slots.push_back(slot);
            for (auto event : initial_events) queue.push_back({slot, event});
            if (!worker.joinable()) worker = std::thread([this] { Work(); });
        } catch (...) {
            std::erase_if(slots, [&](auto& s) { return s.revision == slot.revision; });
            while (queue.size() > old_size) queue.pop_back();
            return u32(ORBIS_USER_SERVICE_ERROR_NO_MEMORY);
        }
        changed.notify_all();
        return 0;
    }
};
}
