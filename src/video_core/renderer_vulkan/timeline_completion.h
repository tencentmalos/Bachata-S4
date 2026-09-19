// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once

#include <atomic>
#include <algorithm>
#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <exception>
#include <functional>
#include <mutex>
#include <thread>

namespace Vulkan {
// One coalesced submission watermark, not a per-submit work queue. The backend
// waits in bounded slices and must honor stop; successful submission alone is
// never published as GPU completion. Driver calls run without the mailbox lock.
class TimelineCompletion {
public:
    using Wait = std::function<bool(uint64_t, std::stop_token)>;
    TimelineCompletion(std::atomic<uint64_t>& completed, Wait wait)
        : completed(completed), wait(std::move(wait)), worker([this](std::stop_token stop) {
              try {
                  Run(stop);
              } catch (...) {
                  std::scoped_lock lock(mutex);
                  error = std::current_exception();
                  failed.store(true, std::memory_order_release);
              }
          }) {}

    ~TimelineCompletion() {
        worker.request_stop();
        cv.notify_all();
        worker.join();
    }

    void Submitted(uint64_t tick) {
        {
            std::scoped_lock lock(mutex);
            if (tick > submitted.load(std::memory_order_relaxed))
                submitted.store(tick, std::memory_order_release);
        }
        cv.notify_all();
    }

    // A GPU signal can become visible while vkQueueSubmit is still using its
    // host command-buffer state. Explicit Wait callers also update completed,
    // so the completion worker alone cannot enforce this ownership boundary.
    uint64_t ReusableTick() const {
        return std::min(completed.load(std::memory_order_acquire),
                        submitted.load(std::memory_order_acquire));
    }

    template <class Health>
    bool WaitSubmitted(uint64_t tick, std::stop_token stop, Health check_health) {
        while (submitted.load(std::memory_order_acquire) < tick) {
            if (stop.stop_requested()) return false;
            check_health(); // May acquire the submission worker lock; not under our mutex.
            CheckHealth();
            std::unique_lock lock(mutex);
            cv.wait_for(lock, stop, std::chrono::milliseconds(50), [&] {
                return submitted.load(std::memory_order_acquire) >= tick;
            });
        }
        return true;
    }

    void CheckHealth() const {
        if (failed.load(std::memory_order_acquire)) {
            std::scoped_lock lock(mutex);
            std::rethrow_exception(error);
        }
    }

    static void Complete(std::atomic<uint64_t>& completed, uint64_t tick) {
        auto previous = completed.load(std::memory_order_relaxed);
        while (previous < tick && !completed.compare_exchange_weak(
                   previous, tick, std::memory_order_release, std::memory_order_relaxed)) {}
    }

private:
    void Run(std::stop_token stop) {
        while (!stop.stop_requested()) {
            uint64_t target;
            {
                std::unique_lock lock(mutex);
                cv.wait(lock, stop, [this] {
                    return submitted.load(std::memory_order_acquire) >
                           completed.load(std::memory_order_acquire);
                });
                if (stop.stop_requested()) return;
                target = submitted.load(std::memory_order_acquire);
            }
            if (!wait(target, stop)) return;
            Complete(completed, target);
        }
    }

    std::atomic<uint64_t>& completed;
    Wait wait;
    mutable std::mutex mutex;
    std::condition_variable_any cv;
    std::atomic<uint64_t> submitted{};
    std::exception_ptr error;
    std::atomic<bool> failed{};
    std::jthread worker;
};
} // namespace Vulkan
