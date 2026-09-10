// Host-deterministic test for the test-only Run-entry gate state machine
// (src/core/guest_cpu/fex/test_run_gate.h). Compiled on the build host with no FEX dependency; it
// exercises exactly the protocol the on-device G24 suite drives, including the concurrency negatives
// the N1/N2 review reproduced (re-arm before exit; one exit confirming two releases; owner timeout).
#include "core/guest_cpu/fex/test_run_gate.h"

#include <cassert>
#include <chrono>
#include <cstdio>
#include <thread>

using Core::GuestCpu::TestGate::RunGate;

static int g_failures = 0;
#define EXPECT(cond, msg) do { \
    if (!(cond)) { std::fprintf(stderr, "FAIL: %s\n", msg); ++g_failures; } \
} while (0)

int main() {
    constexpr std::uint64_t CTX = 7, TID = 11, INVOC = 3;

    // 1. No arm: WaitAtEntry passes straight through.
    {
        RunGate gate;
        EXPECT(gate.WaitAtEntry(CTX, TID, INVOC, 100), "pass-through when idle");
        EXPECT(gate.CurrentPhase() == RunGate::Phase::Idle, "idle after pass-through");
    }

    // 2. Arm -> owner arrives -> release cleanly; Exited observed; another arm then allowed.
    {
        RunGate gate;
        auto token = gate.Arm(TID, CTX);
        EXPECT(token != 0, "arm returns token");
        std::thread owner([&] {
            EXPECT(gate.WaitAtEntry(CTX, TID, INVOC, 1000), "owner released cleanly");
        });
        for (int i = 0; i < 200 && !gate.Arrived(token); ++i) std::this_thread::yield();
        EXPECT(gate.Arrived(token), "arrived after arm");
        EXPECT(gate.BoundInvocation(token) == INVOC, "bound invocation reported");
        EXPECT(!gate.Exited(token), "not exited before release");
        EXPECT(gate.Release(token, 2000), "release returns true after owner leaves");
        EXPECT(gate.Exited(token), "exited after release");
        owner.join();
        auto token2 = gate.Arm(TID, CTX);
        EXPECT(token2 != token && token2 != 0, "next arm allowed after clean exit");
        EXPECT(!gate.Release(token2, 50), "release before arrival fails (new token)");
        // let the would-be owner time out / close; gate stays clean for this test process.
    }

    // 3. Re-arm is refused while the previous owner has not exited.
    {
        RunGate gate;
        auto t1 = gate.Arm(TID, CTX);
        std::thread owner([&] { gate.WaitAtEntry(CTX, TID, INVOC, 5000); });
        for (int i = 0; i < 200 && !gate.Arrived(t1); ++i) std::this_thread::yield();
        // Owner is held (Arrived). A new arm must be rejected, not take over.
        EXPECT(gate.Arm(TID, CTX) == 0, "re-arm refused while owner held");
        EXPECT(gate.Arm(TID + 1, CTX) == 0, "re-arm other thread refused while held");
        gate.Release(t1, 2000);
        owner.join();
        EXPECT(gate.Exited(t1), "owner exited after release");
        EXPECT(gate.Arm(TID, CTX) != 0, "arm accepted only after prior exit");
    }

    // 4. A stale/unknown token release is rejected and does not affect the current generation.
    {
        RunGate gate;
        auto t1 = gate.Arm(TID, CTX);
        std::thread owner([&] { gate.WaitAtEntry(CTX, TID, INVOC, 5000); });
        for (int i = 0; i < 200 && !gate.Arrived(t1); ++i) std::this_thread::yield();
        EXPECT(!gate.Release(t1 + 999, 100), "bogus token release rejected");
        EXPECT(gate.Arrived(t1), "current generation untouched by bogus release");
        EXPECT(!gate.Release(0, 100), "zero token release rejected");
        gate.Release(t1, 2000);
        owner.join();
    }

    // 5. One owner exit confirms exactly one release: a second release call is idempotent (returns
    //    the same true result) but can never confirm a different/newer arm.
    {
        RunGate gate;
        auto t1 = gate.Arm(TID, CTX);
        std::thread owner([&] { gate.WaitAtEntry(CTX, TID, INVOC, 5000); });
        for (int i = 0; i < 200 && !gate.Arrived(t1); ++i) std::this_thread::yield();
        EXPECT(gate.Release(t1, 2000), "first release succeeds");
        EXPECT(gate.Release(t1, 2000), "repeat release of same token is idempotent-true");
        owner.join();
        // A token for a never-armed generation after close must be rejected, not confirmed by the
        // previous owner's exit.
        EXPECT(!gate.Release(t1 + 1, 100), "a different token is not confirmed by this exit");
    }

    // 6. Wrong context/thread owner is not held: the armed token never arrives for it.
    {
        RunGate gate;
        auto t1 = gate.Arm(TID, CTX);
        bool passed = gate.WaitAtEntry(CTX + 1, TID, INVOC, 50);  // wrong context
        EXPECT(passed, "wrong context passes through");
        EXPECT(!gate.Arrived(t1), "armed token not arrived for the matching owner");
        bool passed2 = gate.WaitAtEntry(CTX, TID + 5, INVOC, 50);  // wrong thread
        EXPECT(passed2, "wrong thread passes through");
        EXPECT(!gate.Arrived(t1), "still not arrived");
        // Clean up the held generation: bringing the real owner in then releasing.
        std::thread owner([&] { gate.WaitAtEntry(CTX, TID, INVOC, 5000); });
        for (int i = 0; i < 200 && !gate.Arrived(t1); ++i) std::this_thread::yield();
        EXPECT(gate.Arrived(t1), "matching owner arrives");
        gate.Release(t1, 2000);
        owner.join();
    }

    // 7. Owner timeout: WaitAtEntry returns false and closes the generation; the gate reports a
    //    TimedOut (not a clean exit) but is released back to Idle so a later arm can proceed.
    {
        RunGate gate;
        auto t1 = gate.Arm(TID, CTX);
        bool result = true;
        std::thread owner([&] { result = gate.WaitAtEntry(CTX, TID, INVOC, 120); });
        for (int i = 0; i < 200 && !gate.Arrived(t1); ++i) std::this_thread::yield();
        // Controller never releases; owner must time out.
        owner.join();
        EXPECT(!result, "owner times out -> false");
        EXPECT(gate.CurrentPhase() == RunGate::Phase::Idle, "gate back to idle after timeout");
        EXPECT(gate.Arm(TID, CTX) != 0, "next arm proceeds after timed-out generation");
    }

    if (g_failures == 0) {
        std::printf("test_run_gate_protocol: ALL GATE NEGATIVES PASS\n");
        return 0;
    }
    std::printf("test_run_gate_protocol: %d failure(s)\n", g_failures);
    return 1;
}
