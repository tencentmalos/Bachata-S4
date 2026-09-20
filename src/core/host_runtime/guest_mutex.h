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
#include <vector>
#include "core/guest_cpu/api/address_space.h"
#include "core/host_runtime/guest_clock.h"
#include "core/libraries/kernel/posix_error.h"

namespace Core::HostRuntime {
// Orbis mutex ownership stays on the host. libc is allowed to update the ABI
// prefix flags at +0x20, so the published handle addresses a guest allocation,
// never a native mutex, std::string, pthread pointer or an unmapped integer.
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
        u64 reserved{};
        u32 flags{};
    };
    static_assert(offsetof(Prefix, flags) == 0x20);
    struct Mutex {
        u64 address{}, owner{};
        u32 depth{}, type{1}, waiters{}, protocol{};
        bool retired{};
        Mutex* directory_next{}; // immutable after release publication
        // std::mutex / condition_variable directly use Bionic pthread primitives
        // on Android. Never hold this physical-thread lock across guest execution.
        std::mutex guard;
        std::condition_variable changed;
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
        std::vector<Waiter*> waiters;
        u32 clock{};
        bool retired{};
        std::mutex guard;
    };
    std::map<u64, std::shared_ptr<Cond>> conditions;
    std::map<u64, u32> condition_attributes;
    GuestClock clock;
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
    void WriteOwnership(u64 address, u64 owner, u32 depth) {
        // One checked 12-byte write. Preserve libc's
        // spin/yield/protocol/flags fields; only the first 12 ABI bytes change.
        std::array<std::byte, 12> bytes;
        std::memcpy(bytes.data(), &owner, sizeof(owner));
        std::memcpy(bytes.data() + sizeof(owner), &depth, sizeof(depth));
        Write(address, bytes);
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
        : space(space), allocate(std::move(allocate)) {}

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
        // The callback is destroyed AFTER lock unlocks. It takes the same mutex
        // as the predicate, preventing the check-to-sleep lost-wake window.
        auto wake = [&m] {
            std::lock_guard guard(m.guard);
            m.changed.notify_all();
        };
        std::optional<std::stop_callback<decltype(wake)>> on_stop;
        SyncMetrics::Phase guard_phase{SyncMetrics::Stage::Guard};
        std::unique_lock lock(m.guard);
        guard_phase.End();
        if (m.retired) return POSIX_EINVAL;
        if (m.owner == owner) {
            if (m.type == 2) {
                if (m.depth == UINT32_MAX) return POSIX_EAGAIN;
                Write(m.address + 8, m.depth + 1);
                ++m.depth;
                return 0;
            }
            if (try_only) return POSIX_EBUSY;
            if (m.type != 3) return POSIX_EDEADLK;
        }
        if (m.owner && try_only) return POSIX_EBUSY;
        ++m.waiters;
        struct Waiting {
            Mutex& m;
            ~Waiting() {
                --m.waiters;
                // If a selected waiter cancels or fails publication, pass the
                // available lock to another waiter rather than strand it.
                if (!m.owner && m.waiters) m.changed.notify_one();
            }
        } waiting{m};
        if (m.owner) {
            if (cancel.stop_possible()) {
                // Registration can synchronously invoke wake for a pre-cancelled
                // token. Drop the object lock first; waiter count pins lifetime.
                lock.unlock();
                on_stop.emplace(cancel, wake);
                lock.lock();
            }
            SyncMetrics::Phase park_phase{SyncMetrics::Stage::Park};
            m.changed.wait(lock, [&] { return !m.owner || cancel.stop_requested(); });
        }
        if (cancel.stop_requested()) return POSIX_EINTR;
        SyncMetrics::Phase publish_phase{SyncMetrics::Stage::Publish};
        WriteOwnership(m.address, owner, 1);
        publish_phase.End();
        m.owner = owner;
        m.depth = 1;
        return 0;
    }
    int Unlock(u64 slot, u64 owner) {
        int error{};
        SyncMetrics::Phase lookup_phase{SyncMetrics::Stage::Lookup};
        auto state = FindMutex(slot, false, error);
        lookup_phase.End();
        if (!state) return error;
        auto& m = *state;
        SyncMetrics::Phase guard_phase{SyncMetrics::Stage::Guard};
        std::lock_guard lock(m.guard);
        guard_phase.End();
        if (m.retired) return POSIX_EINVAL;
        if (m.owner != owner) return POSIX_EPERM;
        SyncMetrics::Phase publish_phase{SyncMetrics::Stage::Publish};
        WriteOwnership(m.address, m.depth == 1 ? 0 : owner, m.depth - 1);
        publish_phase.End();
        if (--m.depth == 0) {
            m.owner = 0;
            if (m.waiters) m.changed.notify_one();
        }
        return 0;
    }
    int Destroy(u64 slot) {
        std::lock_guard registry(guard);
        const auto addr = Read<u64>(slot);
        if (addr < 2) return 0;
        auto it = mutexes.find(addr);
        if (it == mutexes.end()) return POSIX_EINVAL;
        auto state = it->second;
        std::lock_guard lock(state->guard);
        if (state->owner || state->waiters) return POSIX_EBUSY;
        if (!Writable(slot, 8)) return POSIX_EFAULT;
        Write(slot, u64{2});
        state->retired = true;
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
        std::lock_guard lock(state->guard);
        if (state->retired) return POSIX_EINVAL;
        bool notified = false;
        for (auto* waiter : state->waiters) {
            if (waiter->notified || waiter->reacquiring ||
                (target_owner && waiter->owner != target_owner)) continue;
            waiter->notified = true;
            TraceCond(CondTraceKind::Notified, addr, waiter->mutex, waiter->owner);
            // Each waiter has its own native condition. A targeted signal must
            // neither wake every owner nor accidentally wake an unselected one.
            waiter->changed.notify_one();
            notified = true;
            if (!broadcast) break;
        }
        return target_owner && !notified ? POSIX_EPERM : 0;
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
        Waiter waiter{.owner = owner, .mutex = m.address};
        // Construct before taking either guard; destruction follows queue
        // removal and guard release, and precedes destruction of the waiter.
        std::stop_callback on_stop(cancel, [&] {
            {
                std::lock_guard lock(c.guard);
                waiter.changed.notify_one();
            }
            {
                std::lock_guard lock(m.guard);
                m.changed.notify_all();
            }
        });
        std::unique_lock cond_lock(c.guard, std::defer_lock);
        std::unique_lock mutex_lock(m.guard, std::defer_lock);
        std::lock(cond_lock, mutex_lock);
        if (c.retired || m.retired) return POSIX_EINVAL;
        if (m.owner != owner) return POSIX_EPERM;
        c.waiters.push_back(&waiter);
        ++m.waiters;
        struct Waiting {
            Cond& c;
            Mutex& m;
            Waiter& waiter;
            std::unique_lock<std::mutex>& cond_lock;
            std::unique_lock<std::mutex>& mutex_lock;
            ~Waiting() {
                if (cond_lock.owns_lock()) cond_lock.unlock();
                if (mutex_lock.owns_lock()) mutex_lock.unlock();
                std::scoped_lock lock(c.guard, m.guard);
                std::erase(c.waiters, &waiter);
                --m.waiters;
                if (!m.owner && m.waiters) m.changed.notify_one();
            }
        } waiting{c, m, waiter, cond_lock, mutex_lock};
        const auto depth = m.depth;
        // Enqueue and guest unlock are atomic with respect to notification and
        // acquisition. Neither the registry nor a VM pin survives the wait.
        WriteOwnership(m.address, 0, 0);
        m.owner = 0;
        m.depth = 0;
        m.changed.notify_one();
        mutex_lock.unlock();
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
        mutex_lock.lock();
        // Session cancellation is terminal and must not wait for a stopped owner.
        m.changed.wait(mutex_lock, [&] { return !m.owner || cancel.stop_requested(); });
        if (!m.owner) {
            WriteOwnership(m.address, owner, depth);
            m.owner = owner;
            m.depth = depth;
        }
        reacquire_phase.End();
        TraceCond(CondTraceKind::Reacquired, addr, m.address, owner,
                  cancel.stop_requested() ? POSIX_EINTR : result);
        return cancel.stop_requested() ? POSIX_EINTR : result;
    }
    size_t PendingWaits(u64 slot) {
        int error{};
        auto state = FindMutex(slot, false, error);
        if (!state) return 0;
        std::lock_guard lock(state->guard);
        return state->retired ? 0 : state->waiters;
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
                             [](auto* waiter) { return waiter->reacquiring; });
    }
    int IsOwned(u64 slot, u64 owner) {
        int error{};
        auto state = FindMutex(slot, false, error);
        if (!state) return 0;
        std::lock_guard lock(state->guard);
        return !state->retired && state->owner == owner;
    }

};
} // namespace Core::HostRuntime
