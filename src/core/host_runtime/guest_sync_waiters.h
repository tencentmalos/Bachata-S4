// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once
#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstring>
#include <memory>
#include <mutex>
#include <optional>
#include <stop_token>
#include <vector>
#include "common/types.h"
#include "core/guest_cpu/api/address_space.h"
#include "core/libraries/kernel/posix_error.h"

namespace Core::HostRuntime {
// Wait/wake on a guest address with an expected value: the host half of the
// app-shipped guest synchronization fast path (guest/runtime/sync). Same shape
// as FreeBSD umtx / Linux futex / Wine RtlWaitOnAddress: the caller publishes
// its "contended" state in guest memory first, then asks to sleep while the
// word still holds that value; a release stores the new value and then wakes.
//
// Lost-wake freedom: Wait reads the word under the address bucket lock and
// enqueues under the same lock; Wake dequeues under that lock after the guest
// (or host) store. Either the store is visible to the read, or the waiter is
// already queued when Wake scans.
//
// No pin or address-space lock survives the park. Waiters are keyed by guest
// VA; the objects behind them (GuestSyncArena blocks) are retained until
// Session teardown, and session cancellation wakes every parked thread through
// its stop token. Retiring a mapping under parked waiters is not supported.
class GuestAddressWaiters final {
public:
    explicit GuestAddressWaiters(GuestCpu::GuestAddressSpace& space) : space(space) {}
    GuestAddressWaiters(const GuestAddressWaiters&) = delete;
    GuestAddressWaiters& operator=(const GuestAddressWaiters&) = delete;

    using Deadline = std::optional<std::chrono::steady_clock::time_point>;
    // width is 4 or 8. Returns 0 when the word already differed or the thread
    // was woken, POSIX_EINTR on cancellation, POSIX_ETIMEDOUT past the deadline,
    // POSIX_EFAULT/POSIX_EINVAL for an unreadable or malformed request.
    int Wait(u64 address, u64 expected, unsigned width, std::stop_token cancel,
             Deadline deadline = {}) {
        if ((width != 4 && width != 8) || address % width)
            return POSIX_EINVAL;
        auto waiter = std::make_shared<Waiter>();
        waiter->address = address;
        auto& bucket = Bucket(address);
        {
            std::lock_guard lock(bucket.guard);
            u64 value{};
            if (!ReadValue(address, width, value))
                return POSIX_EFAULT;
            if (value != expected)
                return 0;
            bucket.waiters.push_back(waiter);
        }
        // A pre-requested token invokes the callback synchronously; it takes
        // only the waiter's own lock, never the bucket lock.
        std::stop_callback on_cancel(cancel, [&] {
            std::lock_guard lock(waiter->park);
            waiter->parked.notify_all();
        });
        int result = 0;
        {
            std::unique_lock lock(waiter->park);
            const auto ready = [&] { return waiter->woken || cancel.stop_requested(); };
            if (deadline) {
                if (!waiter->parked.wait_until(lock, *deadline, ready))
                    result = POSIX_ETIMEDOUT;
            } else {
                waiter->parked.wait(lock, ready);
            }
            if (!result && !waiter->woken)
                result = POSIX_EINTR;
            if (waiter->woken)
                return 0;
        }
        // Not woken yet: leave the queue. If Wake dequeued us meanwhile the
        // wake is ours and must not be dropped.
        {
            std::lock_guard lock(bucket.guard);
            auto& queue = bucket.waiters;
            const auto it = std::find(queue.begin(), queue.end(), waiter);
            if (it == queue.end())
                return 0;
            queue.erase(it);
        }
        return result;
    }
    // Wakes up to `count` waiters parked on `address`, oldest first.
    size_t Wake(u64 address, size_t count) {
        std::vector<std::shared_ptr<Waiter>> selected;
        auto& bucket = Bucket(address);
        {
            std::lock_guard lock(bucket.guard);
            auto& queue = bucket.waiters;
            for (auto it = queue.begin(); it != queue.end() && selected.size() < count;) {
                if ((*it)->address == address) {
                    selected.push_back(std::move(*it));
                    it = queue.erase(it);
                } else {
                    ++it;
                }
            }
        }
        // Notify after releasing the bucket lock so the woken thread does not
        // immediately block on it again.
        for (const auto& waiter : selected) {
            {
                std::lock_guard lock(waiter->park);
                waiter->woken = true;
            }
            waiter->parked.notify_all();
        }
        return selected.size();
    }
    size_t Pending(u64 address) const {
        auto& bucket = Bucket(address);
        std::lock_guard lock(bucket.guard);
        return std::count_if(bucket.waiters.begin(), bucket.waiters.end(),
                             [&](const auto& w) { return w->address == address; });
    }

private:
    struct Waiter {
        u64 address{};
        std::mutex park;
        std::condition_variable_any parked;
        bool woken{};
    };
    struct AddressBucket {
        mutable std::mutex guard;
        std::vector<std::shared_ptr<Waiter>> waiters;
    };
    static constexpr size_t BucketCount = 256;
    GuestCpu::GuestAddressSpace& space;
    mutable std::array<AddressBucket, BucketCount> buckets{};

    AddressBucket& Bucket(u64 address) const {
        auto key = address >> 2;
        key ^= key >> 17;
        key *= 0x9e3779b97f4a7c15ULL;
        return buckets[(key >> 40) & (BucketCount - 1)];
    }
    bool ReadValue(u64 address, unsigned width, u64& value) const {
        // Checked short pin: honours data admission/retirement like ReadData.
        // The load itself is atomic so a concurrent guest store is never torn.
        auto pin = space.AcquireDataSpan({GuestCpu::GuestAddress{address}, width}, false);
        if (!pin)
            return false;
        // Read-only lease; atomic_ref<const T> is C++23, so drop const for the load only.
        auto* host = const_cast<std::byte*>(pin.Value().Bytes().data());
        if (width == 4)
            value = std::atomic_ref(*reinterpret_cast<u32*>(host)).load(std::memory_order_acquire);
        else
            value = std::atomic_ref(*reinterpret_cast<u64*>(host)).load(std::memory_order_acquire);
        return true;
    }
};
} // namespace Core::HostRuntime
