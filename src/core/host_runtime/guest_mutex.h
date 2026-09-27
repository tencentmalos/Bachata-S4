// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once
#include "core/host_runtime/guest_sync_metrics.h"

#include <algorithm>
#include <atomic>
#include <condition_variable>
#include <cstring>
#include <functional>
#include <map>
#include <memory>
#include <mutex>
#include <optional>
#include <stop_token>
#include <thread>
#include <vector>
#include "core/guest_cpu/api/address_space.h"
#include "core/host_runtime/guest_clock.h"
#include "core/host_runtime/guest_sync_abi.h"
#include "core/host_runtime/guest_sync_waiters.h"
#include "core/libraries/kernel/posix_error.h"

namespace Core::HostRuntime {
// Orbis mutex state lives in the guest ABI prefix (guest_sync_abi.h): owner,
// recursion count and a three-state lock word. The host keeps only immutable
// identity (address/type/protocol), retirement and the wait queues, so the
// app-shipped guest fast path (guest/runtime/sync/mutex.c) and these HLE entry
// points interoperate on one object with one protocol. libc is allowed to
// update the prefix flags at +0x20; the published handle addresses a guest
// allocation, never a native mutex, pthread pointer or an unmapped integer.
class GuestMutexDomain final {
public:
    // Optional diagnostic observer, installed before owners start. Called under
    // the owning object guard: it must not reenter, block or inspect guest memory.
    // Different objects may report concurrently.
    enum class CondTraceKind { Enqueued, Notified, Resumed, Reacquired };
    struct CondTraceEvent {
        CondTraceKind kind;
        u64 condition, mutex, owner;
        int result;
    };
    using CondObserver = void (*)(const CondTraceEvent&) noexcept;
    void SetCondObserver(CondObserver observer) { cond_observer = observer; }
private:
    CondObserver cond_observer{};
    void TraceCond(CondTraceKind kind, u64 condition, u64 mutex, u64 owner, int result = 0) const {
        if (cond_observer) cond_observer({kind, condition, mutex, owner, result});
    }
    struct Prefix {
        u64 owner{};
        u32 count{}, spins{}, yields{}, protocol{};
        u32 state{}, unused{};
        u32 flags{};
    };
    static_assert(offsetof(Prefix, owner) == SHAD_SYNC_MUTEX_OWNER);
    static_assert(offsetof(Prefix, count) == SHAD_SYNC_MUTEX_COUNT);
    static_assert(offsetof(Prefix, state) == SHAD_SYNC_MUTEX_STATE);
    static_assert(offsetof(Prefix, flags) == SHAD_SYNC_MUTEX_FLAGS);
    static_assert(sizeof(Prefix) == SHAD_SYNC_MUTEX_PREFIX_SIZE);
    struct Mutex {
        u64 address{};
        u32 type{1}, protocol{};
        // Destroy raises `retiring` before it inspects the word and lowers it
        // after deciding; an acquirer that won the word meanwhile waits for the
        // decision and gives the word back when the object was retired.
        std::atomic<bool> retiring{}, retired{};
        // Condition waiters queued against this mutex (they hold no lock word
        // while parked); counted so Destroy and PendingWaits see them.
        std::atomic<u32> cond_waiters{};
        Mutex* directory_next{}; // immutable after release publication
    };
    // Short writable lease over the ABI prefix. Never held across a park: a
    // live pin blocks retirement of the arena page (address-space M13), and it
    // stays on the admitted VM path so a retiring data edit over the prefix
    // page blocks the operation (guest_native_mutex_tests "range retirement
    // blocks one object").
    struct Object {
        GuestCpu::PinnedSpan pin;
        Prefix* prefix{};
        std::atomic_ref<u32> State() const {
            return std::atomic_ref<u32>(prefix->state);
        }
    };
    struct AttributeState {
        u32 type{1}, protocol{}, ceiling{};
    };
    GuestCpu::GuestAddressSpace& space;
    std::function<u64()> allocate;

    std::mutex guard;
    std::map<u64, Mutex*> mutexes;
    // Retain published identities until owners join at Session teardown. Bucket
    // chains are immutable; lookup never takes the registry lock and has no
    // fixed population ceiling. Retired addresses cannot acquire a new object.
    std::vector<std::unique_ptr<Mutex>> mutex_storage;
    static constexpr size_t DirectorySize = 8192;
    std::array<std::atomic<Mutex*>, DirectorySize> mutex_directory{};
    static size_t DirectoryHash(u64 address) {
        auto key = address >> 6;
        key ^= key >> 30;
        key *= 0xbf58476d1ce4e5b9ULL;
        key ^= key >> 27;
        return key & (DirectorySize - 1);
    }
    Mutex* LookupMutex(u64 address) const {
        auto* state = mutex_directory[DirectoryHash(address)].load(std::memory_order_acquire);
        for (; state; state = state->directory_next)
            if (state->address == address) return state;
        return nullptr;
    }
    void PublishMutex(Mutex* state) {
        // Creation is serialized, readers only traverse fully initialized nodes.
        auto& bucket = mutex_directory[DirectoryHash(state->address)];
        state->directory_next = bucket.load(std::memory_order_relaxed);
        bucket.store(state, std::memory_order_release);
    }
    std::map<u64, AttributeState> attributes;
    struct Waiter {
        u64 owner{};
        u64 mutex{};
        bool notified{};
        bool reacquiring{};
        std::condition_variable changed;
    };
    struct Cond {
        // Shared ownership lets a notifier signal a selected waiter after the
        // condition guard is released, even if that waiter has already left.
        std::vector<std::shared_ptr<Waiter>> waiters;
        u32 clock{};
        bool retired{};
        std::mutex guard;
    };
    std::map<u64, std::shared_ptr<Cond>> conditions;
    std::map<u64, u32> condition_attributes;
    GuestClock clock;
    GuestAddressWaiters waiters;
    int CondCreate(u64 slot, u32 clock_id = 0) {
        if (!Writable(slot, 8))
            return POSIX_EFAULT;
        const u64 addr = allocate();
        if (!addr) return POSIX_ENOMEM;
        auto state = std::make_shared<Cond>();
        state->clock = clock_id;
        conditions.emplace(addr, std::move(state));
        Write(slot, addr);
        return 0;
    }

    template <class T>
    T Read(u64 addr) {
        T value{};
        auto s = space.ReadData(GuestCpu::GuestAddress{addr},
                                std::as_writable_bytes(std::span{&value, 1}));
        if (!s)
            throw std::runtime_error(GuestCpu::Describe(s.GetError()));
        return value;
    }
    template <class T>
    void Write(u64 addr, const T& value) {
        // Retain only this output range; never hold VM metadata across the copy.

        auto s = space.WriteData(GuestCpu::GuestAddress{addr}, std::as_bytes(std::span{&value, 1}));
        if (!s)
            throw std::runtime_error(GuestCpu::Describe(s.GetError()));
    }
    static u64 StateAddress(const Mutex& m) {
        return m.address + SHAD_SYNC_MUTEX_STATE;
    }
    Object Open(const Mutex& m) {
        auto pin = space.AcquireDataSpan({GuestCpu::GuestAddress{m.address}, sizeof(Prefix)}, true);
        if (!pin)
            throw std::runtime_error(GuestCpu::Describe(pin.GetError()));
        Object object{std::move(pin).Value(), nullptr};
        object.prefix = reinterpret_cast<Prefix*>(object.pin.WritableBytes().data());
        return object;
    }
    // Called right after winning the word. A Destroy racing this acquisition
    // either saw the word busy (and backs off) or already retired the object;
    // in the latter case the word is handed back and the caller reports EINVAL.
    bool Confirm(Mutex& m, Object& o) {
        while (m.retiring.load(std::memory_order_acquire))
            std::this_thread::yield();
        if (!m.retired.load(std::memory_order_acquire))
            return true;
        ReleaseWord(m, o);
        return false;
    }
    // Same word protocol as guest/runtime/sync/mutex.c LockCommon after the
    // self-ownership checks: exchange to contended, park while contended. A
    // cancelled park leaves the word contended, which only costs one extra wake.
    int AcquireWord(Mutex& m, u64 owner, std::stop_token cancel) {
        for (;;) {
            {
                auto o = Open(m);
                if (o.State().exchange(SHAD_SYNC_STATE_CONTENDED, std::memory_order_acquire) ==
                    SHAD_SYNC_STATE_FREE) {
                    o.prefix->owner = owner;
                    o.prefix->count = 1;
                    return Confirm(m, o) ? 0 : POSIX_EINVAL;
                }
            }
            SyncMetrics::Phase park_phase{SyncMetrics::Stage::Park};
            const int r = waiters.Wait(StateAddress(m), SHAD_SYNC_STATE_CONTENDED, 4, cancel);
            if (r)
                return r;
        }
    }
    // Guest unlock sequence: clear owner/count, release the word, then wake one
    // parked contender if the word was contended. Caller verified ownership.
    void ReleaseWord(Mutex& m, Object& o) {
        o.prefix->count = 0;
        o.prefix->owner = 0;
        const bool wake = o.State().exchange(SHAD_SYNC_STATE_FREE, std::memory_order_release) ==
                          SHAD_SYNC_STATE_CONTENDED;
        o.pin = {};
        if (wake)
            waiters.Wake(StateAddress(m), 1);
    }
    bool Writable(u64 addr, u64 size) {
        return bool(space.ValidateRange({GuestCpu::GuestAddress{addr}, size},
                                        GuestCpu::GuestPermission::Write));
    }
    int Create(u64 slot, u32 type, u32 protocol = 0) {
        if (!Writable(slot, sizeof(u64)))
            return POSIX_EFAULT;
        const u64 addr = allocate();
        if (!addr) return POSIX_ENOMEM;
        Prefix prefix{};
        prefix.flags = type;
        prefix.protocol = protocol;
        Write(addr, prefix);
        auto state = std::make_unique<Mutex>();
        state->address = addr;
        state->type = type;
        state->protocol = protocol;
        auto* published = state.get();
        mutex_storage.push_back(std::move(state));
        mutexes.emplace(addr, published);
        PublishMutex(published);
        Write(slot, addr);
        return 0;
    }

    Mutex* FindMutex(u64 slot, bool initialize, int& error) {
        // Always reread the checked guest slot. Only immutable native state
        // identity is indexed, never guest mapping permissions or slot contents.
        u64 addr = Read<u64>(slot);
        if (initialize && addr < 2) {
            std::lock_guard registry(guard);
            addr = Read<u64>(slot);
            if (addr < 2) {
                if ((error = Create(slot, addr == 1 ? 4 : 1))) return nullptr;
                addr = Read<u64>(slot);
            }
        }
        auto* state = LookupMutex(addr);
        if (!state) error = POSIX_EINVAL;
        return state;
    }

public:
    GuestMutexDomain(GuestCpu::GuestAddressSpace& space, std::function<u64()> allocate)
        : space(space), allocate(std::move(allocate)), waiters(space) {}
    // Shared with the runtime's wait/wake HLE primitives used by the guest fast path.
    GuestAddressWaiters& Waiters() {
        return waiters;
    }

    int AttributeInit(u64 slot) {
        std::lock_guard lock(guard);
        if (!Writable(slot, 8))
            return POSIX_EFAULT;
        const u64 addr = allocate();
        if (!addr) return POSIX_ENOMEM;
        Write(addr, AttributeState{});
        attributes.emplace(addr, AttributeState{});
        Write(slot, addr);
        return 0;
    }
    int Attribute(u64 slot, u64 value, int action) {
        std::lock_guard lock(guard);
        const auto addr = Read<u64>(slot);
        auto it = attributes.find(addr);
        if (it == attributes.end())
            return POSIX_EINVAL;
        if (action == 0) {
            if (!Writable(slot, 8))
                return POSIX_EFAULT;
            Write(slot, u64{0});
            attributes.erase(it);
            return 0;
        }
        if (action == 1) {
            if (value < 1 || value > 4)
                return POSIX_EINVAL;
            Write(addr, static_cast<u32>(value));
            it->second.type = value;
            return 0;
        }
        if (!Writable(value, 4))
            return POSIX_EFAULT;
        Write(value, it->second.type);
        return 0;
    }
    int Init(u64 slot, u64 attribute_slot) {
        std::lock_guard lock(guard);
        AttributeState value;
        if (attribute_slot) {
            auto it = attributes.find(Read<u64>(attribute_slot));
            if (it == attributes.end())
                return POSIX_EINVAL;
            value = it->second;
        }
        if (mutexes.contains(Read<u64>(slot)))
            return POSIX_EBUSY;
        return Create(slot, value.type, value.protocol);
    }
    int AttributePolicy(u64 slot, u64 value, bool get, bool shared) {
        std::lock_guard lock(guard);
        const auto address = Read<u64>(slot);
        auto it = attributes.find(address);
        if (it == attributes.end())
            return POSIX_EINVAL;
        if (get) {
            if (!Writable(value, 4)) return POSIX_EFAULT;
            Write(value, shared ? u32{0} : it->second.protocol);
            return 0;
        }
        if (shared) return value ? POSIX_EINVAL : 0;
        if (value > 2) return POSIX_EINVAL;
        // Match desktop mutex.cpp: protocol is copied into the real lock and
        // non-None locks skip adaptive spinning. All our waits already block.
        // Neither backend implements real-time priority donation or scheduling.
        auto updated = it->second;
        updated.protocol = value;
        updated.ceiling = 767;
        Write(address, updated);
        it->second = updated;
        return 0;
    }
    int AttributeCeiling(u64 slot, u64 value, bool get) {
        std::lock_guard lock(guard);
        const auto address = Read<u64>(slot);
        auto it = attributes.find(address);
        if (it == attributes.end() || it->second.protocol != 2) return POSIX_EINVAL;
        if (get) {
            if (!Writable(value, 4)) return POSIX_EFAULT;
            Write(value, it->second.ceiling);
        } else {
            if (value < 256 || value > 767) return POSIX_EINVAL;
            auto updated = it->second;
            updated.ceiling = value;
            Write(address, updated);
            it->second = updated;
        }
        return 0;
    }
    int AttributeKind(u64 address) {
        std::lock_guard lock(guard);
        auto it = attributes.find(address);
        return it == attributes.end() ? -1 : int(it->second.type);
    }
    int Lock(u64 slot, u64 owner, bool try_only, std::stop_token cancel) {
        int error{};
        SyncMetrics::Phase lookup_phase{SyncMetrics::Stage::Lookup};
        auto state = FindMutex(slot, true, error);
        lookup_phase.End();
        if (!state) return error;
        auto& m = *state;
        if (m.retired.load(std::memory_order_acquire)) return POSIX_EINVAL;
        if (cancel.stop_requested()) return POSIX_EINTR;
        {
            SyncMetrics::Phase guard_phase{SyncMetrics::Stage::Guard};
            auto o = Open(m);
            guard_phase.End();
            SyncMetrics::Phase publish_phase{SyncMetrics::Stage::Publish};
            u32 expected = SHAD_SYNC_STATE_FREE;
            if (o.State().compare_exchange_strong(expected, SHAD_SYNC_STATE_HELD,
                                                  std::memory_order_acquire,
                                                  std::memory_order_relaxed)) {
                o.prefix->owner = owner;
                o.prefix->count = 1;
                return Confirm(m, o) ? 0 : POSIX_EINVAL;
            }
            if (o.prefix->owner == owner) {
                // Same order as the guest payload: recursive depth, try, error.
                if (m.type == SHAD_SYNC_TYPE_RECURSIVE) {
                    if (o.prefix->count == UINT32_MAX) return POSIX_EAGAIN;
                    ++o.prefix->count;
                    return 0;
                }
                if (try_only) return POSIX_EBUSY;
                if (m.type != SHAD_SYNC_TYPE_NORMAL) return POSIX_EDEADLK;
                // Normal type deadlocks on self-lock, as libthr/desktop do.
            }
            if (try_only) return POSIX_EBUSY;
        }
        return AcquireWord(m, owner, cancel);
    }
    int Unlock(u64 slot, u64 owner) {
        int error{};
        SyncMetrics::Phase lookup_phase{SyncMetrics::Stage::Lookup};
        auto state = FindMutex(slot, false, error);
        lookup_phase.End();
        if (!state) return error;
        auto& m = *state;
        if (m.retired.load(std::memory_order_acquire)) return POSIX_EINVAL;
        SyncMetrics::Phase guard_phase{SyncMetrics::Stage::Guard};
        auto o = Open(m);
        guard_phase.End();
        if (o.prefix->owner != owner) return POSIX_EPERM;
        SyncMetrics::Phase publish_phase{SyncMetrics::Stage::Publish};
        if (o.prefix->count > 1) {
            --o.prefix->count;
            return 0;
        }
        ReleaseWord(m, o);
        return 0;
    }
    int Destroy(u64 slot) {
        std::lock_guard registry(guard);
        const auto addr = Read<u64>(slot);
        if (addr < 2) return 0;
        auto it = mutexes.find(addr);
        if (it == mutexes.end()) return POSIX_EINVAL;
        auto state = it->second;
        state->retiring.store(true, std::memory_order_release);
        bool busy{};
        {
            auto o = Open(*state);
            busy = o.prefix->owner ||
                   o.State().load(std::memory_order_acquire) != SHAD_SYNC_STATE_FREE ||
                   state->cond_waiters.load(std::memory_order_acquire) ||
                   waiters.Pending(StateAddress(*state));
        }
        if (busy || !Writable(slot, 8)) {
            state->retiring.store(false, std::memory_order_release);
            return busy ? POSIX_EBUSY : POSIX_EFAULT;
        }
        state->retired.store(true, std::memory_order_release);
        state->retiring.store(false, std::memory_order_release);
        Write(slot, u64{2});
        mutexes.erase(it);
        return 0;
    }
    enum class CondAttrOp { Init, Destroy, GetClock, SetClock, GetShared, SetShared };
    int CondAttribute(u64 slot, u64 value, CondAttrOp op) {
        std::lock_guard lock(guard);
        if (!space.ValidateRange({GuestCpu::GuestAddress{slot}, 8},
                                 GuestCpu::GuestPermission::Read))
            return POSIX_EFAULT;
        const auto address = Read<u64>(slot);
        if (op == CondAttrOp::Init) {
            if (!Writable(slot, 8))
                return POSIX_EFAULT;
            if (condition_attributes.contains(address))
                return POSIX_EBUSY;
            const u64 created = allocate();
            if (!created) return POSIX_ENOMEM;
            condition_attributes.emplace(created, 0);
            Write(slot, created);
            return 0;
        }
        const auto it = condition_attributes.find(address);
        if (it == condition_attributes.end())
            return POSIX_EINVAL;
        if (op == CondAttrOp::Destroy) {
            if (!Writable(slot, 8))
                return POSIX_EFAULT;
            Write(slot, u64{0});
            condition_attributes.erase(it);
            return 0;
        }
        if (op == CondAttrOp::SetClock) {
            // Same four clocks admitted by desktop condattr_setclock.
            if (u32(value) != 0 && u32(value) != 1 && u32(value) != 2 && u32(value) != 4)
                return POSIX_EINVAL;
            it->second = u32(value);
            return 0;
        }
        if (op == CondAttrOp::SetShared)
            return u32(value) == 0 ? 0 : POSIX_EINVAL;
        if (!Writable(value, 4))
            return POSIX_EFAULT;
        Write(value, op == CondAttrOp::GetClock ? it->second : 0u);
        return 0;
    }
    int CondInit(u64 slot, u64 attr) {
        std::lock_guard lock(guard);
        if (!space.ValidateRange({GuestCpu::GuestAddress{slot}, 8},
                                 GuestCpu::GuestPermission::Read) ||
            (attr && !space.ValidateRange({GuestCpu::GuestAddress{attr}, 8},
                                          GuestCpu::GuestPermission::Read)))
            return POSIX_EFAULT;
        u32 clock_id{};
        if (attr) {
            const auto address = Read<u64>(attr);
            if (address) {
                auto it = condition_attributes.find(address);
                if (it == condition_attributes.end())
                    return POSIX_EINVAL;
                clock_id = it->second;
            }
        }
        if (conditions.contains(Read<u64>(slot)))
            return POSIX_EBUSY;
        return CondCreate(slot, clock_id);
    }
    int CondNotify(u64 slot, bool broadcast, u64 target_owner = 0) {
        std::shared_ptr<Cond> state;
        u64 addr{};
        {
            std::lock_guard registry(guard);
            if (!space.ValidateRange({GuestCpu::GuestAddress{slot}, 8},
                                     GuestCpu::GuestPermission::Read)) return POSIX_EFAULT;
            addr = Read<u64>(slot);
            if (!addr) {
                if (int e = CondCreate(slot)) return e;
                addr = Read<u64>(slot);
            }
            const auto it = conditions.find(addr);
            if (it == conditions.end()) return POSIX_EINVAL;
            state = it->second;
        }
        std::vector<std::shared_ptr<Waiter>> selected;
        {
            std::lock_guard lock(state->guard);
            if (state->retired) return POSIX_EINVAL;
            for (const auto& waiter : state->waiters) {
                if (waiter->notified || waiter->reacquiring ||
                    (target_owner && waiter->owner != target_owner)) continue;
                waiter->notified = true;
                TraceCond(CondTraceKind::Notified, addr, waiter->mutex, waiter->owner);
                selected.push_back(waiter);
                if (!broadcast) break;
            }
        }
        // Each waiter has its own native condition. A targeted signal must
        // neither wake every owner nor accidentally wake an unselected one.
        // `notified` was published under the condition guard; notifying after
        // releasing it keeps the woken waiter from blocking on that guard.
        for (const auto& waiter : selected) waiter->changed.notify_one();
        return target_owner && selected.empty() ? POSIX_EPERM : 0;
    }
    int CondDestroy(u64 slot) {
        std::lock_guard registry(guard);
        if (!space.ValidateRange({GuestCpu::GuestAddress{slot}, 8},
                                 GuestCpu::GuestPermission::Read)) return POSIX_EFAULT;
        const auto addr = Read<u64>(slot);
        if (!addr) return 0;
        auto it = conditions.find(addr);
        if (it == conditions.end()) return POSIX_EINVAL;
        auto state = it->second;
        std::lock_guard lock(state->guard);
        if (!state->waiters.empty()) return POSIX_EBUSY;
        if (!Writable(slot, 8)) return POSIX_EFAULT;
        Write(slot, u64{1});
        state->retired = true;
        conditions.erase(it);
        return 0;
    }
    struct CondDeadline {
        std::optional<u64> relative_us;
        std::optional<std::chrono::nanoseconds> absolute;
    };
    int CondWait(u64 slot, u64 mutex_slot, u64 owner, std::stop_token cancel,
                 const CondDeadline& limit = {}) {
        std::shared_ptr<Cond> condition;
        Mutex* state{};
        u64 addr{};
        {
            std::lock_guard registry(guard);
            if (!space.ValidateRange({GuestCpu::GuestAddress{slot}, 8},
                                     GuestCpu::GuestPermission::Read) ||
                !space.ValidateRange({GuestCpu::GuestAddress{mutex_slot}, 8},
                                     GuestCpu::GuestPermission::Read)) return POSIX_EFAULT;
            addr = Read<u64>(slot);
            if (!addr) {
                if (int e = CondCreate(slot)) return e;
                addr = Read<u64>(slot);
            }
            auto c = conditions.find(addr);
            auto m = mutexes.find(Read<u64>(mutex_slot));
            if (c == conditions.end() || m == mutexes.end()) return POSIX_EINVAL;
            condition = c->second;
            state = m->second;
        }
        auto& c = *condition;
        auto& m = *state;
        auto waiter_ref = std::make_shared<Waiter>();
        waiter_ref->owner = owner;
        waiter_ref->mutex = m.address;
        auto& waiter = *waiter_ref;
        // Construct before taking the condition guard; destruction follows queue
        // removal and guard release, and precedes destruction of the waiter.
        // Mutex reacquisition parks in GuestAddressWaiters, which observes the
        // same token itself.
        std::stop_callback on_stop(cancel, [&] {
            std::lock_guard lock(c.guard);
            waiter.changed.notify_one();
        });
        std::unique_lock cond_lock(c.guard);
        if (c.retired || m.retired.load(std::memory_order_acquire)) return POSIX_EINVAL;
        u32 depth{};
        {
            // Enqueue and guest unlock are atomic with respect to notification:
            // both happen under the condition guard, and a notifier needs that
            // guard before it can select this waiter. Neither the registry nor
            // a VM pin survives the wait.
            auto o = Open(m);
            if (o.prefix->owner != owner) return POSIX_EPERM;
            c.waiters.push_back(waiter_ref);
            m.cond_waiters.fetch_add(1, std::memory_order_acq_rel);
            depth = o.prefix->count;
            ReleaseWord(m, o);
        }
        struct Waiting {
            Cond& c;
            Mutex& m;
            Waiter& waiter;
            std::unique_lock<std::mutex>& cond_lock;
            ~Waiting() {
                if (cond_lock.owns_lock()) cond_lock.unlock();
                {
                    std::lock_guard lock(c.guard);
                    std::erase_if(c.waiters, [&](const auto& item) { return item.get() == &waiter; });
                }
                m.cond_waiters.fetch_sub(1, std::memory_order_acq_rel);
            }
        } waiting{c, m, waiter, cond_lock};
        TraceCond(CondTraceKind::Enqueued, addr, m.address, owner);
        const auto ready = [&] { return waiter.notified || cancel.stop_requested(); };
        int result{};
        SyncMetrics::Phase park_phase{SyncMetrics::Stage::Park};
        if (limit.relative_us) {
            const auto now = std::chrono::steady_clock::now();
            const auto available = std::chrono::duration_cast<std::chrono::microseconds>(
                                       std::chrono::steady_clock::time_point::max() - now).count();
            const auto deadline = *limit.relative_us > u64(available)
                                      ? std::chrono::steady_clock::time_point::max()
                                      : now + std::chrono::microseconds(*limit.relative_us);
            if (!waiter.changed.wait_until(cond_lock, deadline, ready)) result = POSIX_ETIMEDOUT;
        } else if (limit.absolute) {
            while (!ready()) {
                Libraries::Kernel::OrbisKernelTimespec current{};
                std::chrono::nanoseconds now{};
                result = clock.Read(c.clock, current, false);
                if (result) break;
                if (!GuestClock::Duration(current, now)) { result = POSIX_EINVAL; break; }
                if (now >= *limit.absolute) { result = POSIX_ETIMEDOUT; break; }
                // Recheck non-monotonic/virtual clocks as before.
                waiter.changed.wait_for(cond_lock,
                    std::min(*limit.absolute - now,
                             std::chrono::nanoseconds(std::chrono::milliseconds(20))), ready);
            }
        } else {
            waiter.changed.wait(cond_lock, ready);
        }
        park_phase.End();
        TraceCond(CondTraceKind::Resumed, addr, m.address, owner,
                  cancel.stop_requested() ? POSIX_EINTR : result);
        waiter.reacquiring = true;
        cond_lock.unlock();
        SyncMetrics::Phase reacquire_phase{SyncMetrics::Stage::Reacquire};
        // Session cancellation is terminal and must not wait for a stopped
        // owner: a free word is still taken, a held one is not waited for.
        const int reacquired = AcquireWord(m, owner, cancel);
        if (!reacquired) {
            auto o = Open(m);
            o.prefix->count = depth;
        }
        reacquire_phase.End();
        TraceCond(CondTraceKind::Reacquired, addr, m.address, owner,
                  cancel.stop_requested() ? POSIX_EINTR : result);
        return cancel.stop_requested() ? POSIX_EINTR : result;
    }
    // Lock contenders parked on the word plus condition waiters queued against
    // this mutex (they reacquire it later).
    size_t PendingWaits(u64 slot) {
        int error{};
        auto state = FindMutex(slot, false, error);
        if (!state || state->retired.load(std::memory_order_acquire)) return 0;
        return waiters.Pending(StateAddress(*state)) +
               state->cond_waiters.load(std::memory_order_acquire);
    }
    size_t PendingReacquires(u64 slot) {
        std::shared_ptr<Cond> state;
        {
            std::lock_guard registry(guard);
            auto it = conditions.find(Read<u64>(slot));
            if (it == conditions.end()) return 0;
            state = it->second;
        }
        std::lock_guard lock(state->guard);
        return std::count_if(state->waiters.begin(), state->waiters.end(),
                             [](const auto& waiter) { return waiter->reacquiring; });
    }
    int IsOwned(u64 slot, u64 owner) {
        int error{};
        auto state = FindMutex(slot, false, error);
        if (!state || state->retired.load(std::memory_order_acquire)) return 0;
        auto o = Open(*state);
        return o.prefix->owner == owner;
    }

};
} // namespace Core::HostRuntime
