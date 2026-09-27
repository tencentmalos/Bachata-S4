// SPDX-License-Identifier: GPL-2.0-or-later
// Event queue waits shared by the desktop kernel and the Android HLE wait: wake-ups, timeouts,
// small (sub-1.2 ms) HR timers, stop requests, Close and several waiters.
#include <atomic>
#include <chrono>
#include <cstdio>
#include <thread>
#include "core/libraries/kernel/equeue.h"

using namespace Libraries::Kernel;
using Clock = std::chrono::steady_clock;
using namespace std::chrono_literals;

static unsigned checks{}, failures{};
#define CHECK(...)                                                                                 \
    do {                                                                                           \
        ++checks;                                                                                  \
        if (!(__VA_ARGS__)) {                                                                      \
            ++failures;                                                                            \
            std::printf("FAIL %d: %s\n", __LINE__, #__VA_ARGS__);                                  \
        }                                                                                          \
    } while (0)

static void AddUser(EqueueInternal& q, u64 id, bool edge) {
    EqueueEvent event{};
    event.event.ident = id;
    event.event.filter = OrbisKernelEvent::Filter::User;
    event.event.flags = OrbisKernelEvent::Flags::Add | (edge ? OrbisKernelEvent::Flags::Clear : 0);
    CHECK(q.AddEvent(event));
}

static void AddSmall(EqueueInternal& q, u64 id, std::chrono::microseconds after) {
    OrbisKernelBintime time{0, 0};
    // bintime fraction of a second: frac >> 32 is the 32-bit binary fraction.
    time.frac = s64((u64(after.count()) * (u64(1) << 32) / 1000000) << 32);
    EqueueEvent event{};
    event.event.ident = id;
    event.event.filter = OrbisKernelEvent::Filter::HrTimer;
    event.event.flags = OrbisKernelEvent::Flags::Add | OrbisKernelEvent::Flags::OneShot;
    event.event.data = reinterpret_cast<u64>(&time);
    CHECK(q.AddSmallTimer(event));
}

static double Ms(Clock::duration d) {
    return std::chrono::duration<double, std::milli>(d).count();
}

int main() {
    OrbisKernelEvent ev[8]{};
    const OrbisKernelUseconds poll = 0;

    {
        // Poll, level and edge user events.
        EqueueInternal q(1, "poll");
        CHECK(q.WaitForEvents(ev, 8, &poll) == 0);
        AddUser(q, 7, false);
        AddUser(q, 8, true);
        CHECK(q.TriggerEvent(7, OrbisKernelEvent::Filter::User, reinterpret_cast<void*>(0x70)));
        CHECK(q.TriggerEvent(8, OrbisKernelEvent::Filter::User, reinterpret_cast<void*>(0x80)));
        CHECK(q.WaitForEvents(ev, 8, &poll) == 2);
        // The level event stays triggered, the edge (Clear) one does not.
        CHECK(q.WaitForEvents(ev, 8, &poll) == 1);
        CHECK(ev[0].ident == 7 && ev[0].udata == reinterpret_cast<void*>(0x70));
        // Capacity is honoured.
        CHECK(q.TriggerEvent(8, OrbisKernelEvent::Filter::User, nullptr));
        CHECK(q.WaitForEvents(ev, 1, &poll) == 1);
    }
    {
        // Timeout without events.
        EqueueInternal q(2, "timeout");
        const OrbisKernelUseconds timo = 20000;
        const auto start = Clock::now();
        CHECK(q.WaitForEvents(ev, 8, &timo) == 0);
        const double elapsed = Ms(Clock::now() - start);
        CHECK(elapsed >= 19.0 && elapsed < 500.0);
    }
    {
        // A trigger wakes an infinite wait.
        EqueueInternal q(3, "wake");
        AddUser(q, 1, true);
        std::atomic<int> got{-1};
        std::atomic<double> waited{};
        std::thread waiter([&] {
            const auto start = Clock::now();
            got = q.WaitForEvents(ev, 8, nullptr);
            waited = Ms(Clock::now() - start);
        });
        std::this_thread::sleep_for(30ms);
        CHECK(q.TriggerEvent(1, OrbisKernelEvent::Filter::User, nullptr));
        waiter.join();
        CHECK(got == 1);
        CHECK(waited >= 25.0 && waited < 500.0);
    }
    {
        // Small timers expire precisely, in an infinite wait.
        EqueueInternal q(4, "small");
        AddSmall(q, 11, 600us);
        const auto start = Clock::now();
        CHECK(q.WaitForEvents(ev, 8, nullptr) == 1);
        const double elapsed = Ms(Clock::now() - start);
        CHECK(ev[0].ident == 11 && ev[0].filter == OrbisKernelEvent::Filter::HrTimer);
        CHECK(elapsed >= 0.55 && elapsed < 50.0);
        CHECK(!q.HasSmallTimer());
    }
    {
        // A small timer added while a waiter sleeps on a long deadline wakes it at its expiry.
        EqueueInternal q(5, "late_timer");
        std::atomic<int> got{-1};
        std::atomic<double> waited{};
        std::thread waiter([&] {
            const OrbisKernelUseconds timo = 2000000;
            const auto start = Clock::now();
            got = q.WaitForEvents(ev, 8, &timo);
            waited = Ms(Clock::now() - start);
        });
        std::this_thread::sleep_for(20ms);
        AddSmall(q, 12, 300us);
        waiter.join();
        CHECK(got == 1);
        CHECK(waited >= 19.0 && waited < 500.0);
    }
    {
        // A triggered event is delivered while a small timer is still pending.
        EqueueInternal q(6, "mixed");
        AddUser(q, 2, true);
        AddSmall(q, 13, 1000us);
        CHECK(q.TriggerEvent(2, OrbisKernelEvent::Filter::User, nullptr));
        const auto start = Clock::now();
        CHECK(q.WaitForEvents(ev, 8, nullptr) == 1);
        CHECK(ev[0].ident == 2);
        CHECK(Ms(Clock::now() - start) < 0.9);
        CHECK(q.WaitForEvents(ev, 8, nullptr) == 1);
        CHECK(ev[0].ident == 13);
    }
    {
        // Stop requests and Close end WaitReady.
        EqueueInternal q(7, "stop");
        std::stop_source source;
        std::atomic<int> result{-1};
        std::thread waiter([&] { result = int(q.WaitReady(std::nullopt, source.get_token())); });
        std::this_thread::sleep_for(20ms);
        source.request_stop();
        waiter.join();
        CHECK(result == int(EqueueWaitResult::Interrupted));
        CHECK(q.WaitReady(Clock::now() + 5ms, {}) == EqueueWaitResult::TimedOut);
        std::thread closer([&] { result = int(q.WaitReady(std::nullopt, {})); });
        std::this_thread::sleep_for(20ms);
        q.Close();
        closer.join();
        CHECK(result == int(EqueueWaitResult::Closed));
        CHECK(q.TakeTriggered(ev, 8) == 0);
    }
    {
        // Two waiters, edge events: each trigger reaches exactly one waiter, none is lost.
        EqueueInternal q(8, "waiters");
        AddUser(q, 3, true);
        std::atomic<int> total{};
        const OrbisKernelUseconds timo = 300000;
        auto body = [&] {
            OrbisKernelEvent local[4]{};
            total += q.WaitForEvents(local, 4, &timo);
        };
        std::thread a(body), b(body);
        std::this_thread::sleep_for(20ms);
        CHECK(q.TriggerEvent(3, OrbisKernelEvent::Filter::User, nullptr));
        std::this_thread::sleep_for(20ms);
        CHECK(q.TriggerEvent(3, OrbisKernelEvent::Filter::User, nullptr));
        a.join();
        b.join();
        CHECK(total == 2);
    }
    {
        // Producer/consumer stress: every trigger after the consumer's last take is delivered.
        EqueueInternal q(9, "stress");
        AddUser(q, 4, true);
        std::atomic<bool> done{false};
        std::atomic<int> taken{};
        std::thread consumer([&] {
            OrbisKernelEvent local[4]{};
            const OrbisKernelUseconds timo = 200000;
            while (true) {
                const int n = q.WaitForEvents(local, 4, &timo);
                taken += n;
                if (n == 0 && done) {
                    break;
                }
            }
        });
        for (int i = 0; i < 20000; ++i) {
            q.TriggerEvent(4, OrbisKernelEvent::Filter::User, nullptr);
            if ((i & 63) == 0) {
                std::this_thread::yield();
            }
        }
        std::this_thread::sleep_for(20ms);
        const int before = taken;
        q.TriggerEvent(4, OrbisKernelEvent::Filter::User, nullptr);
        std::this_thread::sleep_for(50ms);
        CHECK(taken > before);
        done = true;
        consumer.join();
        CHECK(taken >= 2);
    }

    std::printf("GUEST_EQUEUE checks=%u failures=%u\n", checks, failures);
    return failures ? 1 : 0;
}
