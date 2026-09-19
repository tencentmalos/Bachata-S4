// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once

#include <algorithm>
#include <condition_variable>
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
        // Desktop blocks on this mutex's own native lock. Unrelated guest
        // locks/conditions must not wake for every unlock in the process.
        std::unique_ptr<std::condition_variable_any> changed =
            std::make_unique<std::condition_variable_any>();
    };
    struct AttributeState {
        u32 type{1}, protocol{}, ceiling{};
    };
    GuestCpu::GuestAddressSpace& space;
    std::function<u64()> allocate;
    std::recursive_mutex* vm_mutex{};
    std::mutex guard;
    std::map<u64, Mutex> mutexes;
    std::map<u64, AttributeState> attributes;
    struct Waiter {
        u64 owner{};
        bool notified{};
        bool reacquiring{};
    };
    struct Cond {
        std::vector<Waiter*> waiters;
        u32 clock{};
        std::unique_ptr<std::condition_variable_any> changed =
            std::make_unique<std::condition_variable_any>();
    };
    std::map<u64, Cond> conditions;
    std::map<u64, u32> condition_attributes;
    GuestClock clock;
    size_t allocations{};
    int CondCreate(u64 slot, u32 clock_id = 0) {
        if (!Writable(slot, 8))
            return POSIX_EFAULT;
        if (allocations >= 4096)
            return POSIX_ENOMEM;
        const u64 addr = allocate();
        ++allocations;
        conditions.emplace(addr, Cond{.clock = clock_id});
        Write(slot, addr);
        return 0;
    }

    template <class T>
    T Read(u64 addr) {
        T value{};
        auto s =
            space.Read(GuestCpu::GuestAddress{addr}, std::as_writable_bytes(std::span{&value, 1}));
        if (!s)
            throw std::runtime_error(GuestCpu::Describe(s.GetError()));
        return value;
    }
    template <class T>
    void Write(u64 addr, const T& value) {
        // Allocation/publication holds this same gate. Release it before any wait.
        std::unique_lock<std::recursive_mutex> vm;
        if (vm_mutex) vm = std::unique_lock(*vm_mutex);
        auto s = space.Write(GuestCpu::GuestAddress{addr}, std::as_bytes(std::span{&value, 1}));
        if (!s)
            throw std::runtime_error(GuestCpu::Describe(s.GetError()));
    }
    bool Writable(u64 addr, u64 size) {
        return bool(space.ValidateRange({GuestCpu::GuestAddress{addr}, size},
                                        GuestCpu::GuestPermission::Write));
    }
    int Create(u64 slot, u32 type, u32 protocol = 0) {
        if (!Writable(slot, sizeof(u64)))
            return POSIX_EFAULT;
        if (allocations >= 4096)
            return POSIX_ENOMEM;
        const u64 addr = allocate();
        ++allocations; // Includes destroyed objects: bound generation memory use.
        Prefix prefix{};
        prefix.flags = type;
        prefix.protocol = protocol;
        Write(addr, prefix);
        mutexes.emplace(addr, Mutex{.address = addr, .type = type, .protocol = protocol});
        Write(slot, addr);
        return 0;
    }

public:
    GuestMutexDomain(GuestCpu::GuestAddressSpace& space, std::function<u64()> allocate,
                      std::recursive_mutex* vm_mutex = nullptr)
        : space(space), allocate(std::move(allocate)), vm_mutex(vm_mutex) {}

    int AttributeInit(u64 slot) {
        std::lock_guard lock(guard);
        if (!Writable(slot, 8))
            return POSIX_EFAULT;
        if (allocations >= 4096)
            return POSIX_ENOMEM;
        const u64 addr = allocate();
        ++allocations;
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
        std::unique_lock lock(guard);
        u64 addr = Read<u64>(slot);
        if (addr < 2) {
            if (int e = Create(slot, addr == 1 ? 4 : 1))
                return e;
            addr = Read<u64>(slot);
        }
        auto it = mutexes.find(addr);
        if (it == mutexes.end())
            return POSIX_EINVAL;
        auto& m = it->second;
        if (m.owner == owner) {
            if (m.type == 2) {
                if (m.depth == UINT32_MAX)
                    return POSIX_EAGAIN;
                Write(addr + 8, m.depth + 1);
                ++m.depth;
                return 0;
            }
            if (try_only)
                return POSIX_EBUSY;
            if (m.type != 3)
                return POSIX_EDEADLK;
        }
        if (m.owner && try_only)
            return POSIX_EBUSY;
        ++m.waiters;
        struct Waiting {
            Mutex& mutex;
            ~Waiting() {
                --mutex.waiters;
            }
        } waiting{m};
        const bool ready = !m.owner || m.changed->wait(lock, cancel, [&] { return m.owner == 0; });
        if (!ready || cancel.stop_requested())
            return POSIX_EINTR;
        Write(addr, owner);
        Write(addr + 8, u32{1});
        m.owner = owner;
        m.depth = 1;
        return 0;
    }
    int Unlock(u64 slot, u64 owner) {
        std::lock_guard lock(guard);
        auto it = mutexes.find(Read<u64>(slot));
        if (it == mutexes.end())
            return POSIX_EINVAL;
        auto& m = it->second;
        if (m.owner != owner)
            return POSIX_EPERM;
        Write(m.address + 8, m.depth - 1);
        if (m.depth == 1)
            Write(m.address, u64{0});
        if (--m.depth == 0) {
            m.owner = 0;
            if (m.waiters)
                m.changed->notify_all();
        }
        return 0;
    }
    int Destroy(u64 slot) {
        std::lock_guard lock(guard);
        const auto addr = Read<u64>(slot);
        if (addr < 2)
            return 0;
        auto it = mutexes.find(addr);
        if (it == mutexes.end())
            return POSIX_EINVAL;
        if (it->second.owner || it->second.waiters)
            return POSIX_EBUSY;
        if (!Writable(slot, 8))
            return POSIX_EFAULT;
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
            if (allocations >= 4096)
                return POSIX_ENOMEM;
            const u64 created = allocate();
            ++allocations;
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
        std::lock_guard lock(guard);
        if (!space.ValidateRange({GuestCpu::GuestAddress{slot}, 8},
                                 GuestCpu::GuestPermission::Read))
            return POSIX_EFAULT;
        auto addr = Read<u64>(slot);
        if (!addr) {
            if (int e = CondCreate(slot))
                return e;
            addr = Read<u64>(slot);
        }
        auto it = conditions.find(addr);
        if (it == conditions.end())
            return POSIX_EINVAL;
        bool notified = false;
        for (auto* waiter : it->second.waiters) {
            if (waiter->notified || waiter->reacquiring ||
                (target_owner && waiter->owner != target_owner))
                continue;
            waiter->notified = true;
            notified = true;
            if (!broadcast)
                break;
        }
        if (notified)
            it->second.changed->notify_all();
        return target_owner && !notified ? POSIX_EPERM : 0; // desktop returns 1 if not waiting
    }
    int CondDestroy(u64 slot) {
        std::lock_guard lock(guard);
        if (!space.ValidateRange({GuestCpu::GuestAddress{slot}, 8},
                                 GuestCpu::GuestPermission::Read))
            return POSIX_EFAULT;
        const auto addr = Read<u64>(slot);
        if (!addr)
            return 0;
        auto it = conditions.find(addr);
        if (it == conditions.end())
            return POSIX_EINVAL;
        if (!it->second.waiters.empty())
            return POSIX_EBUSY;
        if (!Writable(slot, 8))
            return POSIX_EFAULT;
        Write(slot, u64{1});
        conditions.erase(it);
        return 0;
    }
    struct CondDeadline {
        std::optional<u64> relative_us;
        std::optional<std::chrono::nanoseconds> absolute;
    };
    int CondWait(u64 slot, u64 mutex_slot, u64 owner, std::stop_token cancel,
                 const CondDeadline& limit = {}) {
        std::unique_lock lock(guard);
        if (!space.ValidateRange({GuestCpu::GuestAddress{slot}, 8},
                                 GuestCpu::GuestPermission::Read) ||
            !space.ValidateRange({GuestCpu::GuestAddress{mutex_slot}, 8},
                                 GuestCpu::GuestPermission::Read))
            return POSIX_EFAULT;
        auto addr = Read<u64>(slot);
        if (!addr) {
            if (int e = CondCreate(slot))
                return e;
            addr = Read<u64>(slot);
        }
        auto c = conditions.find(addr);
        auto m = mutexes.find(Read<u64>(mutex_slot));
        if (c == conditions.end() || m == mutexes.end())
            return POSIX_EINVAL;
        auto& mutex = m->second;
        if (mutex.owner != owner)
            return POSIX_EPERM;
        Waiter waiter{.owner = owner};
        c->second.waiters.push_back(&waiter);
        ++mutex.waiters;
        struct Waiting {
            Cond& cond;
            Waiter* waiter;
            Mutex& mutex;
            ~Waiting() {
                std::erase(cond.waiters, waiter);
                --mutex.waiters;
            }
        } waiting{c->second, &waiter, mutex};
        const auto depth = mutex.depth;
        // Enqueue and release atomically with respect to signal/broadcast/lock.
        Write(mutex.address, u64{0});
        Write(mutex.address + 8, u32{0});
        mutex.owner = 0;
        mutex.depth = 0;
        mutex.changed->notify_all();
        int result{};
        if (limit.relative_us) {
            const auto now = std::chrono::steady_clock::now();
            const auto available = std::chrono::duration_cast<std::chrono::microseconds>(
                                       std::chrono::steady_clock::time_point::max() - now)
                                       .count();
            const auto deadline = *limit.relative_us > u64(available)
                                      ? std::chrono::steady_clock::time_point::max()
                                      : now + std::chrono::microseconds(*limit.relative_us);
            if (!c->second.changed->wait_until(lock, cancel, deadline, [&] { return waiter.notified; }))
                result = POSIX_ETIMEDOUT;
        } else if (limit.absolute) {
            while (!waiter.notified && !cancel.stop_requested()) {
                Libraries::Kernel::OrbisKernelTimespec current{};
                std::chrono::nanoseconds now{};
                result = clock.Read(c->second.clock, current, false);
                if (result)
                    break;
                if (!GuestClock::Duration(current, now)) {
                    result = POSIX_EINVAL;
                    break;
                }
                if (now >= *limit.absolute) {
                    result = POSIX_ETIMEDOUT;
                    break;
                }
                // Re-read the selected clock to honor realtime adjustments and
                // virtual/profiling CPU time rather than treating them as monotonic.
                c->second.changed->wait_for(lock, cancel,
                                 std::min(*limit.absolute - now,
                                          std::chrono::nanoseconds(std::chrono::milliseconds(20))),
                                 [&] { return waiter.notified; });
            }
        } else {
            c->second.changed->wait(lock, cancel, [&] { return waiter.notified; });
        }
        // Retain the condition's waiter until reacquisition finishes. Otherwise
        // CondDestroy can erase the Cond while Waiting still refers to it.
        waiter.reacquiring = true;
        // A generation Stop is terminal, not a guest pthread_cancel request.
        // It must not deadlock reacquiring a mutex whose owner is also stopped.
        bool acquired = mutex.changed->wait(lock, cancel, [&] { return mutex.owner == 0; });
        if (acquired) {
            Write(mutex.address, owner);
            Write(mutex.address + 8, depth);
            mutex.owner = owner;
            mutex.depth = depth;
        }
        return cancel.stop_requested() ? POSIX_EINTR : result;
    }
    size_t PendingWaits(u64 slot) {
        std::lock_guard lock(guard);
        auto it = mutexes.find(Read<u64>(slot));
        return it == mutexes.end() ? 0 : it->second.waiters;
    }
    size_t PendingReacquires(u64 slot) {
        std::lock_guard lock(guard);
        auto it = conditions.find(Read<u64>(slot));
        if (it == conditions.end())
            return 0;
        return std::count_if(it->second.waiters.begin(), it->second.waiters.end(),
                             [](auto* waiter) { return waiter->reacquiring; });
    }
    int IsOwned(u64 slot, u64 owner) {
        std::lock_guard lock(guard);
        auto it = mutexes.find(Read<u64>(slot));
        return it != mutexes.end() && it->second.owner == owner;
    }
};
} // namespace Core::HostRuntime
