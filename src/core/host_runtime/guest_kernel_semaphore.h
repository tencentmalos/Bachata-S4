// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once
#include <algorithm>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstring>
#include <list>
#include <map>
#include <memory>
#include <mutex>
#include <stop_token>
#include <string>
#if defined(__ANDROID__)
#include <android/log.h>
#endif
#include "common/types.h"
#include "core/guest_cpu/api/address_space.h"
#include "core/libraries/kernel/orbis_error.h"
namespace Core::HostRuntime {
// Orbis 32-bit SlotId ABI is distinct from the POSIX sem_t guest-pointer ABI.
// Mirrors desktop OrbisSem token/priority/cancel/delete behavior without g_curthread.
class GuestKernelSemaphore {
    using Clock = std::chrono::steady_clock;
    struct Failure {
        int error;
    };
    struct Waiter {
        s32 need, priority;
        bool done{};
        s32 result{};
    };
    struct Semaphore {
        s32 value{}, initial{}, maximum{};
        bool fifo{};
        std::string name;
        std::list<std::shared_ptr<Waiter>> waiters;
    };
    GuestCpu::GuestAddressSpace& space;

    mutable std::mutex mutex;
    std::condition_variable_any changed;
    std::map<u32, std::shared_ptr<Semaphore>> objects;
    static inline std::atomic<u32> next_id{1};
    bool foreground_admitted{};
    static void Trace(std::string_view nid, u32 id, const Semaphore& sem, s32 requested,
                      s32 result) {
#if defined(__ANDROID__)
        static std::atomic<u32> count{};
        if (nid == "188x57JYp0g" && sem.name != "SuspendSemaphore" &&
            sem.name != "ResumeSemaphore")
            return;
        const u32 index = count.fetch_add(1, std::memory_order_relaxed);
        if (index < 256) {
            __android_log_print(ANDROID_LOG_INFO, "GuestKernelSema",
                                "trace=%u nid=%.*s id=%u name=%s value=%d initial=%d max=%d requested=%d result=%d waiters=%zu",
                                index, static_cast<int>(nid.size()), nid.data(), id,
                                sem.name.c_str(), sem.value, sem.initial, sem.maximum, requested,
                                result, sem.waiters.size());
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
        auto it = objects.find(id);
        Need(it != objects.end(), ORBIS_KERNEL_ERROR_ESRCH);
        return it->second;
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
    static void Wake(Semaphore& sem) {
        for (auto it = sem.waiters.begin(); it != sem.waiters.end();) {
            auto& waiter = *it;
            if (waiter->need > sem.value) {
                ++it;
                continue;
            } // same satisfiable-waiter scan as desktop
            sem.value -= waiter->need;
            waiter->done = true;
            waiter->result = 0;
            it = sem.waiters.erase(it);
        }
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
        std::lock_guard lock(mutex);
        foreground_admitted = true;
    }
    u32 Waiting(u32 id) const {
        std::lock_guard lock(mutex);
        auto it = objects.find(id);
        return it == objects.end() ? 0 : it->second->waiters.size();
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
                std::unique_lock lock(mutex);
                const u32 id = u32(a[0]);
                auto sem = Find(id);
                Need(need > 0 && need <= sem->maximum, ORBIS_KERNEL_ERROR_EINVAL);
                if (foreground_admitted &&
                    (sem->name == "SuspendSemaphore" || sem->name == "ResumeSemaphore") &&
                    sem->value < need) {
                    // Until the Android lifecycle bridge gains a matching
                    // background/revoke callback, an admitted foreground is
                    // a level, not a one-shot edge. Do not affect any other
                    // guest semaphore or desktop behavior.
                    sem->value = need;
                }
                Trace(nid, id, *sem, need, sem->value);
                if (stop.stop_requested())
                    return u32(ORBIS_KERNEL_ERROR_EINTR);
                if (sem->value >= need) {
                    sem->value -= need;
                    Trace(nid, id, *sem, need, 0);
                    return 0;
                }
                if (poll)
                    return u32(ORBIS_KERNEL_ERROR_EBUSY);
                if (a[2] && !timeout)
                    return u32(ORBIS_KERNEL_ERROR_ETIMEDOUT);
                auto waiter = std::make_shared<Waiter>(Waiter{need, priority});
                auto at = sem->waiters.end();
                if (!sem->fifo)
                    at = std::ranges::find_if(
                        sem->waiters, [&](const auto& item) { return item->priority > priority; });
                sem->waiters.insert(at, waiter);
                const auto deadline = Clock::now() + std::chrono::microseconds(timeout);
                if (a[2])
                    changed.wait_until(lock, stop, deadline, [&] { return waiter->done; });
                else
                    changed.wait(lock, stop, [&] { return waiter->done; });
                sem->waiters.remove(waiter);
                s32 result = waiter->done            ? waiter->result
                             : stop.stop_requested() ? ORBIS_KERNEL_ERROR_EINTR
                                                     : ORBIS_KERNEL_ERROR_ETIMEDOUT;
                Trace(nid, id, *sem, need, result);
                lock.unlock();
                if (a[2]) {
                    const s64 remaining = std::chrono::duration_cast<std::chrono::microseconds>(
                                              deadline - Clock::now())
                                              .count();
                    const u32 left = !result ? std::clamp<s64>(remaining, 0, timeout) : 0;

                    const GuestAddressSpace::DataRequest request{
                        {GuestAddress{a[2]}, 4}, GuestPermission::Write, identities};
                    auto pinned = space.AcquireDataBatch({&request, 1});
                    Need(bool(pinned), ORBIS_KERNEL_ERROR_EFAULT);
                    std::memcpy(pinned.Value()[0].WritableBytes().data(), &left, 4);
                }
                return u32(result);
            }
            // Only this semaphore domain is synchronized below; no wait/callback.

            std::lock_guard lock(mutex);
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
                Need(objects.size() < 4096, ORBIS_KERNEL_ERROR_ENOMEM);
                auto sem = std::make_shared<Semaphore>();
                sem->value = sem->initial = s32(a[3]);
                sem->maximum = s32(a[4]);
                sem->fifo = u32(a[2]) == 1;
                sem->name = std::move(name);
                if (foreground_admitted &&
                    (sem->name == "SuspendSemaphore" || sem->name == "ResumeSemaphore") &&
                    sem->value == 0)
                    sem->value = sem->initial = 1;
                const u32 id = next_id.fetch_add(1);
                Need(id && id < INT32_MAX, ORBIS_KERNEL_ERROR_ENOMEM);
                objects.emplace(id, sem);
                Trace(nid, id, *sem, 0, 0);
                try {
                    Put(a[0], id);
                } catch (...) {
                    objects.erase(id);
                    throw;
                }
                return 0;
            }
            auto sem = Find(u32(a[0]));
            if (nid == "4czppHBiriw") {
                const s32 count = a[1];
                Need(count > 0 && count <= sem->maximum - sem->value, ORBIS_KERNEL_ERROR_EINVAL);
                sem->value += count;
                Wake(*sem);
                changed.notify_all();
                Trace(nid, u32(a[0]), *sem, count, 0);
                return 0;
            }
            if (nid == "4DM06U2BNEY") {
                const s32 count = a[1];
                Need(count <= sem->maximum, ORBIS_KERNEL_ERROR_EINVAL);
                if (a[2])
                    Put(a[2], s32(sem->waiters.size()));
                for (auto& waiter : sem->waiters) {
                    waiter->done = true;
                    waiter->result = ORBIS_KERNEL_ERROR_ECANCELED;
                }
                sem->waiters.clear();
                sem->value = count < 0 ? sem->initial : count;
                changed.notify_all();
                return 0;
            }
            if (nid == "R1Jvn8bSCW8") {
                for (auto& waiter : sem->waiters) {
                    waiter->done = true;
                    waiter->result = ORBIS_KERNEL_ERROR_EACCES;
                }
                sem->waiters.clear();
                objects.erase(u32(a[0]));
                changed.notify_all();
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
