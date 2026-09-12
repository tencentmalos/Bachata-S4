// SPDX-FileCopyrightText: Copyright 2026 shadPS4 Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

// SessionCore: the backend-free, JNI-free lifecycle owner for one embeddable
// guest session.
//
// This class exists because the previous JNI code (fex_session_jni.cpp) had a
// use-after-free (a non-owner Stop dereferenced a CpuContext the owner thread
// could destroy concurrently), lost cancel intent requested during preparation,
// raced two callers on one std::thread::join, and reported guest faults as a
// clean exit-0. The lifetime model here fixes all four:
//
//   * shared_ptr<SessionRuntime>  -- memory safety. A Stop copies the shared_ptr
//     under the lock, so the runtime cannot be freed while the Stop is inside a
//     backend call, even as the owner finishes Run.
//   * control-lease counter       -- ordering. The owner tears down (Destroy +
//     drop the runtime) only after every in-flight control call has returned, so
//     Destroy runs with the owner holding the last reference: no concurrent
//     dereference, and no lock is ever held across a blocking wait.
//   * generation                  -- identity. Every request and published state
//     names the generation it belongs to; a stale key or a late old observer
//     cannot touch a newer session.
//   * terminal-once latch         -- exactly one terminal record per generation;
//     repeat Stop/Query returns it idempotently.
//   * persistent cancel intent    -- a Stop during Preparing (before the runtime
//     is published) records intent instead of dereferencing null; the owner
//     honors it before entering a long Run.
//
// SessionCore is process-global in practice (one live session), but it is a
// plain class with an injected backend so the host unit test can drive it with a
// deterministic FakeBackend and no FEX.

#pragma once

#include <condition_variable>
#include <cstdint>
#include <mutex>
#include <string>
#include <thread>

#include "core/host_runtime/session_backend.h"

#ifdef SESSION_TEST_HOOKS
#include "core/host_runtime/session_test_gate.h"
#endif

namespace Core::HostRuntime {

// The public lifecycle states. Matches the spec §1.2 list. Ready = resources
// prepared, before Run; Running = executing; Stopping = teardown in progress.
enum class Phase : std::uint32_t {
    Idle = 0,
    Preparing,
    Ready,
    Running,
    Stopping,
    Stopped,
    Failed,
};

[[nodiscard]] const char* ToString(Phase phase) noexcept;

[[nodiscard]] constexpr bool IsTerminal(Phase phase) noexcept {
    return phase == Phase::Stopped || phase == Phase::Failed;
}

// The single, immutable outcome of one generation.
struct Terminal final {
    RunOutcome outcome{RunOutcome::Returned};
    std::uint32_t error_category{0};  // guest_cpu ErrorCategory ordinal, 0 = None
    std::string detail;
    bool user_requested_stop{false};  // outcome == Cancelled
    bool exited{false};               // reached a terminal (vs. drain-timeout "not exited")
};

// Result of a RequestStop call. Distinct values so the caller (and UI) can tell
// "accepted" from "already gone" from "wrong session" without parsing strings.
enum class StopResult : std::uint32_t {
    Accepted = 0,       // interrupt delivered and the target stopped within budget
    CancelPending,      // recorded during Preparing; the owner will honor it
    AlreadyStopping,    // teardown already in progress
    AlreadyStopped,     // this generation already reached a terminal
    WrongGeneration,    // gen does not match the current session
    Timeout,            // the stop was requested but did not complete in budget
    Error,              // backend RequestCancel/WaitStopped returned an error
};

[[nodiscard]] const char* ToString(StopResult result) noexcept;

// Result of a WaitPhase call.
enum class WaitPhaseResult : std::uint32_t {
    ReachedTarget = 0,
    TerminatedBeforeTarget,  // reached a terminal without passing through target
    WrongGeneration,
    Timeout,
};

class SessionCore {
public:
    explicit SessionCore(ISessionBackend& backend);
    ~SessionCore();

    SessionCore(const SessionCore&) = delete;
    SessionCore& operator=(const SessionCore&) = delete;

    // Starts a new session on a fresh owner thread. Returns the new generation,
    // or 0 if a session is already active (AlreadyRunning) or the owner thread
    // could not be spawned (state rolled back to Idle).
    [[nodiscard]] std::uint64_t Start(const SessionParams& params);

    // Requests a stop of `generation`. Bounded by timeout_ns for the CPU stop
    // receipt (0 = 1s default). Never joins here.
    [[nodiscard]] StopResult RequestStop(std::uint64_t generation, std::uint64_t timeout_ns);

    // Non-blocking snapshot.
    [[nodiscard]] Phase QueryPhase(std::uint64_t generation) const;
    [[nodiscard]] std::uint64_t CurrentGeneration() const;

    // Blocks until `generation` reaches `target` (or a later phase), a terminal,
    // a generation change, or the deadline. deadline_ns = 0 waits indefinitely.
    [[nodiscard]] WaitPhaseResult WaitPhase(std::uint64_t generation, Phase target,
                                            std::uint64_t deadline_ns);

    // Blocks until `generation` reaches a terminal or the deadline. On timeout,
    // ownership is retained (the session is NOT declared Idle) and the returned
    // bool is false. On success, joins the owner thread (the single join site)
    // and returns the immutable Terminal.
    [[nodiscard]] bool WaitTerminal(std::uint64_t generation, std::uint64_t deadline_ns,
                                    Terminal& out);

    // Reads the terminal record for a generation if one exists (idempotent).
    [[nodiscard]] bool TryGetTerminal(std::uint64_t generation, Terminal& out) const;

    // Process-exit path: request stop of the current generation and wait bounded
    // for teardown, so no ownerless native thread is left behind.
    void ShutdownForProcessExit(std::uint64_t deadline_ns);

#ifdef SESSION_TEST_HOOKS
    SessionTestGate& TestGate() { return gate_; }
    // Forces the next owner-thread spawn to fail, to exercise the roll-back path.
    void SetOwnerSpawnFailureForTest(bool enable) { fail_owner_spawn_ = enable; }
    // Shrinks the soft teardown-drain budget so a test can deterministically drive
    // the owner past it (review 2.3) without a multi-second wait.
    void SetTeardownDeadlineForTest(std::uint64_t ns) { teardown_deadline_ns_ = ns; }
#endif

private:
    void OwnerBody(std::uint64_t generation, SessionParams params);
    // Owner-only teardown: close admission for new control leases, drain the
    // in-flight leases (patiently -- a slow Stop must not strand the runtime),
    // Destroy exactly once, then set the terminal. Every owner completion path
    // (early cancel, natural return, backend exception) funnels through here so
    // Destroy never races a control call and the owner never leaves resources
    // unreclaimed.
    void TeardownAndDestroy(std::uint64_t generation,
                            const std::shared_ptr<SessionRuntime>& runtime,
                            const Terminal& terminal, Phase final_phase);
    void FinalizeAndJoin();  // the single join site; requires no lock held
    // Writes the terminal record exactly once for `generation`. Requires lock.
    void SetTerminalLocked(std::uint64_t generation, const Terminal& terminal, Phase phase);

    ISessionBackend& backend_;

    mutable std::mutex mtx_;
    std::condition_variable cv_;

    std::thread owner_;
    bool joined_{true};  // owner_ is not joinable at construction

    std::uint64_t generation_{0};
    Phase phase_{Phase::Idle};
    // Highest non-terminal phase this generation actually passed through
    // (Preparing..Stopping). WaitPhase compares against this, not phase_, so a
    // Prepare failure that jumps Preparing->Failed is never mistaken for having
    // reached Running -- the "never fabricate Running" rule.
    Phase high_water_phase_{Phase::Idle};

    std::shared_ptr<SessionRuntime> runtime_;  // published by owner after Prepare
    int in_flight_control_{0};                 // control-lease count
    bool tearing_down_{false};                 // closes admission for new leases
    bool drain_timed_out_{false};              // soft drain budget elapsed; owner still reclaiming
    std::uint64_t cancel_generation_{0};       // persistent cancel intent

    bool has_terminal_{false};
    std::uint64_t terminal_generation_{0};
    Terminal terminal_{};
    // Highest generation that has ever reached a terminal, retained across the
    // next Start (which clears has_terminal_). Lets a late Stop for an old,
    // already-finished generation return AlreadyStopped instead of being
    // indistinguishable from a generation that never existed.
    std::uint64_t last_terminated_generation_{0};

    std::uint64_t teardown_deadline_ns_{2'000'000'000ull};  // 2s > 1s stop budget

#ifdef SESSION_TEST_HOOKS
    SessionTestGate gate_;
    bool fail_owner_spawn_{false};
#endif
};

}  // namespace Core::HostRuntime
