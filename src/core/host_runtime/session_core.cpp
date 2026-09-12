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
        // FinalizeAndJoin dropped the lock. Another Start could have run to
        // completion in that window (spawned a new owner, advanced generation_).
        // Re-check the invariant against the CURRENT state rather than trusting
        // what we read before unlocking (review 4.1: recheck generation/phase
        // after unlocking join). If a session is now live, this caller loses the
        // race and returns AlreadyRunning.
        if (phase_ != Phase::Idle && !IsTerminal(phase_)) {
            return 0;  // AlreadyRunning: lost the race to a concurrent Start
        }
    }

    // Compute the new generation AFTER any unlock, so two racing Starts cannot
    // mint the same generation value.
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
    drain_timed_out_ = false;
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
    // The owner thread is the ONLY thread that runs the backend Prepare/Run/
    // Destroy calls and the ONLY thread that writes the terminal. Every completion
    // path -- Prepare failure, early cancel during Preparing, a natural Run return,
    // and any exception thrown out of a backend call -- funnels through the same
    // teardown sequence (TeardownAndDestroyLocked): close admission for new
    // control leases, drain the in-flight leases, Destroy exactly once, then set
    // the terminal. This is why an early cancel can no longer skip the drain
    // (review 2.1), a backend exception can no longer abort the process (2.2), and
    // a drain that is slow can no longer strand the runtime (2.3).

    // --- Prepare -----------------------------------------------------------
    std::shared_ptr<SessionRuntime> runtime;
    try {
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
        runtime = std::move(prepared).Value();
    } catch (const std::exception& e) {
        // Prepare threw (e.g. make_shared/vector allocation). The JNI try/catch
        // is on the calling thread and cannot catch this owner-thread exception,
        // so it MUST be handled here or std::terminate aborts the process.
        Terminal t;
        t.outcome = RunOutcome::StartFailed;
        t.error_category = static_cast<std::uint32_t>(Core::GuestCpu::ErrorCategory::BackendFailure);
        t.detail = std::string("Prepare threw: ") + e.what();
        t.exited = true;
        std::lock_guard lock{mtx_};
        SetTerminalLocked(generation, t, Phase::Failed);
        return;
    } catch (...) {
        Terminal t;
        t.outcome = RunOutcome::StartFailed;
        t.error_category = static_cast<std::uint32_t>(Core::GuestCpu::ErrorCategory::BackendFailure);
        t.detail = "Prepare threw a non-std exception";
        t.exited = true;
        std::lock_guard lock{mtx_};
        SetTerminalLocked(generation, t, Phase::Failed);
        return;
    }

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
        // Stop arrived during Preparing. Skip Run entirely, but tear down through
        // the SAME drain-then-Destroy path as a normal exit: a Stop that arrived
        // during Preparing may already be racing toward a control lease (a second
        // Stop after the runtime is published takes ++in_flight_control_), so
        // Destroying here without closing admission and draining would let that
        // control call dereference a runtime the owner is destroying (review 2.1).
        Terminal t;
        t.outcome = RunOutcome::Cancelled;
        t.user_requested_stop = true;
        t.exited = true;
        TeardownAndDestroy(generation, runtime, t, Phase::Stopped);
        return;
    }

    // --- Run ---------------------------------------------------------------
    {
        std::lock_guard lock{mtx_};
        phase_ = Phase::Running;
        high_water_phase_ = Phase::Running;
        cv_.notify_all();
    }

    RunReport report;
    try {
        report = backend_.Run(*runtime);
    } catch (const std::exception& e) {
        report.outcome = RunOutcome::BackendFailed;
        report.error_category =
            static_cast<std::uint32_t>(Core::GuestCpu::ErrorCategory::BackendFailure);
        report.detail = std::string("Run threw: ") + e.what();
    } catch (...) {
        report.outcome = RunOutcome::BackendFailed;
        report.error_category =
            static_cast<std::uint32_t>(Core::GuestCpu::ErrorCategory::BackendFailure);
        report.detail = "Run threw a non-std exception";
    }

#ifdef SESSION_TEST_HOOKS
    gate_.Wait(Checkpoint::OwnerAfterRunBeforeDestroy, generation);
#endif

    Terminal t;
    t.error_category = report.error_category;
    t.detail = std::move(report.detail);
    Phase final_phase;
    switch (report.outcome) {
    case RunOutcome::Returned:
        t.outcome = RunOutcome::Returned;
        t.exited = true;
        final_phase = Phase::Stopped;
        break;
    case RunOutcome::Cancelled:
        t.outcome = RunOutcome::Cancelled;
        t.user_requested_stop = true;
        t.exited = true;
        final_phase = Phase::Stopped;
        break;
    case RunOutcome::Faulted:
    case RunOutcome::BackendFailed:
    case RunOutcome::Unsupported:
    case RunOutcome::Unexpected:
    case RunOutcome::StartFailed:
        t.outcome = report.outcome;
        t.exited = true;
        final_phase = Phase::Failed;
        break;
    }

    TeardownAndDestroy(generation, runtime, t, final_phase);
}

void SessionCore::TeardownAndDestroy(std::uint64_t generation,
                                     const std::shared_ptr<SessionRuntime>& runtime,
                                     const Terminal& terminal, Phase final_phase) {
    // Owner-only. Closes admission for new control leases, then waits for the
    // in-flight leases to drain. The wait is patient: a slow Stop must not make
    // the owner abandon the runtime while a control call is still inside a backend
    // method (review 2.3). We publish an observable "drain is taking longer than
    // the soft budget" note without latching a permanent terminal, then keep
    // waiting until the lease actually reaches zero. Because tearing_down_ closes
    // admission, in_flight_control_ is monotone non-increasing here, so this
    // terminates once every lease returns.
    {
        std::unique_lock lock{mtx_};
        phase_ = Phase::Stopping;
        // Do NOT advance high_water_phase_ here. high_water_phase_ tracks the
        // highest EXECUTION phase (Preparing/Ready/Running) the session actually
        // reached, so WaitPhase(Running) reports TerminatedBeforeTarget for an
        // early cancel that never entered Run. Stopping is teardown, numerically
        // above Running; bumping it would fabricate a "reached Running" signal for
        // a session that skipped Run (the "never fabricate Running" rule).
        tearing_down_ = true;  // RequestStop refuses NEW leases from here on
        cv_.notify_all();

        const auto soft_deadline = std::chrono::steady_clock::now() +
                                   std::chrono::nanoseconds(teardown_deadline_ns_);
        if (!cv_.wait_until(lock, soft_deadline, [&] { return in_flight_control_ == 0; })) {
            // Soft budget elapsed with a lease still held. This is a NOTIFICATION,
            // not a terminal: the session is still owned and the owner stays to
            // finish reclaim. drain_timed_out_ lets an observer see "stop is taking
            // longer than expected" via the terminal note without the owner
            // leaving (review 2.3: no ownerless Stopping, no permanent latch).
            drain_timed_out_ = true;
            cv_.notify_all();
            // Keep waiting until the lease truly drains. Admission is closed, so
            // this cannot grow again; a control call inside the backend will
            // return and decrement.
            cv_.wait(lock, [&] { return in_flight_control_ == 0; });
        }
    }

    // Lease drained: the owner holds the last reference. Destroy is owner-only and
    // runs with no concurrent dereference -- exactly why the drain barrier exists.
    // Destroy must not throw the owner out of teardown either.
    try {
        backend_.Destroy(*runtime);
    } catch (const std::exception& e) {
        std::lock_guard lock{mtx_};
        Terminal failed = terminal;
        failed.outcome = RunOutcome::BackendFailed;
        failed.error_category =
            static_cast<std::uint32_t>(Core::GuestCpu::ErrorCategory::BackendFailure);
        failed.detail = std::string("Destroy threw: ") + e.what();
        failed.exited = true;
        runtime_.reset();
        SetTerminalLocked(generation, failed, Phase::Failed);
        return;
    } catch (...) {
        std::lock_guard lock{mtx_};
        Terminal failed = terminal;
        failed.outcome = RunOutcome::BackendFailed;
        failed.error_category =
            static_cast<std::uint32_t>(Core::GuestCpu::ErrorCategory::BackendFailure);
        failed.detail = "Destroy threw a non-std exception";
        failed.exited = true;
        runtime_.reset();
        SetTerminalLocked(generation, failed, Phase::Failed);
        return;
    }

    {
        std::lock_guard lock{mtx_};
        runtime_.reset();  // owner drops the last ref -> deterministic destruction
        Terminal done = terminal;
        done.exited = true;  // the session did reach a real terminal
        SetTerminalLocked(generation, done, final_phase);
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
    // The lease MUST be returned even if a backend call throws: RequestCancel /
    // WaitStopped on a real backend allocate and can throw, and if the lease is
    // not decremented the owner's teardown drain never completes (review 2.2).
    // This RAII guard decrements in_flight_control_ and notifies on every exit
    // path, including an exception propagating out of this function.
    struct LeaseGuard {
        SessionCore* self;
        ~LeaseGuard() {
            std::lock_guard lock{self->mtx_};
            --self->in_flight_control_;
            self->cv_.notify_all();
        }
    } lease_guard{this};

    StopResult result;
#ifdef SESSION_TEST_HOOKS
    gate_.Wait(Checkpoint::StopAfterLeaseBeforeInterrupt, generation);
#endif
    // A backend RequestCancel/WaitStopped can throw (they allocate on a real
    // backend). An exception must not escape RequestStop: the caller (JNI/Service)
    // gets a StopResult, and the lease is already returned by LeaseGuard above, so
    // the owner's drain still completes (review 2.2). Convert any exception to
    // Error rather than unwinding through the caller.
    try {
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
    } catch (...) {
        result = StopResult::Error;
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
