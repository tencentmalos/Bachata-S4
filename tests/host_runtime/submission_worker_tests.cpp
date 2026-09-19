// SPDX-License-Identifier: GPL-2.0-or-later
#include "video_core/renderer_vulkan/submission_worker.h"
#include <chrono>
#include <cstdio>
#include <future>
#include <semaphore>
#include <vector>

using namespace std::chrono_literals;
using Vulkan::SubmissionWorker;
static unsigned checks{}, failures{};
#define CHECK(x) do { ++checks; if (!(x)) { ++failures; std::printf("FAIL %d: %s\n", __LINE__, #x); } } while (0)

int main() {
    // A present-like driver call can hold the Vulkan queue lock. Producers must
    // still transfer their OWNED batch without falsely reporting submission.
    {
        SubmissionWorker worker(2);
        std::binary_semaphore entered{0}, release{0};
        std::atomic<unsigned> calls{}, gpu_completed{};
        std::vector<unsigned> observed;
        unsigned stack_value = 17;
        auto first = worker.Enqueue([&, value = stack_value](auto& receipt) {
            entered.release(); release.acquire();
            observed.push_back(value); ++calls;
            receipt.started_ns = 123;
        });
        CHECK(entered.try_acquire_for(2s));
        stack_value = 99;
        auto second = worker.Enqueue([&, value = stack_value](auto&) {
            observed.push_back(value); ++calls;
        });
        stack_value = 999;
        CHECK(!first->submitted.load() && !second->submitted.load());
        auto third = std::async(std::launch::async, [&] {
            return worker.Enqueue([&](auto&) { observed.push_back(101); ++calls; });
        });
        CHECK(third.wait_for(30ms) == std::future_status::timeout); // bounded, includes active
        auto waiter = std::async(std::launch::async, [&] { worker.Wait(second->serial); });
        CHECK(waiter.wait_for(30ms) == std::future_status::timeout);
        release.release();
        CHECK(third.wait_for(2s) == std::future_status::ready);
        auto last = third.get(); worker.Wait(last->serial); waiter.get();
        CHECK(first->submitted.load() && second->submitted.load() && last->submitted.load());
        CHECK(first->started_ns == 123);
        CHECK((observed == std::vector<unsigned>{17, 99, 101}));
        CHECK(calls.load() == 3 && gpu_completed.load() == 0); // never conflate with GPU completion
    }
    // Close wakes capacity waiters, but preserves all already accepted work.
    {
        SubmissionWorker worker(1);
        std::binary_semaphore entered{0}, release{0};
        auto receipt = worker.Enqueue([&](auto&) { entered.release(); release.acquire(); });
        CHECK(entered.try_acquire_for(2s));
        auto blocked = std::async(std::launch::async, [&] {
            try { worker.Enqueue([](auto&) {}); return false; }
            catch (const std::runtime_error&) { return true; }
        });
        CHECK(blocked.wait_for(30ms) == std::future_status::timeout);
        worker.Close();
        CHECK(blocked.wait_for(2s) == std::future_status::ready && blocked.get());
        CHECK(!receipt->submitted.load());
        release.release(); worker.Drain();
        CHECK(receipt->submitted.load());
    }
    // Device loss cancels queued jobs and wakes waits/admission. Captures must
    // be destroyed before a scheduler is allowed to tear down its Vulkan objects.
    {
        SubmissionWorker worker(2);
        std::binary_semaphore entered{0}, release{0};
        auto first = worker.Enqueue([&](auto&) {
            entered.release(); release.acquire(); throw std::runtime_error("device lost");
        });
        CHECK(entered.try_acquire_for(2s));
        std::atomic<unsigned> unexpected{};
        auto lease = std::make_shared<int>(1);
        std::weak_ptr<int> weak = lease;
        auto cancelled = worker.Enqueue([&, lease](auto&) { ++unexpected; });
        lease.reset();
        auto waiting = std::async(std::launch::async, [&] {
            try { worker.Wait(cancelled->serial); return false; }
            catch (const std::runtime_error&) { return true; }
        });
        auto blocked = std::async(std::launch::async, [&] {
            try { worker.Enqueue([](auto&) {}); return false; }
            catch (const std::runtime_error&) { return true; }
        });
        CHECK(waiting.wait_for(30ms) == std::future_status::timeout);
        CHECK(blocked.wait_for(30ms) == std::future_status::timeout);
        release.release();
        CHECK(waiting.wait_for(2s) == std::future_status::ready && waiting.get());
        CHECK(blocked.wait_for(2s) == std::future_status::ready && blocked.get());
        CHECK(!first->submitted.load() && !cancelled->submitted.load() && unexpected.load() == 0);
        CHECK(weak.expired());
        try { worker.CheckHealth(); CHECK(false); }
        catch (const std::runtime_error& e) { CHECK(std::string_view(e.what()) == "device lost"); }
    }
    // Multiple scheduler owners must execute in one device FIFO, with no lost
    // batches, concurrent backend calls, or accepted-vs-completed confusion.
    {
        SubmissionWorker worker(8);
        std::vector<uint64_t> serials;
        std::atomic<int> active{}, overlap{};
        auto produce = [&] {
            for (unsigned i = 0; i < 300; ++i) worker.Enqueue([&](auto& receipt) {
                if (active.fetch_add(1)) ++overlap;
                serials.push_back(receipt.serial);
                active.fetch_sub(1);
            });
        };
        std::thread a(produce), b(produce), c(produce);
        a.join(); b.join(); c.join(); worker.Drain();
        CHECK(serials.size() == 900 && overlap.load() == 0);
        bool ordered = true;
        for (size_t i = 0; i < serials.size(); ++i) ordered &= serials[i] == i + 1;
        CHECK(ordered);
        try { worker.Wait(901); CHECK(false); }
        catch (const std::invalid_argument&) { CHECK(true); }
    }
    try { SubmissionWorker invalid(0); CHECK(false); }
    catch (const std::invalid_argument&) { CHECK(true); }
    std::printf("SUBMISSION_WORKER checks=%u failures=%u\n", checks, failures);
    return failures ? 1 : 0;
}
