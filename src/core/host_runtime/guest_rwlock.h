// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once
#include <chrono>
#include <condition_variable>
#include <functional>
#include <map>
#include <mutex>
#include <optional>
#include <stop_token>
#include "common/types.h"
#include "core/guest_cpu/api/address_space.h"
#include "core/libraries/kernel/posix_error.h"

namespace Core::HostRuntime {
// Guest handles and ownership, never host pthread_rwlock_t pointers. Waits drop
// the domain lock and retain no guest pin; VM publication can proceed meanwhile.
class GuestRwlockDomain final {
    struct LockState {
        u64 writer{};
        std::map<u64, u32> readers;
        u32 type{}, waiters{}, writers{};
    };
    GuestCpu::GuestAddressSpace& space;
    std::function<u64()> allocate;

    std::mutex mutex;
    std::condition_variable_any changed;
    std::map<u64, LockState> locks;
    std::map<u64, u32> attributes;
    size_t allocations{};
    bool Read(u64 slot, u64& value) {
        return bool(space.ReadData(GuestCpu::GuestAddress{slot},
                                   std::as_writable_bytes(std::span{&value, 1})));
    }
    template<class T> int Write(u64 slot, const T& value) {
        // Retain only this output range; never hold VM metadata across the copy.

        return space.WriteData(GuestCpu::GuestAddress{slot}, std::as_bytes(std::span{&value, 1}))
                   ? 0
                   : POSIX_EFAULT;
    }
    int Create(u64 slot, u32 type, bool attr, u64& address) {
        if (!space.ValidateRange({GuestCpu::GuestAddress{slot}, 8}, GuestCpu::GuestPermission::Write)) return POSIX_EFAULT;
        if (allocations >= 4096) return POSIX_ENOMEM;
        address = allocate();
        ++allocations;
        if (int error = Write(slot, address)) return error;
        if (attr) attributes.emplace(address, type);
        else locks.emplace(address, LockState{.type = type});
        return 0;
    }
public:
    GuestRwlockDomain(GuestCpu::GuestAddressSpace& space, std::function<u64()> allocate)
        : space(space), allocate(std::move(allocate)) {}
    // actions: init, destroy, get/set pshared, get/set kind.
    int Attribute(u64 slot, int action, u64 value = 0) {
        std::lock_guard guard(mutex);
        u64 address{};
        if (!Read(slot, address)) return POSIX_EFAULT;
        auto it = attributes.find(address);
        if (action == 0) {
            if (it != attributes.end()) return POSIX_EBUSY;
            return Create(slot, 0, true, address);
        }
        if (it == attributes.end()) return POSIX_EINVAL;
        if (action == 1) {
            if (int e = Write(slot, u64{})) return e;
            attributes.erase(it); return 0;
        }
        if (action == 2) return Write(value, u32{});
        if (action == 3) return value ? POSIX_EINVAL : 0; // process-private only
        if (action == 4) return Write(value, it->second);
        if (action != 5 || value > 2) return POSIX_EINVAL;
        it->second = value; return 0;
    }
    int Init(u64 slot, u64 attr) {
        std::lock_guard guard(mutex);
        u64 address{}, a{};
        if (!Read(slot, address) || (attr && !Read(attr, a))) return POSIX_EFAULT;
        if (locks.contains(address)) return POSIX_EBUSY;
        if (attr && !attributes.contains(a)) return POSIX_EINVAL;
        return Create(slot, attr ? attributes.at(a) : 0, false, address);
    }
    int Destroy(u64 slot) {
        std::lock_guard guard(mutex);
        u64 address{};
        if (!Read(slot, address)) return POSIX_EFAULT;
        if (!address) return 0;
        auto it = locks.find(address);
        if (it == locks.end()) return POSIX_EINVAL;
        if (it->second.writer || !it->second.readers.empty() || it->second.waiters) return POSIX_EBUSY;
        if (int e = Write(slot, u64{1})) return e;
        locks.erase(it); return 0;
    }
    int Lock(u64 slot, u64 owner, bool write, bool try_only, std::stop_token cancel,
             std::optional<std::chrono::system_clock::time_point> deadline = {}) {
        std::unique_lock guard(mutex);
        u64 address{};
        if (!Read(slot, address)) return POSIX_EFAULT;
        if (!address) {
            if (int e = Create(slot, 0, false, address)) return e;
        }
        auto it = locks.find(address);
        if (it == locks.end()) return POSIX_EINVAL;
        auto& state = it->second;
        const bool reader = state.readers.contains(owner);
        if (state.writer == owner || (reader && (write || state.type == 2)))
            return try_only ? POSIX_EBUSY : POSIX_EDEADLK;
        auto ready = [&] {
            return !state.writer && (write ? state.readers.empty() :
                   (state.type == 0 || !state.writers || reader));
        };
        if (!ready()) {
            if (try_only) return POSIX_EBUSY;
            ++state.waiters;
            if (write) ++state.writers;
            struct Waiting {
                LockState& state; bool write; std::condition_variable_any& changed;
                ~Waiting() { --state.waiters; if (write) --state.writers; changed.notify_all(); }
            } waiting{state, write, changed};
            const bool acquired = deadline ? changed.wait_until(guard, cancel, *deadline, ready)
                                           : changed.wait(guard, cancel, ready);
            if (cancel.stop_requested()) return POSIX_EINTR;
            if (!acquired) return POSIX_ETIMEDOUT;
        }
        if (cancel.stop_requested()) return POSIX_EINTR;
        if (write) state.writer = owner;
        else {
            auto& depth = state.readers[owner];
            if (depth == UINT32_MAX) return POSIX_EAGAIN;
            ++depth;
        }
        return 0;
    }
    int Unlock(u64 slot, u64 owner) {
        std::lock_guard guard(mutex);
        u64 address{};
        if (!Read(slot, address)) return POSIX_EFAULT;
        auto it = locks.find(address);
        if (it == locks.end()) return POSIX_EINVAL;
        auto& state = it->second;
        if (state.writer == owner) state.writer = 0;
        else {
            auto read = state.readers.find(owner);
            if (read == state.readers.end()) return POSIX_EPERM;
            if (--read->second == 0) state.readers.erase(read);
        }
        changed.notify_all(); return 0;
    }
    u32 Pending(u64 slot) {
        std::lock_guard guard(mutex);
        u64 address{};
        if (!Read(slot, address)) return 0;
        auto it = locks.find(address);
        return it == locks.end() ? 0 : it->second.waiters;
    }
};
} // namespace Core::HostRuntime
