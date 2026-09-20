// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once
#include "core/host_runtime/guest_sync_metrics.h"
#include "common/types.h"
#include <chrono>
#include <condition_variable>
#include <cstring>
#include <functional>
#include <map>
#include <mutex>
#include <optional>
#include <stop_token>
#include "core/guest_cpu/api/address_space.h"
#include "core/libraries/kernel/posix_error.h"

namespace Core::HostRuntime {
// Session-owned sem_t handles refer to guest allocations, never host objects.
// No guest pin survives a wait; destroy refuses objects with active waiters.
class GuestSemaphoreDomain final {
    struct Semaphore {
        u32 value{}, waiters{};
        std::condition_variable_any changed;
    };
    GuestCpu::GuestAddressSpace& space;
    std::function<u64()> allocate;
    std::mutex guard;
    std::map<u64, Semaphore> objects;
    size_t allocations{};
    std::optional<u64> Handle(u64 slot) {
        u64 handle{};
        if (!space.ReadData(GuestCpu::GuestAddress{slot},
                            std::as_writable_bytes(std::span{&handle, 1})))
            return {};
        return handle;
    }

public:
    static constexpr u32 MaxValue = 0x7fffffff;
    GuestSemaphoreDomain(GuestCpu::GuestAddressSpace& space, std::function<u64()> allocate)
        : space(space), allocate(std::move(allocate)) {}
    int Init(u64 slot, s32 shared, u32 value) {
        if (shared != 0 || value > MaxValue)
            return POSIX_EINVAL;
        if (!space.ValidateRange({GuestCpu::GuestAddress{slot}, 8},
                                 GuestCpu::GuestPermission::Write))
            return POSIX_EFAULT;
        std::lock_guard lock(guard);
        auto previous = Handle(slot);
        if (!previous)
            return POSIX_EFAULT;
        if (objects.contains(*previous))
            return POSIX_EBUSY;
        if (allocations >= 4096)
            return POSIX_ENOMEM;
        const u64 handle = allocate(); // May quiesce VM: acquire output pin afterward.
        ++allocations;
        auto output = space.AcquireDataSpan({GuestCpu::GuestAddress{slot}, 8}, true);
        if (!output)
            return POSIX_EFAULT;
        objects.try_emplace(handle).first->second.value = value;
        std::memcpy(output.Value().WritableBytes().data(), &handle, sizeof(handle));
        return 0;
    }
    int Destroy(u64 slot) {
        std::lock_guard lock(guard);
        auto output = space.AcquireDataSpan({GuestCpu::GuestAddress{slot}, 8}, true);
        if (!output)
            return POSIX_EFAULT;
        u64 handle{};
        std::memcpy(&handle, output.Value().Bytes().data(), sizeof(handle));
        auto it = objects.find(handle);
        if (it == objects.end())
            return POSIX_EINVAL;
        if (it->second.waiters)
            return POSIX_EBUSY;
        objects.erase(it);
        handle = 0;
        std::memcpy(output.Value().WritableBytes().data(), &handle, sizeof(handle));
        return 0;
    }
    int Post(u64 slot) {
        SyncMetrics::Phase guard_phase{SyncMetrics::Stage::Guard};
        std::lock_guard lock(guard);
        guard_phase.End();
        auto handle = Handle(slot);
        if (!handle)
            return POSIX_EFAULT;
        auto it = objects.find(*handle);
        if (it == objects.end())
            return POSIX_EINVAL;
        if (it->second.value == MaxValue)
            return POSIX_EOVERFLOW;
        ++it->second.value;
        it->second.changed.notify_one();
        return 0;
    }
    int GetValue(u64 slot, u64 address) {
        std::lock_guard lock(guard);
        auto handle = Handle(slot);
        if (!handle)
            return POSIX_EFAULT;
        auto it = objects.find(*handle);
        if (it == objects.end())
            return POSIX_EINVAL;
        auto output = space.AcquireDataSpan({GuestCpu::GuestAddress{address}, 4}, true);
        if (!output)
            return POSIX_EFAULT;
        std::memcpy(output.Value().WritableBytes().data(), &it->second.value, 4);
        return 0;
    }
    u32 Pending(u64 slot) {
        std::lock_guard lock(guard);
        auto handle = Handle(slot);
        if (!handle) return 0;
        auto it = objects.find(*handle);
        return it == objects.end() ? 0 : it->second.waiters;
    }
    int Wait(u64 slot, bool try_only, std::stop_token cancel,
             std::optional<std::chrono::steady_clock::time_point> deadline = {}) {
        SyncMetrics::Phase guard_phase{SyncMetrics::Stage::Guard};
        std::unique_lock lock(guard);
        guard_phase.End();
        auto handle = Handle(slot);
        if (!handle)
            return POSIX_EFAULT;
        auto it = objects.find(*handle);
        if (it == objects.end())
            return POSIX_EINVAL;
        auto& sem = it->second;
        if (cancel.stop_requested())
            return POSIX_EINTR;
        if (sem.value) {
            --sem.value;
            return 0;
        }
        if (try_only)
            return POSIX_EAGAIN;
        ++sem.waiters;
        const auto ready = [&] { return sem.value != 0; };
        SyncMetrics::Phase park_phase{SyncMetrics::Stage::Park};
        bool acquired = deadline ? sem.changed.wait_until(lock, cancel, *deadline, ready)
                                 : sem.changed.wait(lock, cancel, ready);
        park_phase.End();
        --sem.waiters;
        if (cancel.stop_requested()) {
            // A selected waiter can cancel after Post made a token available.
            // Transfer that wake while holding the predicate guard.
            if (sem.value && sem.waiters) sem.changed.notify_one();
            return POSIX_EINTR;
        }
        if (!acquired)
            return POSIX_ETIMEDOUT;
        --sem.value;
        return 0;
    }
};
} // namespace Core::HostRuntime
