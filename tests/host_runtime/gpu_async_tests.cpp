// SPDX-License-Identifier: GPL-2.0-or-later
#include <chrono>
#include <cstdio>
#include <future>
#include <semaphore>
#include <stdexcept>
#include "core/platform.h"
#include "core/libraries/videoout/redraw_pacer.h"
#include "video_core/renderer_vulkan/timeline_completion.h"

using namespace std::chrono_literals;
static unsigned checks{}, failures{};
#define CHECK(x) do { ++checks; if (!(x)) { ++failures; std::printf("FAIL %d: %s\n", __LINE__, #x); } } while (0)
template <class F> bool Eventually(F check) {
    const auto end = std::chrono::steady_clock::now() + 2s;
    while (!check()) {
        if (std::chrono::steady_clock::now() >= end) return false;
        std::this_thread::yield();
    }
    return true;
}

int main() {
    {
        using Libraries::VideoOut::RedrawPacer;
        RedrawPacer redraw(16ms);
        auto now = RedrawPacer::Clock::time_point{1s};
        CHECK(redraw.TryAcquire(now, 0ms));
        // A 128 ms driver stall creates seven overdue vblanks. Catch-up must
        // advance those clocks without issuing seven more old-frame presents.
        now += 128ms;
        unsigned redraws{};
        for (int debt = 112; debt >= 0; debt -= 16)
            redraws += redraw.TryAcquire(now, -std::chrono::milliseconds(debt));
        CHECK(redraws == 1);
        CHECK(!redraw.TryAcquire(now + 1ms, 0ms));
        CHECK(redraw.TryAcquire(now + 16ms, -1ms));
        CHECK(!redraw.TryAcquire(now + 1s, -64ms));
        CHECK(redraw.TryAcquire(now + 1s, 0ms));
    }
    // Registering frame N+1 must not wait for frame N's slow GPU callback.
    Platform::IrqController irq;
    std::promise<void> entered;
    auto entered_future = entered.get_future();
    std::binary_semaphore release{0};
    std::atomic<unsigned> delivered{};
    irq.RegisterOnce(Platform::InterruptId::GfxFlip, [&](auto) {
        entered.set_value();
        release.acquire();
        delivered.fetch_add(1);
    });
    std::thread signal([&] { irq.Signal(Platform::InterruptId::GfxFlip); });
    CHECK(entered_future.wait_for(2s) == std::future_status::ready);
    auto registration = std::async(std::launch::async, [&] {
        irq.RegisterOnce(Platform::InterruptId::GfxFlip, [&](auto) { delivered.fetch_add(1); });
    });
    CHECK(registration.wait_for(200ms) == std::future_status::ready);
    CHECK(delivered.load() == 0);
    release.release();
    signal.join();
    registration.get();
    CHECK(delivered.load() == 1);
    irq.Signal(Platform::InterruptId::GfxFlip);
    irq.Signal(Platform::InterruptId::GfxFlip);
    CHECK(delivered.load() == 2); // one-shot exactly once

    // Concurrent signalers preserve callback FIFO and never execute together.
    std::promise<void> first;
    auto first_future = first.get_future();
    std::binary_semaphore finish_first{0};
    std::atomic<unsigned> order{};
    irq.RegisterOnce(Platform::InterruptId::GfxFlip, [&](auto) {
        first.set_value(); finish_first.acquire(); order.store(1);
    });
    irq.RegisterOnce(Platform::InterruptId::GfxFlip, [&](auto) {
        order.store(order.load() == 1 ? 2 : 99);
    });
    std::thread a([&] { irq.Signal(Platform::InterruptId::GfxFlip); });
    CHECK(first_future.wait_for(2s) == std::future_status::ready);
    std::thread b([&] { irq.Signal(Platform::InterruptId::GfxFlip); });
    finish_first.release(); a.join(); b.join();
    CHECK(order.load() == 2);

    // Persistent unregister still waits for its active callback to retire.
    std::promise<void> persistent;
    auto persistent_future = persistent.get_future();
    std::binary_semaphore finish_persistent{0};
    unsigned persistent_calls{};
    irq.Register(Platform::InterruptId::GfxEop, [&](auto) {
        ++persistent_calls; persistent.set_value(); finish_persistent.acquire();
    }, &irq);
    std::thread p([&] { irq.Signal(Platform::InterruptId::GfxEop); });
    CHECK(persistent_future.wait_for(2s) == std::future_status::ready);
    auto unregister = std::async(std::launch::async, [&] { irq.Unregister(Platform::InterruptId::GfxEop, &irq); });
    CHECK(unregister.wait_for(30ms) == std::future_status::timeout);
    finish_persistent.release(); p.join(); unregister.get();
    irq.Signal(Platform::InterruptId::GfxEop);
    CHECK(persistent_calls == 1);

    // A blocked driver must not block Submitted/CheckHealth, nor retire memory
    // until the requested GPU watermark actually completes. New work coalesces.
    std::atomic<uint64_t> completed{};
    std::mutex gpu_mutex;
    std::condition_variable_any gpu_cv;
    uint64_t actual_gpu{};
    std::atomic<uint64_t> waiting_for{};
    {
        Vulkan::TimelineCompletion tracker(completed, [&](uint64_t tick, std::stop_token stop) {
            std::unique_lock lock(gpu_mutex);
            waiting_for.store(tick);
            return gpu_cv.wait(lock, stop, [&] { return actual_gpu >= tick; });
        });
        CHECK(completed.load() == 0 && waiting_for.load() == 0);
        tracker.Submitted(1);
        CHECK(Eventually([&] { return waiting_for.load() == 1; }));
        auto producer = std::async(std::launch::async, [&] {
            for (uint64_t tick = 2; tick <= 1000; ++tick) tracker.Submitted(tick);
            tracker.CheckHealth();
        });
        CHECK(producer.wait_for(200ms) == std::future_status::ready);
        producer.get();
        CHECK(completed.load() == 0);
        { std::scoped_lock lock(gpu_mutex); actual_gpu = 1; }
        gpu_cv.notify_all();
        CHECK(Eventually([&] { return waiting_for.load() == 1000; }));
        CHECK(completed.load() == 1);
        { std::scoped_lock lock(gpu_mutex); actual_gpu = 1000; }
        gpu_cv.notify_all();
        CHECK(Eventually([&] { return completed.load() == 1000; }));
        Vulkan::TimelineCompletion::Complete(completed, 999);
        CHECK(completed.load() == 1000);
        tracker.Submitted(1001);
        CHECK(Eventually([&] { return waiting_for.load() == 1001; }));
    } // Must cancel/join a blocked backend before its referenced GPU state dies.
    CHECK(completed.load() == 1000);

    std::atomic<uint64_t> failed_completed{};
    Vulkan::TimelineCompletion failed(failed_completed, [](uint64_t, std::stop_token) -> bool {
        throw std::runtime_error("injected device lost");
    });
    failed.Submitted(1);
    CHECK(Eventually([&] {
        try { failed.CheckHealth(); return false; }
        catch (const std::runtime_error& e) { return std::string_view(e.what()) == "injected device lost"; }
    }));
    CHECK(failed_completed.load() == 0);
    // GPU completion before vkQueueSubmit returns is NOT a resource lease
    // release. Reproduce the explicit-wait/producer-reuse interleaving without
    // depending on a particular driver's timing.
    {
        std::atomic<uint64_t> gpu{};
        Vulkan::TimelineCompletion tracker(gpu, [](uint64_t, std::stop_token) { return true; });
        tracker.Submitted(1);
        CHECK(Eventually([&] { return tracker.ReusableTick() == 1; }));
        Vulkan::TimelineCompletion::Complete(gpu, 2); // GPU done, host call still held
        CHECK(tracker.ReusableTick() == 1);
        auto waiter = std::async(std::launch::async, [&] {
            return tracker.WaitSubmitted(2, {}, [] {});
        });
        CHECK(waiter.wait_for(30ms) == std::future_status::timeout);
        tracker.Submitted(2); // exact submit returned, no queue/device idle needed
        CHECK(waiter.wait_for(2s) == std::future_status::ready && waiter.get());
        CHECK(tracker.ReusableTick() == 2);
        tracker.Submitted(1); // delayed receipts cannot regress the watermark
        CHECK(tracker.ReusableTick() == 2);
        std::stop_source cancel;
        auto cancelled = std::async(std::launch::async, [&] {
            return tracker.WaitSubmitted(3, cancel.get_token(), [] {});
        });
        CHECK(cancelled.wait_for(30ms) == std::future_status::timeout);
        cancel.request_stop();
        CHECK(cancelled.wait_for(2s) == std::future_status::ready && !cancelled.get());
        try {
            tracker.WaitSubmitted(3, {}, [] { throw std::runtime_error("submit failed"); });
            CHECK(false);
        } catch (const std::runtime_error&) { CHECK(true); }
        CHECK(tracker.ReusableTick() == 2);
    }
    std::printf("GPU_ASYNC checks=%u failures=%u\n", checks, failures);
    return failures ? 1 : 0;
}
