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
    std::recursive_mutex& vm;
    mutable std::mutex mutex;
    std::condition_variable_any changed;
    std::map<u32, std::shared_ptr<Semaphore>> objects;
    static inline std::atomic<u32> next_id{1};
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
        Need(address && bool(space.Read(GuestCpu::GuestAddress{address},
                                        std::as_writable_bytes(std::span{&value, 1}))),
             ORBIS_KERNEL_ERROR_EFAULT);
        return value;
    }
    template <class T>
    void Put(u64 address, const T& value) {
        auto pin = space.AcquirePinnedSpan({GuestCpu::GuestAddress{address}, sizeof(T)}, true);
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
    GuestKernelSemaphore(GuestCpu::GuestAddressSpace& space, std::recursive_mutex& vm)
        : space(space), vm(vm) {}
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
                struct Part {
                    u64 begin, end, generation;
                };
                std::vector<Part> identities;
                if (!poll && a[2]) {
                    std::lock_guard gate(vm);
                    Need(Writable(a[2], 4), ORBIS_KERNEL_ERROR_EFAULT);
                    timeout = Read<u32>(a[2]);
                    for (u64 at = a[2], end = at + 4; at < end;) {
                        auto mapping = space.Query(GuestAddress{at});
                        Need(bool(mapping), ORBIS_KERNEL_ERROR_EFAULT);
                        u64 next = std::min(end, mapping.Value().range.End());
                        identities.push_back({at, next, mapping.Value().mapping_generation});
                        at = next;
                    }
                }
                std::unique_lock lock(mutex);
                auto sem = Find(u32(a[0]));
                Need(need > 0 && need <= sem->maximum, ORBIS_KERNEL_ERROR_EINVAL);
                if (stop.stop_requested())
                    return u32(ORBIS_KERNEL_ERROR_EINTR);
                if (sem->value >= need) {
                    sem->value -= need;
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
                lock.unlock();
                if (a[2]) {
                    const s64 remaining = std::chrono::duration_cast<std::chrono::microseconds>(
                                              deadline - Clock::now())
                                              .count();
                    const u32 left = !result ? std::clamp<s64>(remaining, 0, timeout) : 0;
                    std::lock_guard gate(vm);
                    for (auto [begin, end, generation] : identities)
                        for (u64 at = begin; at < end;) {
                            auto mapping = space.Query(GuestAddress{at});
                            Need(mapping && mapping.Value().mapping_generation == generation,
                                 ORBIS_KERNEL_ERROR_EFAULT);
                            at = std::min(end, mapping.Value().range.End());
                        }
                    Put(a[2], left);
                }
                return u32(result);
            }
            // Only short operations below; VM -> domain order, no wait/callback.
            std::lock_guard gate(vm);
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
                const u32 id = next_id.fetch_add(1);
                Need(id && id < INT32_MAX, ORBIS_KERNEL_ERROR_ENOMEM);
                objects.emplace(id, sem);
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
