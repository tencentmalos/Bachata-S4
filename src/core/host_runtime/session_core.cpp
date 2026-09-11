// SPDX-FileCopyrightText: Copyright 2026 shadPS4 Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#include "core/host_runtime/session_core.h"

#include <chrono>
#include <utility>

namespace Core::HostRuntime {

namespace {

std::uint64_t NowNs() {
    return static_cast<std::uint64_t>(
        std::chrono::duration_cast<std::chrono::nanoseconds>(
            std::chrono::steady_clock::now().time_since_epoch())
            .count());
}

}  // namespace

const char* ToString(Phase phase) noexcept {
    switch (phase) {
    case Phase::Idle: return "Idle";
    case Phase::Preparing: return "Preparing";
    case Phase::Ready: return "Ready";
    case Phase::Running: return "Running";
    case Phase::Stopping: return "Stopping";
    case Phase::Stopped: return "Stopped";
    case Phase::Failed: return "Failed";
    }
    return "Unknown";
}

const char* ToString(StopResult result) noexcept {
    switch (result) {
    case StopResult::Accepted: return "Accepted";
    case StopResult::CancelPending: return "CancelPending";
    case StopResult::AlreadyStopping: return "AlreadyStopping";
    case StopResult::AlreadyStopped: return "AlreadyStopped";
    case StopResult::WrongGeneration: return "WrongGeneration";
    case StopResult::Timeout: return "Timeout";
    case StopResult::Error: return "Error";
    }
    return "Unknown";
}

SessionCore::SessionCore(ISessionBackend& backend) : backend_(backend) {}

SessionCore::~SessionCore() {
    // Best-effort teardown so a destroyed core never leaves an owner thread
    // running. ShutdownForProcessExit is the deliberate path; this is the safety
    // net if a caller forgets it.
    ShutdownForProcessExit(teardown_deadline_ns_);
    FinalizeAndJoin();
}

std::uint64_t SessionCore::Start(const SessionParams& params) {
    std::unique_lock lock{mtx_};

    // One live session. A previous generation that reached a terminal but whose
    // owner thread has not been joined yet is finalized here before we spawn a
    // new one (the single join site is FinalizeAndJoin, called without the lock).
    if (phase_ != Phase::Idle && !IsTerminal(phase_)) {
        return 0;  // AlreadyRunning
    }
    if (!joined_) {
        lock.unlock();
        FinalizeAndJoin();
        lock.lock();
    }

    const std::uint64_t gen = generation_ + 1;

#ifdef SESSION_TEST_HOOKS
    if (fail_owner_spawn_) {
        // Roll back cleanly: do not leak a generation or leave a non-Idle phase.
        phase_ = Phase::Idle;
        high_water_phase_ = Phase::Idle;
        return 0;
    }
#endif

    generation_ = gen;
    phase_ = Phase::Preparing;
    high_water_phase_ = Phase::Preparing;
    runtime_.reset();
    in_flight_control_ = 0;
    tearing_down_ = false;
    cancel_generation_ = 0;
    has_terminal_ = false;

    SessionParams owned = params;
    try {
        owner_ = std::thread(&SessionCore::OwnerBody, this, gen, std::move(owned));
        joined_ = false;
    } catch (...) {
        // Thread creation failed: roll back to Idle, no generation leak visible
        // to callers (generation_ moved forward but phase is Idle and no owner).
        phase_ = Phase::Idle;
        high_water_phase_ = Phase::Idle;
        runtime_.reset();
        return 0;
    }

    cv_.notify_all();
    return gen;
}

void SessionCore::OwnerBody(std::uint64_t generation, SessionParams params) {
    // --- Prepare -----------------------------------------------------------
    auto prepared = backend_.Prepare(params);
    if (!prepared) {
        Terminal t;
        t.outcome = RunOutcome::StartFailed;
        t.error_category = static_cast<std::uint32_t>(prepared.GetError().category);
        t.detail = Core::GuestCpu::Describe(prepared.GetError());
        t.exited = true;
        std::lock_guard lock{mtx_};
        SetTerminalLocked(generation, t, Phase::Failed);
        return;
    }
    std::shared_ptr<SessionRuntime> runtime = std::move(prepared).Value();

#ifdef SESSION_TEST_HOOKS
    gate_.Wait(Checkpoint::BeforePublish, generation);
#endif

    // --- Publish runtime, become Ready, honor any preparing-phase cancel ---
    bool cancel_before_run = false;
    {
        std::lock_guard lock{mtx_};
        runtime_ = runtime;
        phase_ = Phase::Ready;
        high_water_phase_ = Phase::Ready;
        cancel_before_run = (cancel_generation_ == generation);
        cv_.notify_all();
    }

    if (cancel_before_run) {
        // Stop arrived during Preparing. Skip Run entirely; tear down as a clean
        // user cancel. Run was never entered, so there is nothing to interrupt.
        Terminal t;
        t.outcome = RunOutcome::Cancelled;
        t.user_requested_stop = true;
        t.exited = true;
        backend_.Destroy(*runtime);
        std::lock_guard lock{mtx_};
        runtime_.reset();
        SetTerminalLocked(generation, t, Phase::Stopped);
        return;
    }

    // --- Run ---------------------------------------------------------------
    {
        std::lock_guard lock{mtx_};
        phase_ = Phase::Running;
        high_water_phase_ = Phase::Running;
        cv_.notify_all();
    }

    RunReport report = backend_.Run(*runtime);

#ifdef SESSION_TEST_HOOKS
    gate_.Wait(Checkpoint::OwnerAfterRunBeforeDestroy, generation);
#endif

    // --- Teardown: drain the control-lease, then Destroy -------------------
    bool drained;
    {
        std::unique_lock lock{mtx_};
        phase_ = Phase::Stopping;
        high_water_phase_ = Phase::Stopping;
        tearing_down_ = true;  // step below refuses NEW leases
        cv_.notify_all();
        // Bounded wait; releases the lock while blocked so a stopper can
        // decrement in_flight_control_ and notify.
        const auto deadline = std::chrono::steady_clock::now() +
                              std::chrono::nanoseconds(teardown_deadline_ns_);
        drained = cv_.wait_until(lock, deadline, [&] { return in_flight_control_ == 0; });
    }

    Terminal t;
    t.error_category = report.error_category;
    t.detail = std::move(report.detail);
    switch (report.outcome) {
    case RunOutcome::Returned:
        t.outcome = RunOutcome::Returned;
        t.exited = true;
        break;
    case RunOutcome::Cancelled:
        t.outcome = RunOutcome::Cancelled;
        t.user_requested_stop = true;
        t.exited = true;
        break;
    case RunOutcome::Faulted:
    case RunOutcome::BackendFailed:
    case RunOutcome::Unsupported:
    case RunOutcome::Unexpected:
    case RunOutcome::StartFailed:
        t.outcome = report.outcome;
        t.exited = true;
        break;
    }

    if (!drained) {
        // A control call did not return within the teardown budget. Do NOT
        // Destroy and do NOT drop the runtime: the shared_ptr keeps everything
        // alive and owned. Report "not exited" and keep ownership; never Idle.
        Terminal timed_out;
        timed_out.outcome = RunOutcome::BackendFailed;
        timed_out.error_category =
            static_cast<std::uint32_t>(Core::GuestCpu::ErrorCategory::Timeout);
        timed_out.detail = "teardown drain timed out; session not exited";
        timed_out.exited = false;
        std::lock_guard lock{mtx_};
        SetTerminalLocked(generation, timed_out, Phase::Stopping);
        return;
    }

    // Lease drained: the owner holds the last reference. DestroyThread is
    // owner-only; running it here with no concurrent dereference is exactly why
    // the drain barrier exists.
    backend_.Destroy(*runtime);
    {
        std::lock_guard lock{mtx_};
        runtime_.reset();  // owner drops the last ref -> deterministic destruction
        SetTerminalLocked(generation, t,
                          t.outcome == RunOutcome::Returned || t.outcome == RunOutcome::Cancelled
                              ? Phase::Stopped
                              : Phase::Failed);
    }
}

StopResult SessionCore::RequestStop(std::uint64_t generation, std::uint64_t timeout_ns) {
    std::shared_ptr<SessionRuntime> rt;
    {
        std::unique_lock lock{mtx_};
        if (generation != generation_) {
            // Idempotent terminal read for a past generation. A generation at or
            // below the last one that finished is treated as already stopped.
            if (generation <= last_terminated_generation_)
                return StopResult::AlreadyStopped;
            return StopResult::WrongGeneration;
        }
        if (IsTerminal(phase_)) {
            return StopResult::AlreadyStopped;
        }
        if (phase_ == Phase::Preparing && runtime_ == nullptr) {
            // Context not yet published: record intent, dereference nothing.
            cancel_generation_ = generation;
            return StopResult::CancelPending;
        }
        if (tearing_down_) {
            return StopResult::AlreadyStopping;
        }
        cancel_generation_ = generation;
        rt = runtime_;  // shared_ptr copy: keeps the runtime alive across the calls
        if (rt == nullptr) {
            // Ready/Running but runtime somehow null: treat as pending, owner
            // will observe cancel_generation_.
            return StopResult::CancelPending;
        }
        ++in_flight_control_;  // take the lease
    }

    // Lease held (shared_ptr copy also held). No lock across the blocking calls.
    StopResult result;
#ifdef SESSION_TEST_HOOKS
    gate_.Wait(Checkpoint::StopAfterLeaseBeforeInterrupt, generation);
#endif
    auto ticket = backend_.RequestCancel(*rt);
    if (!ticket) {
        result = StopResult::Error;
    } else {
        const std::uint64_t budget = timeout_ns != 0 ? timeout_ns : 1'000'000'000ull;
        auto stopped = backend_.WaitStopped(*rt, ticket.Value(), budget);
        if (stopped)
            result = StopResult::Accepted;
        else if (stopped.Category() == Core::GuestCpu::ErrorCategory::Timeout)
            result = StopResult::Timeout;
        else
            result = StopResult::Error;
    }

    {
        std::lock_guard lock{mtx_};
        --in_flight_control_;
        cv_.notify_all();
    }
    return result;
}

Phase SessionCore::QueryPhase(std::uint64_t generation) const {
    std::lock_guard lock{mtx_};
    if (generation != generation_) {
        if (has_terminal_ && terminal_generation_ == generation)
            return terminal_.outcome == RunOutcome::Returned ||
                           terminal_.outcome == RunOutcome::Cancelled
                       ? Phase::Stopped
                       : Phase::Failed;
        return Phase::Idle;
    }
    return phase_;
}

std::uint64_t SessionCore::CurrentGeneration() const {
    std::lock_guard lock{mtx_};
    // A generation is "current" only while a session is live. After a terminal,
    // report 0 so a freshly restarted process cannot resurrect a fake Running.
    if (phase_ == Phase::Idle || IsTerminal(phase_))
        return 0;
    return generation_;
}

WaitPhaseResult SessionCore::WaitPhase(std::uint64_t generation, Phase target,
                                       std::uint64_t deadline_ns) {
    std::unique_lock lock{mtx_};
    auto predicate = [&] {
        if (generation != generation_)
            return true;  // superseded
        return high_water_phase_ >= target || IsTerminal(phase_);
    };
    if (deadline_ns == 0) {
        cv_.wait(lock, predicate);
    } else {
        const auto deadline =
            std::chrono::steady_clock::now() + std::chrono::nanoseconds(deadline_ns);
        if (!cv_.wait_until(lock, deadline, predicate))
            return WaitPhaseResult::Timeout;
    }
    if (generation != generation_)
        return WaitPhaseResult::WrongGeneration;
    // Reached the target phase (possibly already terminal afterwards) iff the
    // high-water mark actually passed through it. A Prepare failure that jumped
    // Preparing->Failed has high_water_phase_ == Preparing, so a wait for Running
    // correctly reports TerminatedBeforeTarget rather than fabricating Running.
    if (high_water_phase_ >= target)
        return WaitPhaseResult::ReachedTarget;
    return WaitPhaseResult::TerminatedBeforeTarget;
}

bool SessionCore::WaitTerminal(std::uint64_t generation, std::uint64_t deadline_ns, Terminal& out) {
    {
        std::unique_lock lock{mtx_};
        auto predicate = [&] {
            return (has_terminal_ && terminal_generation_ == generation) ||
                   (generation != generation_ && generation < generation_);
        };
        if (deadline_ns == 0) {
            cv_.wait(lock, predicate);
        } else {
            const auto deadline =
                std::chrono::steady_clock::now() + std::chrono::nanoseconds(deadline_ns);
            if (!cv_.wait_until(lock, deadline, predicate)) {
                // Timeout: keep ownership, never declare Idle.
                return false;
            }
        }
        if (!(has_terminal_ && terminal_generation_ == generation))
            return false;
        out = terminal_;
    }
    // Terminal reached: join the owner thread (the single join site).
    FinalizeAndJoin();
    return true;
}

bool SessionCore::TryGetTerminal(std::uint64_t generation, Terminal& out) const {
    std::lock_guard lock{mtx_};
    if (has_terminal_ && terminal_generation_ == generation) {
        out = terminal_;
        return true;
    }
    return false;
}

void SessionCore::ShutdownForProcessExit(std::uint64_t deadline_ns) {
    std::uint64_t gen;
    {
        std::lock_guard lock{mtx_};
        if (phase_ == Phase::Idle || IsTerminal(phase_))
            return;
        gen = generation_;
    }
    (void)RequestStop(gen, deadline_ns);
    Terminal ignored;
    (void)WaitTerminal(gen, deadline_ns, ignored);
}

void SessionCore::FinalizeAndJoin() {
    std::thread to_join;
    {
        std::lock_guard lock{mtx_};
        if (joined_)
            return;
        // Only join once a terminal is reached; a caller must not join a live
        // owner. WaitTerminal guarantees this, and ShutdownForProcessExit waits
        // for the terminal before calling here.
        if (!has_terminal_)
            return;
        to_join = std::move(owner_);
        joined_ = true;
    }
    if (to_join.joinable())
        to_join.join();
}

void SessionCore::SetTerminalLocked(std::uint64_t generation, const Terminal& terminal,
                                    Phase phase) {
    if (has_terminal_ && terminal_generation_ == generation)
        return;  // terminal-once
    has_terminal_ = true;
    terminal_generation_ = generation;
    terminal_ = terminal;
    if (generation > last_terminated_generation_)
        last_terminated_generation_ = generation;
    phase_ = phase;
    cv_.notify_all();
}

}  // namespace Core::HostRuntime
