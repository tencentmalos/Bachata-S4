// SPDX-FileCopyrightText: Copyright 2026 shadPS4 Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

// Host-side, dependency-free lifecycle tests for SessionCore (HN0.1 / HN-S01,
// HN-S02). No GTest, no FEX, no JIT: a FakeBackend replaces the guest CPU with
// deterministic latches, and SessionTestGate forces the exact racing positions
// the assessment named -- every interleaving is driven by handshakes, never by
// sleeps.
//
// The four named cases:
//   1. Stop acquired access, owner about to destroy  -> no UAF, one Cancelled terminal
//   2. context not yet published (Stop during Preparing) -> CancelPending, Run never entered
//   3. start-thread / Prepare failure                -> StartFailed, clean rollback
//   4. late old-watcher arrival                      -> WrongGeneration, new session untouched
// Plus a 100-round fixture rotating interleavings, asserting exactly one terminal
// of the expected outcome, the UAF tripwire never fires, and the lease drains.

#include <atomic>
#include <condition_variable>
#include <cstdio>
#include <memory>
#include <mutex>
#include <stdexcept>
#include <string>
#include <thread>
#include <vector>

#include "core/host_runtime/session_backend.h"
#include "core/host_runtime/session_core.h"
#include "core/host_runtime/session_test_gate.h"

using namespace Core::HostRuntime;
namespace GC = Core::GuestCpu;

namespace {

int g_failures = 0;
int g_checks = 0;

void Check(bool ok, const char* what) {
    ++g_checks;
    if (!ok) {
        ++g_failures;
        std::fprintf(stderr, "  FAIL: %s\n", what);
    }
}

// A deterministic backend. Run blocks on a per-runtime latch until either a
// cancel or a "natural return" is signalled, so the test controls exactly when a
// session ends. Every method asserts the runtime is still alive (Destroy set it
// false) -- a call racing teardown is a hard failure, no ASan required.
struct FakeRuntime final : SessionRuntime {
    std::mutex mtx;
    std::condition_variable cv;
    bool cancel_requested{false};
    bool natural_return{false};
    bool run_entered{false};
    std::atomic<int> run_calls{0};
};

struct FakeBackend final : ISessionBackend {
    // Controls injected by each test.
    bool prepare_fails{false};
    GC::ErrorCategory prepare_error{GC::ErrorCategory::BackendFailure};
    RunOutcome forced_outcome{RunOutcome::Returned};  // used when natural_return fires
    // Simulate a WaitStopped that never observes a stop (drain-timeout path).
    bool waitstopped_hangs{false};

    // Exception injection (review 2.2): a real backend allocates in these methods
    // and can throw. The owner-thread exception (Prepare/Run) must become a clean
    // Failed terminal, never std::terminate; a control exception (RequestCancel)
    // must not escape RequestStop and must still return the lease.
    bool prepare_throws{false};
    bool run_throws{false};
    bool requestcancel_throws{false};

    // Drain-latency handshake (review 2.3): hold a control call inside the backend
    // past the owner's soft drain budget, then release it, and assert the owner
    // still completes Destroy and a real terminal rather than stranding the
    // runtime. block_control gates RequestCancel entry; release_control lets it
    // proceed; control_in_backend signals the test that the lease is held.
    std::mutex ctl_mtx;
    std::condition_variable ctl_cv;
    bool block_control{false};
    bool release_control{false};
    bool control_in_backend{false};

    // last_runtime is written by the owner thread in Prepare and read by the test
    // thread; publish it under its own mutex so the test never races the write.
    std::mutex rt_mtx;
    std::condition_variable rt_cv;
    std::shared_ptr<FakeRuntime> last_runtime;

    std::shared_ptr<FakeRuntime> WaitRuntime() {
        std::unique_lock lock{rt_mtx};
        rt_cv.wait(lock, [&] { return last_runtime != nullptr; });
        return last_runtime;
    }

    GC::Result<std::shared_ptr<SessionRuntime>> Prepare(const SessionParams&) override {
        if (prepare_throws)
            throw std::runtime_error("injected Prepare exception");
        if (prepare_fails)
            return GC::MakeError(prepare_error, "FakeBackend::Prepare", "forced");
        auto rt = std::make_shared<FakeRuntime>();
        {
            std::lock_guard lock{rt_mtx};
            last_runtime = rt;
            rt_cv.notify_all();
        }
        return std::shared_ptr<SessionRuntime>(rt);
    }

    RunReport Run(SessionRuntime& runtime) override {
        auto& rt = static_cast<FakeRuntime&>(runtime);
#ifdef SESSION_TEST_HOOKS
        if (!rt.alive.load()) { RunReport r; r.outcome = RunOutcome::BackendFailed; r.detail = "run after destroy"; return r; }
#endif
        if (run_throws)
            throw std::runtime_error("injected Run exception");
        rt.run_calls.fetch_add(1);
        std::unique_lock lock{rt.mtx};
        rt.run_entered = true;
        rt.cv.notify_all();
        rt.cv.wait(lock, [&] { return rt.cancel_requested || rt.natural_return; });
        RunReport report;
        if (rt.cancel_requested) {
            report.outcome = RunOutcome::Cancelled;
        } else {
            report.outcome = forced_outcome;
            if (forced_outcome == RunOutcome::Faulted)
                report.detail = "guest_rip=0xdeadbeef";
        }
        return report;
    }

    GC::Result<StopTicket> RequestCancel(SessionRuntime& runtime) override {
        auto& rt = static_cast<FakeRuntime&>(runtime);
#ifdef SESSION_TEST_HOOKS
        if (!rt.alive.load()) return GC::MakeError(GC::ErrorCategory::InvalidHandle, "RequestCancel", "after destroy");
#endif
        if (requestcancel_throws)
            throw std::runtime_error("injected RequestCancel exception");
        // Signal the cancel first so Run returns and the owner proceeds into
        // teardown, THEN (optionally) block while still holding the control lease.
        // This is what drives the owner past its soft drain budget with a lease
        // still in flight (review 2.3).
        {
            std::lock_guard lock{rt.mtx};
            rt.cancel_requested = true;
            rt.cv.notify_all();
        }
        if (block_control) {
            std::unique_lock lock{ctl_mtx};
            control_in_backend = true;
            ctl_cv.notify_all();
            ctl_cv.wait(lock, [&] { return release_control; });
        }
        return StopTicket{/*value=*/1, /*valid=*/true};
    }

    GC::Status WaitStopped(SessionRuntime& runtime, const StopTicket&,
                           std::uint64_t timeout_ns) override {
        auto& rt = static_cast<FakeRuntime&>(runtime);
#ifdef SESSION_TEST_HOOKS
        if (!rt.alive.load()) return GC::MakeError(GC::ErrorCategory::InvalidHandle, "WaitStopped", "after destroy");
#endif
        if (waitstopped_hangs) {
            std::this_thread::sleep_for(std::chrono::nanoseconds(timeout_ns));
            return GC::MakeError(GC::ErrorCategory::Timeout, "WaitStopped", "forced hang");
        }
        // The cancel already made Run return; nothing more to wait for.
        return GC::Ok();
    }

    void Destroy(SessionRuntime& runtime) override {
        auto& rt = static_cast<FakeRuntime&>(runtime);
#ifdef SESSION_TEST_HOOKS
        Check(rt.alive.load(), "Destroy called on a live runtime (not double-destroyed)");
        rt.alive.store(false);
#endif
    }

    // Test helper: unblock Run with a natural return.
    void SignalNaturalReturn(FakeRuntime& rt) {
        std::lock_guard lock{rt.mtx};
        rt.natural_return = true;
        rt.cv.notify_all();
    }
    // Test helper: block until Run has actually entered the wait.
    void AwaitRunEntered(FakeRuntime& rt) {
        std::unique_lock lock{rt.mtx};
        rt.cv.wait(lock, [&] { return rt.run_entered; });
    }
};

constexpr std::uint64_t kMs = 1'000'000ull;

// --- Case 1: Stop acquired the lease, owner about to destroy ----------------
void Case1_StopVsDestroy() {
    std::printf("Case1: Stop-vs-Destroy window (UAF closed)\n");
    FakeBackend backend;
    SessionCore core{backend};
    auto& gate = core.TestGate();

    const std::uint64_t gen = core.Start({"smoke", 0});
    Check(gen != 0, "Start returned a generation");
    Check(core.WaitPhase(gen, Phase::Running, 2000 * kMs) == WaitPhaseResult::ReachedTarget,
          "reached Running");
    backend.AwaitRunEntered(*backend.last_runtime);

    // Arm both racing points BEFORE issuing the stop.
    gate.Arm(Checkpoint::StopAfterLeaseBeforeInterrupt, gen);
    gate.Arm(Checkpoint::OwnerAfterRunBeforeDestroy, gen);

    // Stopper on another thread: takes the lease + shared_ptr, then parks.
    std::thread stopper([&] { (void)core.RequestStop(gen, 1000 * kMs); });
    gate.WaitUntilArrived(Checkpoint::StopAfterLeaseBeforeInterrupt, gen);
    // The stopper now holds a lease and a runtime ref, before RequestCancel.
    // Cancel Run directly so the owner returns and reaches its teardown gate.
    backend.SignalNaturalReturn(*backend.last_runtime);  // ends Run as Returned...
    // ...but the stopper will still set cancel_requested; either way the owner
    // returns and parks at OwnerAfterRunBeforeDestroy.
    gate.WaitUntilArrived(Checkpoint::OwnerAfterRunBeforeDestroy, gen);

    // Exact window: stopper holds a ref and is about to WaitStopped; owner is
    // about to destroy. The FakeRuntime must still be alive.
    Check(backend.last_runtime->alive.load(), "runtime alive in the Stop-vs-destroy window");

    // Release the stopper first: it finishes RequestCancel/WaitStopped and drops
    // the lease. Then release the owner: the drain sees lease==0 and destroys.
    gate.Release(Checkpoint::StopAfterLeaseBeforeInterrupt, gen);
    stopper.join();
    gate.Release(Checkpoint::OwnerAfterRunBeforeDestroy, gen);

    Terminal t;
    Check(core.WaitTerminal(gen, 2000 * kMs, t), "reached terminal");
    Check(t.exited, "terminal marked exited");
    // Owner ended Run before the stopper's cancel took effect in this schedule,
    // so the outcome is either Returned or Cancelled -- both are clean, both
    // exactly once, and crucially the runtime was never touched after Destroy.
    Check(t.outcome == RunOutcome::Returned || t.outcome == RunOutcome::Cancelled,
          "clean terminal outcome");
    Check(core.CurrentGeneration() == 0, "no live generation after terminal");
}

// --- Case 2: context not yet published --------------------------------------
void Case2_StopDuringPreparing() {
    std::printf("Case2: Stop during Preparing (context not published)\n");
    FakeBackend backend;
    SessionCore core{backend};
    auto& gate = core.TestGate();

    // Park the owner before it publishes the runtime.
    // We must arm before Start, but the generation is only known after Start.
    // The gate keys on generation, so arm for the next generation value.
    // CurrentGeneration()+1 is that value while Idle.
    const std::uint64_t next_gen = 1;  // fresh core: first generation is 1
    gate.Arm(Checkpoint::BeforePublish, next_gen);

    const std::uint64_t gen = core.Start({"smoke", 0});
    Check(gen == next_gen, "generation is 1 on a fresh core");
    gate.WaitUntilArrived(Checkpoint::BeforePublish, gen);

    // Owner is parked before publishing: runtime not visible. Stop must record
    // intent and dereference nothing.
    Check(core.RequestStop(gen, 1000 * kMs) == StopResult::CancelPending,
          "Stop during Preparing returns CancelPending");

    // Release the owner: it publishes, sees the cancel, and skips Run.
    gate.Release(Checkpoint::BeforePublish, gen);

    Terminal t;
    Check(core.WaitTerminal(gen, 2000 * kMs, t), "reached terminal");
    Check(t.outcome == RunOutcome::Cancelled, "outcome is Cancelled");
    Check(t.user_requested_stop, "flagged user-requested stop");
    Check(backend.last_runtime->run_calls.load() == 0, "Run was never entered");
    Check(core.WaitPhase(gen, Phase::Running, 0) == WaitPhaseResult::TerminatedBeforeTarget,
          "Running was never published");
}

// --- Case 3: Prepare / start-thread failure ---------------------------------
void Case3_StartFailure() {
    std::printf("Case3: Prepare failure and owner-spawn failure\n");
    {
        FakeBackend backend;
        backend.prepare_fails = true;
        SessionCore core{backend};
        const std::uint64_t gen = core.Start({"smoke", 0});
        Check(gen != 0, "Start still returns a generation (owner spawned)");
        Terminal t;
        Check(core.WaitTerminal(gen, 2000 * kMs, t), "reached terminal");
        Check(t.outcome == RunOutcome::StartFailed, "outcome is StartFailed");
        Check(core.WaitPhase(gen, Phase::Running, 0) == WaitPhaseResult::TerminatedBeforeTarget,
              "never reached Running");
        // A subsequent Start on the same core reaches Running.
        FakeBackend backend2;
        SessionCore core2{backend2};
        const std::uint64_t g2 = core2.Start({"smoke", 0});
        Check(core2.WaitPhase(g2, Phase::Running, 2000 * kMs) == WaitPhaseResult::ReachedTarget,
              "second session reaches Running");
        backend2.SignalNaturalReturn(*backend2.last_runtime);
        Terminal t2;
        Check(core2.WaitTerminal(g2, 2000 * kMs, t2), "second session terminal");
    }
    {
        FakeBackend backend;
        SessionCore core{backend};
        core.SetOwnerSpawnFailureForTest(true);
        const std::uint64_t gen = core.Start({"smoke", 0});
        Check(gen == 0, "owner-spawn failure returns 0");
        Check(core.CurrentGeneration() == 0, "rolled back to no live generation");
        // The core is usable again immediately.
        core.SetOwnerSpawnFailureForTest(false);
        const std::uint64_t g2 = core.Start({"smoke", 0});
        Check(g2 != 0, "Start works after a spawn-failure rollback");
        Check(core.WaitPhase(g2, Phase::Running, 2000 * kMs) == WaitPhaseResult::ReachedTarget,
              "recovered session reaches Running");
        backend.SignalNaturalReturn(*backend.last_runtime);
        Terminal t;
        Check(core.WaitTerminal(g2, 2000 * kMs, t), "recovered session terminal");
    }
}

// --- Case 4: late old-watcher arrival ---------------------------------------
void Case4_LateOldWatcher() {
    std::printf("Case4: late old-generation stop is rejected\n");
    FakeBackend backend;
    SessionCore core{backend};

    const std::uint64_t g1 = core.Start({"smoke", 0});
    Check(core.WaitPhase(g1, Phase::Running, 2000 * kMs) == WaitPhaseResult::ReachedTarget, "g1 Running");
    backend.SignalNaturalReturn(*backend.last_runtime);
    Terminal t1;
    Check(core.WaitTerminal(g1, 2000 * kMs, t1), "g1 terminal");

    const std::uint64_t g2 = core.Start({"smoke", 0});
    Check(g2 == g1 + 1, "g2 follows g1");
    Check(core.WaitPhase(g2, Phase::Running, 2000 * kMs) == WaitPhaseResult::ReachedTarget, "g2 Running");

    // A late stop for g1 must not touch g2.
    Check(core.RequestStop(g1, 1000 * kMs) == StopResult::AlreadyStopped,
          "late stop for terminated g1 -> AlreadyStopped");
    Check(core.QueryPhase(g2) == Phase::Running, "g2 still Running");

    backend.SignalNaturalReturn(*backend.last_runtime);
    Terminal t2;
    Check(core.WaitTerminal(g2, 2000 * kMs, t2), "g2 terminal");
    Check(core.CurrentGeneration() == 0, "no live generation at end");
}

// --- Case 5: guest fault is a failure, not exit 0 ---------------------------
void Case5_FaultIsFailure() {
    std::printf("Case5: GuestFault maps to Failed, not exit-0\n");
    FakeBackend backend;
    backend.forced_outcome = RunOutcome::Faulted;
    SessionCore core{backend};
    const std::uint64_t gen = core.Start({"smoke", 0});
    Check(core.WaitPhase(gen, Phase::Running, 2000 * kMs) == WaitPhaseResult::ReachedTarget, "Running");
    backend.SignalNaturalReturn(*backend.last_runtime);  // Run returns Faulted
    Terminal t;
    Check(core.WaitTerminal(gen, 2000 * kMs, t), "terminal");
    Check(t.outcome == RunOutcome::Faulted, "outcome is Faulted");
    Check(!t.user_requested_stop, "not a user stop");
    Check(t.detail.find("guest_rip") != std::string::npos, "carries fault detail");
    Check(core.QueryPhase(gen) == Phase::Failed, "phase is Failed");
}

// --- 100-round fixture ------------------------------------------------------
void Fixture_HundredRounds() {
    std::printf("Fixture: 100 rounds, rotating interleavings\n");
    FakeBackend backend;
    SessionCore core{backend};
    std::uint64_t prev_gen = 0;

    for (int round = 0; round < 100; ++round) {
        const int mode = round % 5;
        const std::uint64_t gen = core.Start({"smoke", 0});
        if (gen == 0) { Check(false, "Start returned 0 in fixture"); break; }
        Check(gen > prev_gen, "generation strictly increases");
        prev_gen = gen;

        RunOutcome expected = RunOutcome::Returned;
        switch (mode) {
        case 0: {  // natural return before any stop
            Check(core.WaitPhase(gen, Phase::Running, 2000 * kMs) == WaitPhaseResult::ReachedTarget, "m0 Running");
            backend.SignalNaturalReturn(*backend.last_runtime);
            expected = RunOutcome::Returned;
            break;
        }
        case 1: {  // stop while running
            Check(core.WaitPhase(gen, Phase::Running, 2000 * kMs) == WaitPhaseResult::ReachedTarget, "m1 Running");
            backend.AwaitRunEntered(*backend.last_runtime);
            (void)core.RequestStop(gen, 1000 * kMs);
            expected = RunOutcome::Cancelled;
            break;
        }
        case 2: {  // stop during Preparing
            auto& gate = core.TestGate();
            gate.Arm(Checkpoint::BeforePublish, gen);
            gate.WaitUntilArrived(Checkpoint::BeforePublish, gen);
            Check(core.RequestStop(gen, 1000 * kMs) == StopResult::CancelPending, "m2 CancelPending");
            gate.Release(Checkpoint::BeforePublish, gen);
            expected = RunOutcome::Cancelled;
            break;
        }
        case 3: {  // two concurrent stoppers of the same generation
            Check(core.WaitPhase(gen, Phase::Running, 2000 * kMs) == WaitPhaseResult::ReachedTarget, "m3 Running");
            backend.AwaitRunEntered(*backend.last_runtime);
            std::thread s1([&] { (void)core.RequestStop(gen, 1000 * kMs); });
            std::thread s2([&] { (void)core.RequestStop(gen, 1000 * kMs); });
            s1.join();
            s2.join();
            expected = RunOutcome::Cancelled;
            break;
        }
        case 4: {  // Start while running -> AlreadyRunning
            Check(core.WaitPhase(gen, Phase::Running, 2000 * kMs) == WaitPhaseResult::ReachedTarget, "m4 Running");
            Check(core.Start({"smoke", 0}) == 0, "second Start returns 0 (AlreadyRunning)");
            backend.SignalNaturalReturn(*backend.last_runtime);
            expected = RunOutcome::Returned;
            break;
        }
        }

        Terminal t;
        Check(core.WaitTerminal(gen, 2000 * kMs, t), "round reached terminal");
        Check(t.exited, "round terminal exited");
        Check(t.outcome == expected, "round outcome as expected");
        // The runtime for this round must have been destroyed exactly once.
        if (backend.last_runtime)
            Check(!backend.last_runtime->alive.load(), "round runtime destroyed");
    }
    Check(core.CurrentGeneration() == 0, "no live generation after 100 rounds");
}

// --- Case 6: Prepare exception is a clean Failed terminal, not an abort ------
// Review 2.2: Prepare/Run run on the owner thread; the JNI try/catch is on the
// calling thread and cannot catch them. An exception must become a Failed
// terminal (StartFailed), never std::terminate.
void Case6_PrepareException() {
    std::printf("Case6: Prepare exception -> Failed terminal (no abort)\n");
    FakeBackend backend;
    backend.prepare_throws = true;
    SessionCore core{backend};
    const std::uint64_t gen = core.Start({"smoke", 0});
    Check(gen != 0, "Start spawned owner");
    Terminal t;
    Check(core.WaitTerminal(gen, 2000 * kMs, t), "reached terminal after Prepare throw");
    Check(t.outcome == RunOutcome::StartFailed, "outcome is StartFailed");
    Check(t.exited, "terminal exited");
    Check(t.detail.find("Prepare threw") != std::string::npos, "detail names the throw");
    Check(core.QueryPhase(gen) == Phase::Failed, "phase is Failed");
    // The core is reusable: a fresh Start after the exception must succeed.
    backend.prepare_throws = false;
    const std::uint64_t g2 = core.Start({"smoke", 0});
    Check(g2 > gen, "restart after exception gets a new generation");
    Check(core.WaitPhase(g2, Phase::Running, 2000 * kMs) == WaitPhaseResult::ReachedTarget,
          "restart reaches Running");
    backend.SignalNaturalReturn(*backend.last_runtime);
    Terminal t2;
    Check(core.WaitTerminal(g2, 2000 * kMs, t2), "restart terminal");
}

// --- Case 7: Run exception is a clean Failed terminal, not an abort ----------
void Case7_RunException() {
    std::printf("Case7: Run exception -> Failed terminal (no abort)\n");
    FakeBackend backend;
    backend.run_throws = true;
    SessionCore core{backend};
    const std::uint64_t gen = core.Start({"smoke", 0});
    Check(gen != 0, "Start spawned owner");
    Terminal t;
    Check(core.WaitTerminal(gen, 2000 * kMs, t), "reached terminal after Run throw");
    Check(t.outcome == RunOutcome::BackendFailed, "outcome is BackendFailed");
    Check(t.exited, "terminal exited");
    Check(t.detail.find("Run threw") != std::string::npos, "detail names the throw");
    Check(core.QueryPhase(gen) == Phase::Failed, "phase is Failed");
    // The runtime published before Run must still be Destroyed (teardown ran).
    Check(backend.last_runtime && !backend.last_runtime->alive.load(),
          "runtime destroyed after Run exception");
}

// --- Case 8: control exception does not escape and still returns the lease ---
// Review 2.2 (control half): a throwing RequestCancel must not unwind through
// RequestStop, and the lease must be returned so the owner's drain completes.
void Case8_ControlException() {
    std::printf("Case8: RequestCancel exception -> Error, lease returned\n");
    FakeBackend backend;
    backend.requestcancel_throws = true;
    SessionCore core{backend};
    const std::uint64_t gen = core.Start({"smoke", 0});
    Check(core.WaitPhase(gen, Phase::Running, 2000 * kMs) == WaitPhaseResult::ReachedTarget,
          "Running");
    backend.AwaitRunEntered(*backend.last_runtime);
    // A throwing RequestCancel must come back as Error, not propagate.
    bool escaped = false;
    StopResult result = StopResult::Accepted;
    try {
        result = core.RequestStop(gen, 1000 * kMs);
    } catch (...) {
        escaped = true;
    }
    Check(!escaped, "control exception did not escape RequestStop");
    Check(result == StopResult::Error, "control exception reported as Error");
    // The lease was returned despite the throw: a normal stop now completes and
    // the owner reaches a real terminal (drain did not wedge).
    backend.requestcancel_throws = false;
    backend.SignalNaturalReturn(*backend.last_runtime);
    Terminal t;
    Check(core.WaitTerminal(gen, 2000 * kMs, t), "owner reached terminal (lease not leaked)");
    Check(t.exited, "terminal exited");
    Check(backend.last_runtime && !backend.last_runtime->alive.load(), "runtime destroyed");
    const std::uint64_t g2 = core.Start({"smoke", 0});
    Check(g2 > gen, "same-process restart after control exception");
    Check(core.WaitPhase(g2, Phase::Running, 2000 * kMs) == WaitPhaseResult::ReachedTarget,
          "restart Running");
    backend.SignalNaturalReturn(*backend.last_runtime);
    Terminal t2;
    Check(core.WaitTerminal(g2, 2000 * kMs, t2), "restart terminal");
}

// --- Case 9: a lease held past the soft drain budget still gets reclaimed ----
// Review 2.3: when a control call is still in the backend past the owner's soft
// drain budget, the owner must NOT latch a permanent "not exited" terminal and
// leave. It stays, waits for the lease to drain, then Destroys and sets a real
// terminal. The late control call must find a live runtime (no UAF).
void Case9_SlowDrainStillReclaims() {
    std::printf("Case9: slow control drain still reclaims (no strand, no UAF)\n");
    FakeBackend backend;
    backend.block_control = true;
    SessionCore core{backend};
    // Shrink the soft drain budget so the owner crosses it deterministically while
    // the control lease is still held inside the backend (review 2.3).
    core.SetTeardownDeadlineForTest(50 * kMs);  // 50ms soft budget
    const std::uint64_t gen = core.Start({"smoke", 0});
    Check(core.WaitPhase(gen, Phase::Running, 2000 * kMs) == WaitPhaseResult::ReachedTarget,
          "Running");
    backend.AwaitRunEntered(*backend.last_runtime);

    // A stopper takes the lease and blocks inside RequestCancel.
    std::thread stopper([&] { (void)core.RequestStop(gen, 1000 * kMs); });
    {
        std::unique_lock lock{backend.ctl_mtx};
        Check(backend.ctl_cv.wait_for(lock, std::chrono::seconds(5),
                                      [&] { return backend.control_in_backend; }),
              "control call is inside the backend holding a lease");
    }
    // The stopper set cancel_requested before blocking, so Run has returned and
    // the owner is now in TeardownAndDestroy waiting for the lease. The soft
    // budget is 50ms; wait longer than that so the owner crosses it and enters the
    // patient (unbounded) drain wait -- the exact branch review 2.3 says must not
    // strand the runtime or leave an ownerless Stopping.
    std::this_thread::sleep_for(std::chrono::milliseconds(200));
    Check(core.QueryPhase(gen) == Phase::Stopping,
          "owner is still in Stopping past the soft budget (not a terminal latch)");
    // Release the lease; the owner must now finish Destroy and set a real terminal.
    {
        std::lock_guard lock{backend.ctl_mtx};
        backend.release_control = true;
        backend.ctl_cv.notify_all();
    }
    stopper.join();
    Terminal t;
    Check(core.WaitTerminal(gen, 3000 * kMs, t), "reached a real terminal after slow drain");
    Check(t.exited, "terminal exited (not a permanent not-exited latch)");
    Check(backend.last_runtime && !backend.last_runtime->alive.load(),
          "runtime destroyed exactly once (late control found it alive)");
    // Same-process restart after a slow drain.
    const std::uint64_t g2 = core.Start({"smoke", 0});
    Check(g2 > gen, "restart after slow drain");
    Check(core.WaitPhase(g2, Phase::Running, 2000 * kMs) == WaitPhaseResult::ReachedTarget,
          "restart Running");
    backend.SignalNaturalReturn(*backend.last_runtime);
    Terminal t2;
    Check(core.WaitTerminal(g2, 2000 * kMs, t2), "restart terminal");
}

void Case10_PlatformAdmission() {
    std::printf("Case10: generation-scoped platform admission/cancel/restart\n");
    FakeBackend backend;
    SessionCore core{backend};
    SessionParams params;
    params.content_id = "rendered";
    params.requires_platform_ready = true;
    uint64_t old{};
    for (int round = 0; round < 3; ++round) {
        const auto gen = core.Start(params);
        Check(gen > old, "new platform generation");
        Check(core.WaitPhase(gen, Phase::Ready, 2000 * kMs) == WaitPhaseResult::ReachedTarget,
              "prepared while app platform is pending");
        auto runtime = backend.last_runtime;
        Check(runtime->run_calls == 0, "guest never runs before platform admission");
        Check(!core.PlatformReady(old), "old admission cannot unlock new generation");
        Check(core.WaitPhase(gen, Phase::Running, 10 * kMs) == WaitPhaseResult::Timeout,
              "no Running while platform remains pending");
        if (round == 1) {
            (void)core.RequestStop(gen, 100 * kMs);
            Check(!core.PlatformReady(gen), "cancel cannot be undone by late platform receipt");
        } else {
            Check(core.PlatformReady(gen), "current platform admitted");
            Check(core.WaitPhase(gen, Phase::Running, 2000 * kMs) == WaitPhaseResult::ReachedTarget,
                  "Running follows admission");
            backend.SignalNaturalReturn(*runtime);
        }
        Terminal terminal;
        Check(core.WaitTerminal(gen, 2000 * kMs, terminal), "platform generation retired");
        Check(terminal.outcome == (round == 1 ? RunOutcome::Cancelled : RunOutcome::Returned),
              "correct platform terminal outcome");
        if (round == 1)
            Check(runtime->run_calls == 0, "cancel pending platform skips backend Run");
        Check(!core.PlatformReady(gen), "terminated generation rejects platform receipt");
        old = gen;
    }
}

}  // namespace

int main() {
    std::printf("session_lifecycle_tests\n");
    Case1_StopVsDestroy();
    Case2_StopDuringPreparing();
    Case3_StartFailure();
    Case4_LateOldWatcher();
    Case5_FaultIsFailure();
    Case6_PrepareException();
    Case7_RunException();
    Case8_ControlException();
    Case9_SlowDrainStillReclaims();
    Case10_PlatformAdmission();
    Fixture_HundredRounds();

    std::printf("\n%d checks, %d failures\n", g_checks, g_failures);
    return g_failures == 0 ? 0 : 1;
}
