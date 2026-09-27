// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once
#include "core/host_runtime/guest_sync_metrics.h"
#include <chrono>
#include <condition_variable>
#include <functional>
#include <map>
#include <memory>
#include <mutex>
#include <optional>
#include <stop_token>
#include "common/types.h"
#include "core/guest_cpu/api/address_space.h"
#include "core/libraries/kernel/posix_error.h"
#include "core/libraries/kernel/sync/rw_lock.h"

namespace Core::HostRuntime {
// Guest handles and ownership, never host pthread_rwlock_t pointers. The lock
// itself is the shared Libraries::Kernel::Sync::RwLock the desktop kernel also
// uses (owner tracking, type rules, selective wakes, a lock per object); the
// domain mutex only covers the handle and attribute maps. Waits retain no guest
// pin; VM publication can proceed meanwhile.
class GuestRwlockDomain final {
    using LockState = Libraries::Kernel::Sync::RwLock;
    GuestCpu::GuestAddressSpace& space;
    std::function<u64()> allocate;

    std::mutex mutex;
    // Shared ownership lets Unlock notify after releasing the domain mutex.
    std::map<u64, std::shared_ptr<LockState>> locks;
    std::map<u64, u32> attributes;
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
        // Addresses are session-unique tokens. Capacity follows actual backing
        // and host allocation availability, not the number ever initialized.
        address = allocate();
        if (!address) return POSIX_ENOMEM;
        try {
            if (attr) {
                if (!attributes.emplace(address, type).second) return POSIX_EINVAL;
            } else {
                auto state = std::make_shared<LockState>(type);
                if (!locks.emplace(address, std::move(state)).second) return POSIX_EINVAL;
            }
        } catch (const std::bad_alloc&) {
            return POSIX_ENOMEM;
        }
        // Publish only a fully constructed object. A failed guest write must
        // not leave a live entry or replace the caller's previous handle.
        if (int error = Write(slot, address)) {
            if (attr) attributes.erase(address);
            else locks.erase(address);
            return error;
        }
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
        if (it->second->Busy()) return POSIX_EBUSY;
        if (int e = Write(slot, u64{1})) return e;
        locks.erase(it); return 0;
    }
    int Lock(u64 slot, u64 owner, bool write, bool try_only, std::stop_token cancel,
             std::optional<std::chrono::system_clock::time_point> deadline = {}) {
        std::shared_ptr<LockState> state;
        {
            SyncMetrics::Phase guard_phase{SyncMetrics::Stage::Guard};
            std::unique_lock guard(mutex);
            guard_phase.End();
            u64 address{};
            if (!Read(slot, address)) return POSIX_EFAULT;
            if (!address) {
                if (int e = Create(slot, 0, false, address)) return e;
            }
            auto it = locks.find(address);
            if (it == locks.end()) return POSIX_EINVAL;
            state = it->second;
        }
        if (cancel.stop_requested()) return POSIX_EINTR;
        // The guest deadline is on the realtime clock; the wait runs on the steady clock.
        Libraries::Kernel::Sync::Deadline steady;
        if (deadline) {
            const auto left = *deadline - std::chrono::system_clock::now();
            steady = std::chrono::steady_clock::now() +
                     std::chrono::duration_cast<std::chrono::steady_clock::duration>(
                         std::max(left, decltype(left)::zero()));
        }
        Libraries::Kernel::Sync::ParkerWait wait{std::move(cancel), steady};
        SyncMetrics::Phase park_phase{SyncMetrics::Stage::Park};
        return state->Lock(owner, write, try_only, wait);
    }
    int Unlock(u64 slot, u64 owner) {
        std::shared_ptr<LockState> state;
        {
            SyncMetrics::Phase guard_phase{SyncMetrics::Stage::Guard};
            std::lock_guard guard(mutex);
            guard_phase.End();
            u64 address{};
            if (!Read(slot, address)) return POSIX_EFAULT;
            auto it = locks.find(address);
            if (it == locks.end()) return POSIX_EINVAL;
            state = it->second;
        }
        return state->Unlock(owner);
    }
    u32 Pending(u64 slot) {
        std::lock_guard guard(mutex);
        u64 address{};
        if (!Read(slot, address)) return 0;
        auto it = locks.find(address);
        return it == locks.end() ? 0 : it->second->Waiting();
    }
};
} // namespace Core::HostRuntime
