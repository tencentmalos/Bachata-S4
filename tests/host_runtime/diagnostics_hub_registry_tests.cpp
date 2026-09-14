// SPDX-License-Identifier: GPL-2.0-or-later
#include <atomic>
#include <cstdio>
#include <thread>
#include <vector>

#include "core/diagnostics/diagnostics_hub_registry.h"

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
    // Use a local hub (not the process singleton) so tests are independent.
    // The container has no hidden global state beyond Instance().
    {
        DiagnosticsHub hub;
        DiagnosticsSnapshot snap;
        // No session registered yet.
        CHECK(!hub.QuerySnapshot(snap, 10));
        CHECK(!snap.has_session);
        CHECK(snap.snapshot_ns == 10);
        CHECK(hub.ActiveGeneration() == 0);
        // generation 0 is rejected.
        CHECK(hub.Register(0, 1) == nullptr);
    }

    // --- register / query / revoke lifecycle ---
    {
        DiagnosticsHub hub;
        auto pub = hub.Register(5, 777);
        CHECK(pub != nullptr);
        CHECK(hub.ActiveGeneration() == 5);
        pub->SetStage("Running");
        pub->Advance(AdvanceSignal::HostPresent, 1234);

        DiagnosticsSnapshot snap;
        CHECK(hub.QuerySnapshot(snap, 42));
        CHECK(snap.has_session);
        CHECK(snap.generation == 5);
        CHECK(snap.pid == 777);
        CHECK(snap.stage == "Running");
        CHECK(snap.Counter(AdvanceSignal::HostPresent).count == 1);

        // Revoke by the owning generation clears it.
        CHECK(hub.Revoke(5));
        CHECK(hub.ActiveGeneration() == 0);
        CHECK(!hub.QuerySnapshot(snap, 43));
        CHECK(!snap.has_session);
    }

    // --- stale generation cannot revoke a newer one ---
    {
        DiagnosticsHub hub;
        (void)hub.Register(5, 1);
        // A newer generation registers, replacing gen 5's slot.
        auto pub7 = hub.Register(7, 2);
        pub7->SetStage("gen7");
        // A late teardown for the OLD generation 5 must be a no-op.
        CHECK(!hub.Revoke(5));
        CHECK(hub.ActiveGeneration() == 7);  // gen 7 still active
        DiagnosticsSnapshot snap;
        CHECK(hub.QuerySnapshot(snap, 0));
        CHECK(snap.generation == 7);
        CHECK(snap.stage == "gen7");
        // The correct generation can revoke.
        CHECK(hub.Revoke(7));
        CHECK(hub.ActiveGeneration() == 0);
    }

    // --- reader keeps the publisher alive across a concurrent revoke ---
    // A reader copies the shared_ptr, then reads; a concurrent Revoke+Register
    // must not free the publisher under it or corrupt the snapshot.
    {
        DiagnosticsHub hub;
        constexpr int kIters = 30000;
        std::atomic<bool> go{false};
        std::atomic<u64> good_reads{0};

        std::thread churner([&] {
            while (!go.load(std::memory_order_acquire)) {
            }
            for (int i = 1; i <= kIters; ++i) {
                const u64 gen = static_cast<u64>(i) + 1;  // strictly increasing, non-zero
                auto pub = hub.Register(gen, gen * 10);
                pub->Advance(AdvanceSignal::QueueSubmit, i);
                hub.Revoke(gen);
            }
        });

        std::thread reader([&] {
            while (!go.load(std::memory_order_acquire)) {
            }
            DiagnosticsSnapshot snap;
            for (int i = 0; i < kIters; ++i) {
                if (hub.QuerySnapshot(snap, i)) {
                    // If a session was seen, its identity must be internally
                    // consistent: pid == generation*10 (as the churner sets it).
                    if (snap.pid == snap.generation * 10) {
                        good_reads.fetch_add(1, std::memory_order_relaxed);
                    } else {
                        ++failures;  // torn/corrupt snapshot
                    }
                }
            }
        });

        go.store(true, std::memory_order_release);
        churner.join();
        reader.join();
        // The churner ran to completion without crashing (use-after-free guard);
        // good_reads may be any value including 0 depending on interleaving, but
        // no read was ever corrupt (checked above).
        std::printf("registry concurrency: %llu consistent reads\n",
                    static_cast<unsigned long long>(good_reads.load()));
        // Final state: last Revoke cleared it.
        CHECK(hub.ActiveGeneration() == 0);
    }

    // --- the process singleton is reachable and independent instances work ---
    {
        DiagnosticsHub& a = DiagnosticsHub::Instance();
        DiagnosticsHub& b = DiagnosticsHub::Instance();
        CHECK(&a == &b);  // same process-wide instance
    }

    // --- convenience producer pushes: no-op when empty, apply to active ---
    {
        DiagnosticsHub hub;
        // No active generation: convenience pushes must be safe no-ops.
        hub.Advance(AdvanceSignal::GuestFlip);
        hub.PublishCount(AdvanceSignal::HostPresent, 5);
        DiagnosticsSnapshot snap;
        CHECK(!hub.QuerySnapshot(snap, 0));

        // With an active generation, they land on its publisher.
        auto pub = hub.Register(3, 30);
        hub.Advance(AdvanceSignal::GuestFlip);
        hub.Advance(AdvanceSignal::GuestFlip, 2);
        hub.PublishCount(AdvanceSignal::HostPresent, 9);
        CHECK(hub.QuerySnapshot(snap, 0));
        CHECK(snap.Counter(AdvanceSignal::GuestFlip).count == 3);
        CHECK(snap.Counter(AdvanceSignal::HostPresent).count == 9);
        // After revoke, pushes are no-ops again (don't resurrect a dead gen).
        CHECK(hub.Revoke(3));
        hub.Advance(AdvanceSignal::GuestFlip);
        CHECK(!hub.QuerySnapshot(snap, 0));
        (void)pub;
    }

    std::printf("diagnostics_hub_registry: %u checks, %u failures\n", checks, failures);
    return failures == 0 ? 0 : 1;
}
