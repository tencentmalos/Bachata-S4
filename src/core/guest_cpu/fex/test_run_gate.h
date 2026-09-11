// Test-only deterministic Run-entry delay protocol for coordinator tests (G24).
//
// Standard-library only, shared verbatim by the FEX backend (GUEST_CPU_TEST_HOOKS) and the host
// protocol unit test. One arm = one hold of one thread's Run, identified by a generation token.
//
// Three distinct terminal dispositions keep error/timeout from being reported as a clean release:
//   Released -- controller Released only after the owner Arrived; owner resumes cleanly.
//   Aborted  -- controller Released (or cancelled the generation) BEFORE the owner arrived; the
//               owner still leaves but its WaitAtEntry returns false (not a clean release).
//   TimedOut -- the owner's wait budget elapsed with no disposition yet; WaitAtEntry returns false
//               and Run must NOT be allowed to enter the guest.
// Every disposition is owned by its token and archived when the owner closes the generation, so a
// stale/late call or a new generation can never read or write another generation's outcome.
#pragma once

#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <mutex>
#include <unordered_map>

namespace Core::GuestCpu::TestGate {

enum class Disposition : int { None = 0, Released = 1, Aborted = 2, TimedOut = 3 };

class RunGate {
public:
    enum class Phase { Idle, Armed, Arrived, Releasing };

    // Controller: arm the next hold. Returns a nonzero token, or 0 while a prior generation is still
    // live (controller must wait for its Exited first).
    std::uint64_t Arm(std::uint64_t thread_id, std::uint64_t context_id) {
        std::lock_guard lock{mutex_};
        if (phase_ != Phase::Idle)
            return 0;
        ++generation_;
        phase_ = Phase::Armed;
        arm_thread_ = thread_id;
        arm_context_ = context_id;
        gen_ = Generation{};
        gen_.token = generation_;
        history_.erase(0);
        cv_.notify_all();
        return generation_;
    }

    bool Arrived(std::uint64_t token) {
        std::lock_guard lock{mutex_};
        return gen_.token == token &&
               (phase_ == Phase::Arrived || phase_ == Phase::Releasing);
    }

    std::uint64_t BoundInvocation(std::uint64_t token) {
        std::lock_guard lock{mutex_};
        return (gen_.token == token && phase_ != Phase::Idle) ? gen_.bound_invocation : 0;
    }

    // True iff THIS token had a clean Released disposition. Idempotent for the same token regardless
    // of when it is called (before/after a new Arm): the outcome is keyed by token, archived on
    // close, and never changes. Returns false for Aborted/TimedOut/unknown/stale tokens.
    bool Release(std::uint64_t token, std::uint64_t timeout_ms) {
        std::unique_lock lock{mutex_};
        if (token == 0)
            return false;
        if (token != gen_.token) {
            auto it = history_.find(token);
            return it != history_.end() && it->second == Disposition::Released;
        }
        // Live token. A disposition already set (Released or an early Abort) is immutable.
        if (gen_.disposition != Disposition::None) {
            if (gen_.disposition == Disposition::Released && gen_.owner_left)
                return true;
            // Released but owner still closing: wait it out. Aborted: report false immediately.
            if (gen_.disposition != Disposition::Released)
                return false;
        } else if (phase_ == Phase::Arrived) {
            // The legal release: owner is at the gate.
            gen_.disposition = Disposition::Released;
            phase_ = Phase::Releasing;
            cv_.notify_all();
        } else {
            // phase_ == Armed: release requested before the owner arrived = abort, never a clean
            // release. Mark Aborted so the owner, whenever it reaches the gate, leaves immediately
            // with WaitAtEntry=false. Do not set release_requested-as-clean.
            gen_.disposition = Disposition::Aborted;
            cv_.notify_all();
            return false;
        }
        // Released path: wait for THIS token's owner to leave.
        const auto wait_ms = std::chrono::milliseconds(timeout_ms ? timeout_ms : 1000);
        cv_.wait_for(lock, wait_ms,
                     [&] { return gen_.token == token && gen_.owner_left; });
        return gen_.token == token && gen_.owner_left &&
               gen_.disposition == Disposition::Released;
    }

    // Abort the current generation regardless of arrival (test helper for cancel/timeout flows).
    // Returns true if a live generation was signalled; the owner leaves with WaitAtEntry=false.
    bool Abort(std::uint64_t token) {
        std::lock_guard lock{mutex_};
        if (token == 0 || token != gen_.token)
            return false;
        if (gen_.disposition == Disposition::None) {
            gen_.disposition = Disposition::Aborted;
            cv_.notify_all();
        }
        return true;
    }

    bool Exited(std::uint64_t token) {
        std::lock_guard lock{mutex_};
        if (gen_.token == token)
            return gen_.owner_left;
        return history_.find(token) != history_.end();
    }

    // The terminal disposition for a token (after it closed), for assertion diagnostics.
    Disposition Outcome(std::uint64_t token) {
        std::lock_guard lock{mutex_};
        if (gen_.token == token && gen_.owner_left)
            return gen_.disposition;
        auto it = history_.find(token);
        return it == history_.end() ? Disposition::None : it->second;
    }

    // Owner side. Returns true ONLY on a clean Released; false on Abort or timeout (Run must then
    // refuse to enter the guest). Pass-through when not the held owner / nothing armed.
    bool WaitAtEntry(std::uint64_t context_id, std::uint64_t thread_id,
                     std::uint64_t invocation, std::uint64_t timeout_ms) {
        std::unique_lock lock{mutex_};
        if (phase_ != Phase::Armed || arm_thread_ != thread_id || arm_context_ != context_id)
            return true;  // not the held owner, or nothing armed: pass straight through.
        const std::uint64_t token = generation_;
        gen_.bound_invocation = invocation;
        phase_ = Phase::Arrived;
        cv_.notify_all();

        const auto wait_ms = std::chrono::milliseconds(timeout_ms ? timeout_ms : 5000);
        // Wake on any disposition set by the controller (Released/Aborted) or on timeout.
        const bool decided = cv_.wait_for(lock, wait_ms,
                                          [&] { return gen_.disposition != Disposition::None; });
        if (!decided && gen_.disposition == Disposition::None)
            gen_.disposition = Disposition::TimedOut;
        const bool clean = (gen_.disposition == Disposition::Released);
        gen_.owner_left = true;
        history_[token] = gen_.disposition;
        phase_ = Phase::Idle;
        cv_.notify_all();
        return clean;
    }

    Phase CurrentPhase() {
        std::lock_guard lock{mutex_};
        return phase_;
    }

private:
    struct Generation {
        std::uint64_t token{};
        std::uint64_t bound_invocation{};
        Disposition disposition{Disposition::None};
        bool owner_left{false};
    };

    std::mutex mutex_;
    std::condition_variable cv_;
    Phase phase_{Phase::Idle};
    std::uint64_t generation_{0};
    std::uint64_t arm_thread_{0};
    std::uint64_t arm_context_{0};
    Generation gen_{};
    std::unordered_map<std::uint64_t, Disposition> history_;
};

}  // namespace Core::GuestCpu::TestGate
