// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once
#include "core/host_runtime/guest_sync_metrics.h"
#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstddef>
#include <functional>
#include <limits>
#include <map>
#include <memory>
#include <mutex>
#include <optional>
#include <shared_mutex>
#include <stop_token>
#include <utility>
#include <vector>
#include "common/types.h"
#include "core/guest_cpu/api/address_space.h"
#include "core/host_runtime/guest_sync_abi.h"
#include "core/host_runtime/guest_sync_waiters.h"
#include "core/libraries/kernel/posix_error.h"

namespace Core::HostRuntime {
// Orbis/POSIX rwlocks. The state lives in guest memory (guest_sync_abi.h): the object
// prefix holds a word (write-locked bit, parked-waiter bits, number of read holders),
// the type and the write owner; each thread records its read holds and their depth in
// a small table below its handle. The app-shipped guest fast path
// (guest/runtime/sync/mutex.c) takes and releases uncontended locks on the same word
// and table without leaving the guest; these HLE entry points are its slow path
// (blocking, timeouts, errors, full tables) and the whole implementation when the fast
// path is off. Semantics follow the shared desktop core (kernel/sync/rw_lock.h):
// types 0/1/2, EDEADLK on a self-deadlock, EPERM on an unlock by a non-holder.
//
// Waits park on the word through GuestAddressWaiters, the same instance the guest
// fast path wakes through, and drop every lock and guest pin while parked.
class GuestRwlockDomain final {
    struct LockState {
        u64 address{};
        u32 type{};
        // Serializes host slow paths on this lock (FreeBSD umtx's busy lock): parked
        // waiter counts, the waiter bits and overflow holds change only under it. The
        // guest fast path never takes it.
        std::mutex mutex;
        u32 blocked_readers{}, blocked_writers{};
        // Read holds the owner's table could not record (table full, or no table).
        // Rare (a thread with more than 15 read holds at once); empty costs nothing.
        std::vector<std::pair<u64, u32>> overflow;
        bool destroyed{};
        auto FindOverflow(u64 owner) {
            return std::find_if(overflow.begin(), overflow.end(),
                                [owner](const auto& r) { return r.first == owner; });
        }
    };
    struct Prefix {
        u32 word{}, type{};
        u64 writer{};
    };
    static_assert(offsetof(Prefix, word) == SHAD_SYNC_RW_WORD);
    static_assert(offsetof(Prefix, type) == SHAD_SYNC_RW_TYPE);
    static_assert(offsetof(Prefix, writer) == SHAD_SYNC_RW_WRITER);
    static_assert(sizeof(Prefix) == SHAD_SYNC_RW_PREFIX_SIZE);
    struct Holds {
        u64 overflow{}, reserved{};
        struct Entry {
            u64 lock{}, depth{};
        } entries[SHAD_SYNC_RW_HOLD_ENTRIES];
    };
    static_assert(sizeof(Holds) == SHAD_SYNC_RW_HOLDS_SIZE);
    static constexpr u32 kWaiters = SHAD_SYNC_RW_WRITE_WAITERS | SHAD_SYNC_RW_READ_WAITERS;
    static constexpr size_t kWakeAll = std::numeric_limits<size_t>::max();
    // Short writable lease over the prefix. Never held across a park: a live pin
    // blocks retirement of the arena page.
    struct Object {
        GuestCpu::PinnedSpan pin;
        Prefix* prefix{};
        std::atomic_ref<u32> Word() const {
            return std::atomic_ref<u32>(prefix->word);
        }
        std::atomic_ref<u64> Writer() const {
            return std::atomic_ref<u64>(prefix->writer);
        }
    };

    GuestCpu::GuestAddressSpace& space;
    std::function<u64()> allocate, allocate_attribute;
    std::unique_ptr<GuestAddressWaiters> own_waiters;
    GuestAddressWaiters* waiters;

    // Guards `locks` and `attributes`. Never held while waiting; lock order: table,
    // then a LockState mutex.
    std::shared_mutex table;
    std::map<u64, std::shared_ptr<LockState>> locks;
    std::map<u64, u32> attributes;
    bool Read(u64 slot, u64& value) {
        return bool(space.ReadData(GuestCpu::GuestAddress{slot},
                                   std::as_writable_bytes(std::span{&value, 1})));
    }
    template <class T>
    int Write(u64 slot, const T& value) {
        // Retain only this output range; never hold VM metadata across the copy.
        return space.WriteData(GuestCpu::GuestAddress{slot}, std::as_bytes(std::span{&value, 1}))
                   ? 0
                   : POSIX_EFAULT;
    }
    int Open(const LockState& state, Object& object) {
        auto pin = space.AcquireDataSpan({GuestCpu::GuestAddress{state.address}, sizeof(Prefix)}, true);
        if (!pin)
            return POSIX_EFAULT;
        object.pin = std::move(pin).Value();
        object.prefix = reinterpret_cast<Prefix*>(object.pin.WritableBytes().data());
        return 0;
    }
    // An owner's read-hold table: owners are thread handles and the table sits at a
    // fixed distance below. An owner without one (unreadable) keeps every hold in
    // the lock's overflow list instead.
    bool LoadHolds(u64 owner, Holds& holds) {
        return owner >= SHAD_SYNC_RW_HOLDS_BELOW_HANDLE &&
               bool(space.ReadData(GuestCpu::GuestAddress{owner - SHAD_SYNC_RW_HOLDS_BELOW_HANDLE},
                                   std::as_writable_bytes(std::span{&holds, 1})));
    }
    void StoreHolds(u64 owner, const Holds& holds) {
        (void)Write(owner - SHAD_SYNC_RW_HOLDS_BELOW_HANDLE, holds);
    }
    static Holds::Entry* FindHold(Holds& holds, u64 lock) {
        for (auto& entry : holds.entries)
            if (entry.lock == lock)
                return &entry;
        return nullptr;
    }
    // Records a fresh read hold (the word already counts it).
    void AddHold(LockState& state, u64 owner) {
        Holds holds{};
        const bool has_table = LoadHolds(owner, holds);
        if (has_table) {
            for (auto& entry : holds.entries) {
                if (!entry.lock) {
                    entry = {state.address, 1};
                    StoreHolds(owner, holds);
                    return;
                }
            }
        }
        state.overflow.emplace_back(owner, 1);
        if (has_table) {
            ++holds.overflow;
            StoreHolds(owner, holds);
        }
    }
    static bool OwnsWrite(const Object& o, u64 owner) {
        const u32 w = o.Word().load(std::memory_order_acquire);
        return (w & SHAD_SYNC_RW_WRITE_LOCKED) && w != SHAD_SYNC_RW_DESTROYED &&
               o.Writer().load(std::memory_order_relaxed) == owner;
    }
    static bool Admits(const LockState& state, u32 w, bool write) {
        if (w & SHAD_SYNC_RW_WRITE_LOCKED)
            return false;
        if (write)
            return (w & SHAD_SYNC_RW_READERS) == 0;
        return state.type == 0 || !(w & SHAD_SYNC_RW_WRITE_WAITERS);
    }
    // A request that stops waiting without the lock: the last waiter of its kind
    // takes its bit off the word, and a departing writer lets readers it kept out in.
    int Leave(LockState& state, Object& o, std::unique_lock<std::mutex>& guard, bool write,
              bool waiting, int result) {
        bool wake = false;
        if (waiting) {
            u32& blocked = write ? state.blocked_writers : state.blocked_readers;
            if (--blocked == 0 && o.prefix) {
                o.Word().fetch_and(~(write ? SHAD_SYNC_RW_WRITE_WAITERS : SHAD_SYNC_RW_READ_WAITERS),
                                   std::memory_order_relaxed);
                wake = write && state.blocked_readers;
            }
        }
        o = {};
        guard.unlock();
        if (wake)
            waiters->Wake(state.address + SHAD_SYNC_RW_WORD, kWakeAll);
        return result;
    }
    // Caller holds `table` exclusively.
    int Create(u64 slot, u32 type, bool attr, u64& address) {
        if (!space.ValidateRange({GuestCpu::GuestAddress{slot}, 8}, GuestCpu::GuestPermission::Write))
            return POSIX_EFAULT;
        // Addresses are session-unique tokens. Capacity follows actual backing
        // and host allocation availability, not the number ever initialized.
        address = attr ? allocate_attribute() : allocate();
        if (!address)
            return POSIX_ENOMEM;
        if (!attr) {
            if (int error = Write(address, Prefix{0, type, 0}))
                return error;
        }
        try {
            if (attr) {
                if (!attributes.emplace(address, type).second)
                    return POSIX_EINVAL;
            } else {
                auto state = std::make_shared<LockState>();
                state->address = address;
                state->type = type;
                if (!locks.emplace(address, std::move(state)).second)
                    return POSIX_EINVAL;
            }
        } catch (const std::bad_alloc&) {
            return POSIX_ENOMEM;
        }
        // Publish only a fully constructed object. A failed guest write must
        // not leave a live entry or replace the caller's previous handle.
        if (int error = Write(slot, address)) {
            if (attr)
                attributes.erase(address);
            else
                locks.erase(address);
            return error;
        }
        return 0;
    }
    // The state a slot names; `create` initializes a zero (static initializer) slot.
    int Find(u64 slot, bool create, std::shared_ptr<LockState>& state) {
        u64 address{};
        if (!Read(slot, address))
            return POSIX_EFAULT;
        if (address) {
            std::shared_lock guard(table);
            if (auto it = locks.find(address); it != locks.end()) {
                state = it->second;
                return 0;
            }
            return POSIX_EINVAL;
        }
        if (!create)
            return POSIX_EINVAL;
        std::unique_lock guard(table);
        // Another thread may have initialized the slot meanwhile.
        if (!Read(slot, address))
            return POSIX_EFAULT;
        if (!address) {
            if (int e = Create(slot, 0, false, address))
                return e;
        }
        auto it = locks.find(address);
        if (it == locks.end())
            return POSIX_EINVAL;
        state = it->second;
        return 0;
    }

public:
    // `shared_waiters` must be the instance behind the runtime's wake import, so a
    // guest release reaches threads parked here. `allocate` returns lock objects
    // (inside the fast-path window when one exists); attribute objects come from
    // `allocate_attribute` (defaults to `allocate`).
    GuestRwlockDomain(GuestCpu::GuestAddressSpace& space, std::function<u64()> allocate,
                      GuestAddressWaiters* shared_waiters = nullptr,
                      std::function<u64()> allocate_attribute = {})
        : space(space), allocate(std::move(allocate)),
          allocate_attribute(std::move(allocate_attribute)),
          own_waiters(shared_waiters ? nullptr : std::make_unique<GuestAddressWaiters>(space)),
          waiters(shared_waiters ? shared_waiters : own_waiters.get()) {
        if (!this->allocate_attribute)
            this->allocate_attribute = this->allocate;
    }
    // actions: init, destroy, get/set pshared, get/set kind.
    int Attribute(u64 slot, int action, u64 value = 0) {
        std::unique_lock guard(table);
        u64 address{};
        if (!Read(slot, address))
            return POSIX_EFAULT;
        auto it = attributes.find(address);
        if (action == 0) {
            if (it != attributes.end())
                return POSIX_EBUSY;
            return Create(slot, 0, true, address);
        }
        if (it == attributes.end())
            return POSIX_EINVAL;
        if (action == 1) {
            if (int e = Write(slot, u64{}))
                return e;
            attributes.erase(it);
            return 0;
        }
        if (action == 2)
            return Write(value, u32{});
        if (action == 3)
            return value ? POSIX_EINVAL : 0; // process-private only
        if (action == 4)
            return Write(value, it->second);
        if (action != 5 || value > 2)
            return POSIX_EINVAL;
        it->second = value;
        return 0;
    }
    int Init(u64 slot, u64 attr) {
        std::unique_lock guard(table);
        u64 address{}, a{};
        if (!Read(slot, address) || (attr && !Read(attr, a)))
            return POSIX_EFAULT;
        if (locks.contains(address))
            return POSIX_EBUSY;
        if (attr && !attributes.contains(a))
            return POSIX_EINVAL;
        return Create(slot, attr ? attributes.at(a) : 0, false, address);
    }
    int Destroy(u64 slot) {
        std::unique_lock guard(table);
        u64 address{};
        if (!Read(slot, address))
            return POSIX_EFAULT;
        if (!address)
            return 0;
        auto it = locks.find(address);
        if (it == locks.end())
            return POSIX_EINVAL;
        // Keep the state alive past the erase below while its mutex is still held.
        const auto keep = it->second;
        auto& state = *keep;
        std::lock_guard state_guard(state.mutex);
        if (state.blocked_readers || state.blocked_writers || !state.overflow.empty())
            return POSIX_EBUSY;
        Object o;
        if (int e = Open(state, o))
            return e;
        // Held (by any path) or waited on: refuse. The marker also sends a guest
        // fast path that still names this object back to the HLE.
        u32 expected = 0;
        if (!o.Word().compare_exchange_strong(expected, SHAD_SYNC_RW_DESTROYED,
                                              std::memory_order_acq_rel))
            return POSIX_EBUSY;
        if (int e = Write(slot, u64{1})) {
            o.Word().store(0, std::memory_order_release);
            return e;
        }
        // A Lock that found this state before the erase sees `destroyed`.
        state.destroyed = true;
        locks.erase(it);
        return 0;
    }
    int Lock(u64 slot, u64 owner, bool write, bool try_only, std::stop_token cancel,
             std::optional<std::chrono::system_clock::time_point> deadline = {}) {
        SyncMetrics::Phase guard_phase{SyncMetrics::Stage::Guard};
        std::shared_ptr<LockState> found;
        if (int e = Find(slot, true, found))
            return e;
        auto& state = *found;
        std::unique_lock guard(state.mutex);
        guard_phase.End();
        if (state.destroyed)
            return POSIX_EINVAL;
        Object o;
        if (int e = Open(state, o))
            return e;
        Holds holds{};
        const bool table_ok = LoadHolds(owner, holds);
        auto* hold = table_ok ? FindHold(holds, state.address) : nullptr;
        const auto over = state.FindOverflow(owner);
        const bool reader = hold || over != state.overflow.end();
        if (OwnsWrite(o, owner) || (reader && (write || state.type == SHAD_SYNC_RW_TYPE_RECURSIVE_REFUSED)))
            return try_only ? POSIX_EBUSY : POSIX_EDEADLK;
        if (reader) {
            // Recursive read: the word already counts this owner once.
            if (hold) {
                if (hold->depth >= UINT32_MAX)
                    return POSIX_EAGAIN;
                ++hold->depth;
                StoreHolds(owner, holds);
            } else {
                if (over->second == UINT32_MAX)
                    return POSIX_EAGAIN;
                ++over->second;
            }
            return 0;
        }
        GuestAddressWaiters::Deadline until;
        if (deadline) {
            // Relative to now on the steady clock; a year is as good as forever.
            auto left = *deadline - std::chrono::system_clock::now();
            left = std::clamp<decltype(left)>(left, decltype(left)::zero(), std::chrono::hours(24 * 365));
            until = std::chrono::steady_clock::now() +
                    std::chrono::duration_cast<std::chrono::steady_clock::duration>(left);
        }
        const u64 word_address = state.address + SHAD_SYNC_RW_WORD;
        bool waiting = false, timed_out = false;
        for (;;) {
            auto word = o.Word();
            u32 w = word.load(std::memory_order_acquire);
            if (Admits(state, w, write)) {
                if (!write && (w & SHAD_SYNC_RW_READERS) == SHAD_SYNC_RW_READERS)
                    return Leave(state, o, guard, write, waiting, POSIX_EAGAIN);
                u32 next = write ? (w | SHAD_SYNC_RW_WRITE_LOCKED) : (w + 1);
                // The last parked waiter of its kind takes its bit along when it enters.
                if (waiting && (write ? state.blocked_writers : state.blocked_readers) == 1)
                    next &= ~(write ? SHAD_SYNC_RW_WRITE_WAITERS : SHAD_SYNC_RW_READ_WAITERS);
                if (!word.compare_exchange_weak(w, next, std::memory_order_acquire,
                                                std::memory_order_relaxed))
                    continue;
                if (waiting)
                    --(write ? state.blocked_writers : state.blocked_readers);
                if (write)
                    o.Writer().store(owner, std::memory_order_relaxed);
                else
                    AddHold(state, owner);
                return 0;
            }
            if (try_only)
                return Leave(state, o, guard, write, waiting, POSIX_EBUSY);
            if (timed_out)
                return Leave(state, o, guard, write, waiting, POSIX_ETIMEDOUT);
            if (cancel.stop_requested())
                return Leave(state, o, guard, write, waiting, POSIX_EINTR);
            if (!waiting) {
                waiting = true;
                ++(write ? state.blocked_writers : state.blocked_readers);
            }
            const u32 bit = write ? SHAD_SYNC_RW_WRITE_WAITERS : SHAD_SYNC_RW_READ_WAITERS;
            if (!(w & bit)) {
                if (!word.compare_exchange_weak(w, w | bit, std::memory_order_relaxed,
                                                std::memory_order_relaxed))
                    continue;
                w |= bit;
            }
            // Park while the word still reads `w`: a release changes it before it
            // wakes, so a release between here and the wait is never lost.
            o = {};
            guard.unlock();
            int result;
            {
                SyncMetrics::Phase park_phase{SyncMetrics::Stage::Park};
                result = waiters->Wait(word_address, w, sizeof(u32), cancel, until);
            }
            guard.lock();
            if (int e = Open(state, o))
                return Leave(state, o, guard, write, waiting, e);
            if (result == POSIX_ETIMEDOUT)
                timed_out = true; // one more look: a lock freed at the deadline is taken
            else if (result)
                return Leave(state, o, guard, write, waiting, result);
        }
    }
    int Unlock(u64 slot, u64 owner) {
        std::shared_ptr<LockState> found;
        {
            SyncMetrics::Phase guard_phase{SyncMetrics::Stage::Guard};
            if (int e = Find(slot, false, found))
                return e;
        }
        auto& state = *found;
        std::unique_lock guard(state.mutex);
        if (state.destroyed)
            return POSIX_EINVAL;
        Object o;
        if (int e = Open(state, o))
            return e;
        bool wake = false;
        if (OwnsWrite(o, owner)) {
            o.Writer().store(0, std::memory_order_relaxed);
            wake = o.Word().fetch_and(~SHAD_SYNC_RW_WRITE_LOCKED, std::memory_order_release) & kWaiters;
        } else {
            Holds holds{};
            const bool table_ok = LoadHolds(owner, holds);
            bool released = false;
            if (auto* hold = table_ok ? FindHold(holds, state.address) : nullptr) {
                if (--hold->depth == 0) {
                    hold->lock = 0;
                    released = true;
                }
                StoreHolds(owner, holds);
            } else if (auto over = state.FindOverflow(owner); over != state.overflow.end()) {
                if (--over->second == 0) {
                    state.overflow.erase(over);
                    released = true;
                    if (table_ok && holds.overflow) {
                        --holds.overflow;
                        StoreHolds(owner, holds);
                    }
                }
            } else {
                return POSIX_EPERM;
            }
            if (released) {
                const u32 old = o.Word().fetch_sub(1, std::memory_order_release);
                wake = (old & SHAD_SYNC_RW_READERS) == 1 && (old & kWaiters);
            }
        }
        // Parked waiters re-check under this lock's mutex; wake after releasing it
        // so they do not immediately block on it again.
        o = {};
        guard.unlock();
        if (wake)
            waiters->Wake(state.address + SHAD_SYNC_RW_WORD, kWakeAll);
        return 0;
    }
    u32 Pending(u64 slot) {
        std::shared_ptr<LockState> found;
        if (Find(slot, false, found))
            return 0;
        std::lock_guard guard(found->mutex);
        return found->blocked_readers + found->blocked_writers;
    }
};
} // namespace Core::HostRuntime
