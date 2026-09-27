// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once
#include "core/host_runtime/guest_sync_metrics.h"
#include <array>
#include <atomic>
#include <cstring>
#include <memory>
#include <span>
#include <stop_token>
#include <string>
#include <string_view>
#include <vector>
#if defined(__ANDROID__)
#include <android/log.h>
#endif
#include "common/types.h"
#include "core/guest_cpu/api/address_space.h"
#include "core/libraries/kernel/orbis_error.h"
#include "core/libraries/kernel/sync/kernel_semaphore.h"
#include "core/libraries/kernel/sync/object_table.h"
namespace Core::HostRuntime {
// Orbis 32-bit SlotId ABI is distinct from the POSIX sem_t guest-pointer ABI.
// The semaphore itself (token accounting, waiter order, wake protocol) is the
// shared Libraries::Kernel::Sync::KernelSemaphore the desktop kernel also uses;
// this layer only decodes guest arguments, checks guest pointers and publishes
// ids. Each semaphore has its own lock; the id table is locked only for lookup.
class GuestKernelSemaphore {
    using Semaphore = Libraries::Kernel::Sync::KernelSemaphore;
    struct Failure {
        int error;
    };
    GuestCpu::GuestAddressSpace& space;
    Libraries::Kernel::Sync::ObjectTable<Semaphore> objects{4096};
    std::atomic<bool> foreground_admitted{};

    static void Trace(std::string_view nid, u32 id, const Semaphore& sem, s32 requested,
                      s32 result) {
#if defined(__ANDROID__)
        static std::atomic<u32> count{};
        // Only the startup sample is logged. Once exhausted, avoid a shared
        // atomic RMW on every semaphore operation.
        if (count.load(std::memory_order_relaxed) >= 256)
            return;
        if (nid == "188x57JYp0g" && sem.Name() != "SuspendSemaphore" &&
            sem.Name() != "ResumeSemaphore")
            return;
        const u32 index = count.fetch_add(1, std::memory_order_relaxed);
        if (index < 256) {
            const auto state = sem.Snapshot();
            __android_log_print(ANDROID_LOG_INFO, "GuestKernelSema",
                                "trace=%u nid=%.*s id=%u name=%s value=%d initial=%d max=%d requested=%d result=%d waiters=%zu",
                                index, static_cast<int>(nid.size()), nid.data(), id,
                                sem.Name().c_str(), state.value, state.initial, state.maximum,
                                requested, result, state.waiters);
        }
#else
        (void)nid;
        (void)id;
        (void)sem;
        (void)requested;
        (void)result;
#endif
    }
    static void Need(bool ok, int error) {
        if (!ok)
            throw Failure{error};
    }
    std::shared_ptr<Semaphore> Find(u32 id) {
        auto sem = objects.Find(id);
        Need(sem != nullptr, ORBIS_KERNEL_ERROR_ESRCH);
        return sem;
    }
    template <class T>
    T Read(u64 address) {
        T value{};
        Need(address && bool(space.ReadData(GuestCpu::GuestAddress{address},
                                            std::as_writable_bytes(std::span{&value, 1}))),
             ORBIS_KERNEL_ERROR_EFAULT);
        return value;
    }
    template <class T>
    void Put(u64 address, const T& value) {
        auto pin = space.AcquireDataSpan({GuestCpu::GuestAddress{address}, sizeof(T)}, true);
        Need(bool(pin), ORBIS_KERNEL_ERROR_EFAULT);
        std::memcpy(pin.Value().WritableBytes().data(), &value, sizeof(T));
    }
    bool Writable(u64 address, size_t size) {
        return address && bool(space.ValidateRange({GuestCpu::GuestAddress{address}, size},
                                                   GuestCpu::GuestPermission::Write));
    }
    bool Lifecycle(const Semaphore& sem) const {
        return foreground_admitted.load(std::memory_order_relaxed) &&
               (sem.Name() == "SuspendSemaphore" || sem.Name() == "ResumeSemaphore");
    }

public:
    GuestKernelSemaphore(GuestCpu::GuestAddressSpace& space) : space(space) {}
    // Android rendered sessions have already passed the app's foreground/
    // Surface admission before guest startup. Unity's named SuspendSemaphore
    // and ResumeSemaphore are created with count zero and are otherwise
    // released by the platform's suspend/resume notifications; carry the
    // initial foreground/resume edge into the guest domain.
    // Desktop remains unchanged because it never enables this flag.
    void AdmitForeground() {
        foreground_admitted.store(true, std::memory_order_relaxed);
    }
    u32 Waiting(u32 id) const {
        auto sem = objects.Find(id);
        return sem ? static_cast<u32>(sem->Waiting()) : 0;
    }
    u64 Dispatch(std::string_view nid, const std::array<u64, 6>& a, s32 priority,
                 std::stop_token stop = {}) {
        using namespace GuestCpu;
        try {
            if (nid == "Zxa0VhQVTsk" || nid == "12wOHk8ywb0") {
                const bool poll = nid == "12wOHk8ywb0";
                const s32 need = a[1];
                u32 timeout{};
                std::vector<GuestAddressSpace::MappingIdentity> identities;
                if (!poll && a[2]) {
                    auto pin = space.AcquireDataSpan({GuestAddress{a[2]}, 4}, true, stop);
                    Need(bool(pin), ORBIS_KERNEL_ERROR_EFAULT);
                    std::memcpy(&timeout, pin.Value().Bytes().data(), 4);
                    for (u64 at = a[2], end = at + 4; at < end;) {
                        auto mapping = space.Query(GuestAddress{at});
                        Need(bool(mapping), ORBIS_KERNEL_ERROR_EFAULT);
                        u64 next = std::min(end, mapping.Value().range.End());
                        identities.push_back({at, next, mapping.Value().mapping_generation});
                        at = next;
                    }
                }
                const u32 id = u32(a[0]);
                auto sem = Find(id);
                Need(need > 0 && need <= sem->Snapshot().maximum, ORBIS_KERNEL_ERROR_EINVAL);
                if (Lifecycle(*sem)) {
                    // Until the Android lifecycle bridge gains a matching
                    // background/revoke callback, an admitted foreground is
                    // a level, not a one-shot edge. Do not affect any other
                    // guest semaphore or desktop behavior.
                    sem->RaiseTo(need);
                }
                if (poll) {
                    if (stop.stop_requested())
                        return u32(ORBIS_KERNEL_ERROR_EINTR);
                    const s32 result = sem->Poll(need);
                    Trace(nid, id, *sem, need, result);
                    return u32(result);
                }
                bool parked = false;
                SyncMetrics::Phase park_phase{SyncMetrics::Stage::Park};
                const s32 result = sem->Wait(need, u32(priority), a[2] ? &timeout : nullptr,
                                             stop, &parked);
                park_phase.End();
                Trace(nid, id, *sem, need, result);
                if (a[2] && parked) {
                    // The time left goes back only to the mapping that supplied the budget.
                    const GuestAddressSpace::DataRequest request{
                        {GuestAddress{a[2]}, 4}, GuestPermission::Write, identities};
                    auto pinned = space.AcquireDataBatch({&request, 1});
                    Need(bool(pinned), ORBIS_KERNEL_ERROR_EFAULT);
                    std::memcpy(pinned.Value()[0].WritableBytes().data(), &timeout, 4);
                }
                return u32(result);
            }
            if (nid == "188x57JYp0g") {
                Need(a[1] && u32(a[2]) <= 2 && s32(a[3]) >= 0 && s32(a[4]) > 0 &&
                         s32(a[3]) <= s32(a[4]) && !a[5],
                     ORBIS_KERNEL_ERROR_EINVAL);
                Need(Writable(a[0], 4), ORBIS_KERNEL_ERROR_EFAULT);
                std::string name;
                for (u64 i = 0;; ++i) {
                    Need(a[1] <= UINT64_MAX - i, ORBIS_KERNEL_ERROR_EFAULT);
                    char c = Read<char>(a[1] + i);
                    if (!c)
                        break;
                    Need(i < 31, ORBIS_KERNEL_ERROR_ENAMETOOLONG);
                    name += c;
                }
                s32 initial = s32(a[3]);
                const bool lifecycle = foreground_admitted.load(std::memory_order_relaxed) &&
                                       (name == "SuspendSemaphore" || name == "ResumeSemaphore");
                if (lifecycle && initial == 0)
                    initial = 1;
                auto sem = std::make_shared<Semaphore>(std::move(name), initial, s32(a[4]),
                                                       u32(a[2]) == 1);
                const auto id = objects.Insert(sem);
                Need(id.has_value(), ORBIS_KERNEL_ERROR_ENOMEM);
                Trace(nid, *id, *sem, 0, 0);
                try {
                    Put(a[0], *id);
                } catch (...) {
                    objects.Erase(*id);
                    throw;
                }
                return 0;
            }
            if (nid == "4czppHBiriw") {
                auto sem = Find(u32(a[0]));
                const s32 result = sem->Signal(s32(a[1]));
                Trace(nid, u32(a[0]), *sem, s32(a[1]), result);
                return u32(result);
            }
            if (nid == "4DM06U2BNEY") {
                auto sem = Find(u32(a[0]));
                const s32 count = a[1];
                Need(count <= sem->Snapshot().maximum, ORBIS_KERNEL_ERROR_EINVAL);
                if (a[2]) {
                    // Validated before any waiter is released, as the waiter count is an output.
                    Need(Writable(a[2], 4), ORBIS_KERNEL_ERROR_EFAULT);
                }
                s32 waiters{};
                const s32 result = sem->Cancel(count, &waiters);
                if (result == ORBIS_OK && a[2])
                    Put(a[2], waiters);
                return u32(result);
            }
            if (nid == "R1Jvn8bSCW8") {
                auto sem = objects.Erase(u32(a[0]));
                Need(sem != nullptr, ORBIS_KERNEL_ERROR_ESRCH);
                sem->Delete();
                return 0;
            }
            return u32(ORBIS_KERNEL_ERROR_EINVAL);
        } catch (Failure failure) {
            return u32(failure.error);
        } catch (const std::bad_alloc&) {
            return u32(ORBIS_KERNEL_ERROR_ENOMEM);
        }
    }
};
} // namespace Core::HostRuntime
