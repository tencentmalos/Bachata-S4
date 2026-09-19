// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once

#include <atomic>
#include <condition_variable>
#include <cstdint>
#include <deque>
#include <exception>
#include <functional>
#include <memory>
#include <mutex>
#include <stdexcept>
#include <thread>

namespace Vulkan {

// A receipt means host submission, never GPU completion. Only the worker writes
// started_ns; the consumer reads it after the release/acquire submitted flag.
struct SubmissionReceipt {
    uint64_t serial{};
    uint64_t started_ns{};
    std::atomic<bool> submitted{};
};

// One FIFO per VkDevice/queue. The capacity includes the executing job. Jobs own
// their parameter storage; GPU object lifetime stays with timeline retirement.
// Close wakes blocked producers and drains accepted jobs. A driver failure
// poisons admission, discards unexecuted jobs and wakes all host waiters.
class SubmissionWorker {
public:
    using Job = std::function<void(SubmissionReceipt&)>;
    explicit SubmissionWorker(size_t capacity, std::function<void()> initialize = {})
        : capacity(capacity) {
        if (!capacity) throw std::invalid_argument("zero submission capacity");
        worker = std::thread([this, initialize = std::move(initialize)] {
            try {
                if (initialize) initialize();
                Run();
            } catch (...) {
                std::scoped_lock lock(mutex);
                jobs.clear();
                error = std::current_exception();
                failed.store(true, std::memory_order_release);
                cv.notify_all();
            }
        });
    }
    ~SubmissionWorker() {
        Close();
        worker.join(); // No referenced Vulkan object may die before this returns.
    }
    SubmissionWorker(const SubmissionWorker&) = delete;
    SubmissionWorker& operator=(const SubmissionWorker&) = delete;

    std::shared_ptr<SubmissionReceipt> Enqueue(Job job) {
        auto receipt = std::make_shared<SubmissionReceipt>();
        std::unique_lock lock(mutex);
        cv.wait(lock, [&] { return error || closed || accepted - finished < capacity; });
        ThrowIfFailed();
        if (closed) throw std::runtime_error("Vulkan submission worker closed");
        receipt->serial = accepted + 1;
        jobs.push_back({receipt, std::move(job)});
        accepted = receipt->serial;
        lock.unlock();
        cv.notify_all();
        return receipt;
    }

    void Wait(uint64_t serial) const {
        std::unique_lock lock(mutex);
        if (serial > accepted) throw std::invalid_argument("unaccepted submission serial");
        cv.wait(lock, [&] { return error || finished >= serial; });
        ThrowIfFailed();
    }
    void Drain() const {
        uint64_t serial;
        { std::scoped_lock lock(mutex); serial = accepted; }
        Wait(serial);
    }
    void CheckHealth() const {
        if (failed.load(std::memory_order_acquire)) {
            std::scoped_lock lock(mutex);
            ThrowIfFailed();
        }
    }
    void Close() {
        { std::scoped_lock lock(mutex); closed = true; }
        cv.notify_all();
    }

private:
    struct Work { std::shared_ptr<SubmissionReceipt> receipt; Job job; };
    void ThrowIfFailed() const { if (error) std::rethrow_exception(error); }
    void Run() {
        for (;;) {
            Work work;
            {
                std::unique_lock lock(mutex);
                cv.wait(lock, [&] { return closed || !jobs.empty(); });
                if (jobs.empty()) return;
                work = std::move(jobs.front());
                jobs.pop_front();
            }
            work.job(*work.receipt); // Never hold the mailbox mutex in a driver call.
            work.job = {}; // Release captured ownership before publishing completion.
            work.receipt->submitted.store(true, std::memory_order_release);
            {
                std::scoped_lock lock(mutex);
                finished = work.receipt->serial;
            }
            cv.notify_all();
        }
    }
    const size_t capacity;
    mutable std::mutex mutex;
    mutable std::condition_variable cv;
    std::deque<Work> jobs;
    uint64_t accepted{}, finished{};
    bool closed{};
    std::exception_ptr error;
    std::atomic<bool> failed{};
    std::thread worker;
};
} // namespace Vulkan
