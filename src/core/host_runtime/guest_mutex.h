// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once

#include <algorithm>
#include <condition_variable>
#include <functional>
#include <map>
#include <mutex>
#include <stop_token>
#include <vector>
#include "core/guest_cpu/api/address_space.h"
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
        u32 depth{}, type{1}, waiters{};
    };
    GuestCpu::GuestAddressSpace& space;
    std::function<u64()> allocate;
    std::mutex guard;
    std::condition_variable_any changed;
    std::map<u64, Mutex> mutexes;
    std::map<u64, u32> attributes;
    struct Waiter {
        bool notified{};
        bool reacquiring{};
    };
    struct Cond {
        std::vector<Waiter*> waiters;
    };
    std::map<u64, Cond> conditions;
    size_t allocations{};
    int CondCreate(u64 slot) {
        if (!Writable(slot, 8))
            return POSIX_EFAULT;
        if (allocations >= 4096)
            return POSIX_ENOMEM;
        const u64 addr = allocate();
        ++allocations;
        conditions.emplace(addr, Cond{});
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
        auto s = space.Write(GuestCpu::GuestAddress{addr}, std::as_bytes(std::span{&value, 1}));
        if (!s)
            throw std::runtime_error(GuestCpu::Describe(s.GetError()));
    }
    bool Writable(u64 addr, u64 size) {
        return bool(space.ValidateRange({GuestCpu::GuestAddress{addr}, size},
                                        GuestCpu::GuestPermission::Write));
    }
    int Create(u64 slot, u32 type) {
        if (!Writable(slot, sizeof(u64)))
            return POSIX_EFAULT;
        if (allocations >= 4096)
            return POSIX_ENOMEM;
        const u64 addr = allocate();
        ++allocations; // Includes destroyed objects: bound generation memory use.
        Prefix prefix{};
        prefix.flags = type;
        Write(addr, prefix);
        mutexes.emplace(addr, Mutex{.address = addr, .type = type});
        Write(slot, addr);
        return 0;
    }

public:
    GuestMutexDomain(GuestCpu::GuestAddressSpace& space, std::function<u64()> allocate)
        : space(space), allocate(std::move(allocate)) {}

    int AttributeInit(u64 slot) {
        std::lock_guard lock(guard);
        if (!Writable(slot, 8))
            return POSIX_EFAULT;
        if (allocations >= 4096)
            return POSIX_ENOMEM;
        const u64 addr = allocate();
        ++allocations;
        Write(addr, u32{1});
        attributes.emplace(addr, 1);
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
            it->second = value;
            return 0;
        }
        if (!Writable(value, 4))
            return POSIX_EFAULT;
        Write(value, it->second);
        return 0;
    }
    int Init(u64 slot, u64 attribute_slot) {
        std::lock_guard lock(guard);
        u32 type = 1;
        if (attribute_slot) {
            auto it = attributes.find(Read<u64>(attribute_slot));
            if (it == attributes.end())
                return POSIX_EINVAL;
            type = it->second;
        }
        if (mutexes.contains(Read<u64>(slot)))
            return POSIX_EBUSY;
        return Create(slot, type);
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
        const bool ready = changed.wait(lock, cancel, [&] { return m.owner == 0; });
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
            changed.notify_all();
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
    int CondInit(u64 slot, u64 attr) {
        std::lock_guard lock(guard);
        if (attr && Read<u64>(attr))
            return POSIX_ENOTSUP;
        if (conditions.contains(Read<u64>(slot)))
            return POSIX_EBUSY;
        return CondCreate(slot);
    }
    int CondNotify(u64 slot, bool broadcast) {
        std::lock_guard lock(guard);
        auto addr = Read<u64>(slot);
        if (!addr) {
            if (int e = CondCreate(slot))
                return e;
            addr = Read<u64>(slot);
        }
        auto it = conditions.find(addr);
        if (it == conditions.end())
            return POSIX_EINVAL;
        for (auto* waiter : it->second.waiters) {
            if (waiter->notified)
                continue;
            waiter->notified = true;
            if (!broadcast)
                break;
        }
        changed.notify_all();
        return 0;
    }
    int CondDestroy(u64 slot) {
        std::lock_guard lock(guard);
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
    int CondWait(u64 slot, u64 mutex_slot, u64 owner, std::stop_token cancel) {
        std::unique_lock lock(guard);
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
        Waiter waiter;
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
        changed.notify_all();
        changed.wait(lock, cancel, [&] { return waiter.notified; });
        // Retain the condition's waiter until reacquisition finishes. Otherwise
        // CondDestroy can erase the Cond while Waiting still refers to it.
        waiter.reacquiring = true;
        // A generation Stop is terminal, not a guest pthread_cancel request.
        // It must not deadlock reacquiring a mutex whose owner is also stopped.
        bool acquired = changed.wait(lock, cancel, [&] { return mutex.owner == 0; });
        if (acquired) {
            Write(mutex.address, owner);
            Write(mutex.address + 8, depth);
            mutex.owner = owner;
            mutex.depth = depth;
        }
        return cancel.stop_requested() ? POSIX_EINTR : 0;
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
