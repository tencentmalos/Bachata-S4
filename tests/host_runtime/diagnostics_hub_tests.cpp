// SPDX-License-Identifier: GPL-2.0-or-later
#include <atomic>
#include <cstdio>
#include <thread>
#include <vector>

#include "core/diagnostics/diagnostics_hub.h"

using namespace Core::Diagnostics;

static unsigned checks{}, failures{};
#define CHECK(x)                                                                                    \
    do {                                                                                            \
        ++checks;                                                                                   \
        if (!(x)) {                                                                                 \
            ++failures;                                                                             \
            std::printf("FAIL line %d: %s\n", __LINE__, #x);                                        \
        }                                                                                           \
    } while (0)

int main() {
    // --- empty publisher: no session ---
    {
        DiagnosticsPublisher pub;
        DiagnosticsSnapshot snap;
        pub.CopyInto(snap, 100);
        CHECK(!snap.has_session);
        CHECK(snap.generation == 0);
        CHECK(snap.snapshot_ns == 100);
        // All counters default to count 0, available true, never advanced.
        CHECK(snap.Counter(AdvanceSignal::HostPresent).count == 0);
        CHECK(snap.Counter(AdvanceSignal::HostPresent).available);
        CHECK(snap.Counter(AdvanceSignal::HostPresent).last_advance_ns == 0);
    }

    // --- identity + phase + strings ---
    {
        DiagnosticsPublisher pub;
        pub.SetIdentity(42, 12345);
        pub.SetPhase(3);
        pub.SetRunUuid("run-xyz");
        pub.SetStage("Running");
        pub.SetStopReason("");
        DiagnosticsSnapshot snap;
        pub.CopyInto(snap, 7);
        CHECK(snap.has_session);
        CHECK(snap.generation == 42);
        CHECK(pub.Generation() == 42);
        CHECK(snap.pid == 12345);
        CHECK(snap.phase == 3);
        CHECK(snap.run_uuid == "run-xyz");
        CHECK(snap.stage == "Running");
        CHECK(snap.stop_reason.empty());
    }

    // --- unavailable is distinct from 0 (spec §3.1) ---
    {
        DiagnosticsPublisher pub;
        pub.SetIdentity(1, 1);
        pub.MarkAvailable(AdvanceSignal::GpuRetire, false);
        DiagnosticsSnapshot snap;
        pub.CopyInto(snap, 0);
        CHECK(!snap.Counter(AdvanceSignal::GpuRetire).available);  // "cannot measure"
        CHECK(snap.Counter(AdvanceSignal::GpuRetire).count == 0);
        // A different signal at 0 is still "available and genuinely 0".
        CHECK(snap.Counter(AdvanceSignal::HostPresent).available);
        CHECK(snap.Counter(AdvanceSignal::HostPresent).count == 0);
    }

    // --- Advance: delta + timestamp ---
    {
        DiagnosticsPublisher pub;
        pub.SetIdentity(1, 1);
        pub.Advance(AdvanceSignal::HostDraw, 1000);
        pub.Advance(AdvanceSignal::HostDraw, 2000, 3);
        DiagnosticsSnapshot snap;
        pub.CopyInto(snap, 0);
        CHECK(snap.Counter(AdvanceSignal::HostDraw).count == 4);
        CHECK(snap.Counter(AdvanceSignal::HostDraw).last_advance_ns == 2000);
    }

    // --- PublishCount: absolute, timestamp only on forward progress ---
    {
        DiagnosticsPublisher pub;
        pub.SetIdentity(1, 1);
        pub.PublishCount(AdvanceSignal::HostPresent, 10, 500);
        DiagnosticsSnapshot s1;
        pub.CopyInto(s1, 0);
        CHECK(s1.Counter(AdvanceSignal::HostPresent).count == 10);
        CHECK(s1.Counter(AdvanceSignal::HostPresent).last_advance_ns == 500);
        // Same value again: no forward progress -> timestamp must NOT advance.
        pub.PublishCount(AdvanceSignal::HostPresent, 10, 900);
        DiagnosticsSnapshot s2;
        pub.CopyInto(s2, 0);
        CHECK(s2.Counter(AdvanceSignal::HostPresent).count == 10);
        CHECK(s2.Counter(AdvanceSignal::HostPresent).last_advance_ns == 500);  // unchanged
        // Forward: timestamp updates.
        pub.PublishCount(AdvanceSignal::HostPresent, 11, 1300);
        DiagnosticsSnapshot s3;
        pub.CopyInto(s3, 0);
        CHECK(s3.Counter(AdvanceSignal::HostPresent).count == 11);
        CHECK(s3.Counter(AdvanceSignal::HostPresent).last_advance_ns == 1300);
    }

    // --- concurrency: many writers Advance while a reader copies; no lost counts,
    //     reader never blocks or crashes ---
    {
        DiagnosticsPublisher pub;
        pub.SetIdentity(99, 99);
        constexpr int kThreads = 8;
        constexpr int kPerThread = 20000;
        std::atomic<bool> go{false};
        std::atomic<bool> stop_reader{false};
        std::atomic<u64> reads{0};

        std::thread reader([&] {
            DiagnosticsSnapshot snap;
            while (!stop_reader.load(std::memory_order_acquire)) {
                pub.CopyInto(snap, 1);
                // reader must always see a consistent generation
                if (snap.generation != 99) {
                    ++failures;
                }
                reads.fetch_add(1, std::memory_order_relaxed);
            }
        });

        std::vector<std::thread> writers;
        for (int t = 0; t < kThreads; ++t) {
            writers.emplace_back([&] {
                while (!go.load(std::memory_order_acquire)) {
                }
                for (int i = 0; i < kPerThread; ++i) {
                    pub.Advance(AdvanceSignal::QueueSubmit, static_cast<u64>(i));
                }
            });
        }
        go.store(true, std::memory_order_release);
        for (auto& w : writers) {
            w.join();
        }
        stop_reader.store(true, std::memory_order_release);
        reader.join();

        DiagnosticsSnapshot snap;
        pub.CopyInto(snap, 0);
        CHECK(snap.Counter(AdvanceSignal::QueueSubmit).count ==
              static_cast<u64>(kThreads) * kPerThread);  // no lost increments
        CHECK(reads.load() > 0);                          // reader made progress
        std::printf("concurrency: %llu reads, final count %llu\n",
                    static_cast<unsigned long long>(reads.load()),
                    static_cast<unsigned long long>(snap.Counter(AdvanceSignal::QueueSubmit).count));
    }

    std::printf("diagnostics_hub: %u checks, %u failures\n", checks, failures);
    return failures == 0 ? 0 : 1;
}
