// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once
#include "core/host_runtime/guest_sync_metrics.h"
#include "common/types.h"
#include <chrono>
#include <cstring>
#include <functional>
#include <map>
#include <memory>
#include <mutex>
#include <optional>
#include <stop_token>
#include "core/guest_cpu/api/address_space.h"
#include "core/libraries/kernel/posix_error.h"
#include "core/libraries/kernel/sync/counting_semaphore.h"

namespace Core::HostRuntime {
// Session-owned sem_t handles refer to guest allocations, never host objects.
// The semaphore is the shared Libraries::Kernel::Sync::CountingSemaphore the
// desktop kernel also uses: an atomic count, a per-object FIFO of waiters and a
// single selected wake per post. The domain guard only covers the handle map.
// No guest pin survives a wait; destroy refuses objects with active waiters.
class GuestSemaphoreDomain final {
    using Semaphore = Libraries::Kernel::Sync::CountingSemaphore;
    GuestCpu::GuestAddressSpace& space;
    std::function<u64()> allocate;
    std::mutex guard;
    // Shared ownership keeps an object alive for a waiter or poster that looked
    // it up before a concurrent Destroy.
    std::map<u64, std::shared_ptr<Semaphore>> objects;
    size_t allocations{};
    std::optional<u64> Handle(u64 slot) {
        u64 handle{};
        if (!space.ReadData(GuestCpu::GuestAddress{slot},
                            std::as_writable_bytes(std::span{&handle, 1})))
            return {};
        return handle;
    }
    // EFAULT for an unreadable slot, EINVAL for a handle that is not a live semaphore.
    std::shared_ptr<Semaphore> Lookup(u64 slot, int& error) {
        auto handle = Handle(slot);
        if (!handle) {
            error = POSIX_EFAULT;
            return nullptr;
        }
        SyncMetrics::Phase guard_phase{SyncMetrics::Stage::Guard};
        std::lock_guard lock(guard);
        guard_phase.End();
        auto it = objects.find(*handle);
        if (it == objects.end()) {
            error = POSIX_EINVAL;
            return nullptr;
        }
        return it->second;
    }

public:
    static constexpr u32 MaxValue = Semaphore::MaxValue;
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
        objects.emplace(handle, std::make_shared<Semaphore>(value));
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
        if (it->second->Waiting())
            return POSIX_EBUSY;
        objects.erase(it);
        handle = 0;
        std::memcpy(output.Value().WritableBytes().data(), &handle, sizeof(handle));
        return 0;
    }
    int Post(u64 slot) {
        int error{};
        auto sem = Lookup(slot, error);
        if (!sem)
            return error;
        return sem->Post();
    }
    int GetValue(u64 slot, u64 address) {
        int error{};
        auto sem = Lookup(slot, error);
        if (!sem)
            return error;
        const u32 value = sem->Value();
        auto output = space.AcquireDataSpan({GuestCpu::GuestAddress{address}, 4}, true);
        if (!output)
            return POSIX_EFAULT;
        std::memcpy(output.Value().WritableBytes().data(), &value, 4);
        return 0;
    }
    u32 Pending(u64 slot) {
        int error{};
        auto sem = Lookup(slot, error);
        return sem ? static_cast<u32>(sem->Waiting()) : 0;
    }
    int Wait(u64 slot, bool try_only, std::stop_token cancel,
             std::optional<std::chrono::steady_clock::time_point> deadline = {}) {
        int error{};
        auto sem = Lookup(slot, error);
        if (!sem)
            return error;
        if (cancel.stop_requested())
            return POSIX_EINTR;
        if (sem->TryWait())
            return 0;
        if (try_only)
            return POSIX_EAGAIN;
        Libraries::Kernel::Sync::ParkerWait wait{std::move(cancel), deadline};
        SyncMetrics::Phase park_phase{SyncMetrics::Stage::Park};
        return sem->Wait(wait);
    }
};
} // namespace Core::HostRuntime
