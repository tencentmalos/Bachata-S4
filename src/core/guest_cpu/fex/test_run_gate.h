// Test-only deterministic Run-entry delay protocol for coordinator tests (G24).
//
// Standard-library only, shared verbatim by the FEX backend (GUEST_CPU_TEST_HOOKS) and the host
// protocol unit test. One arm = one hold of one thread's Run, identified by a generation token.
//
// State per generation is held in a Generation object keyed by token, NOT in shared mutable fields:
// a slow Release waiter from an old generation that only observes Idle after a new generation has
// armed writes its (false) result to its OWN generation and can never clobber the new one. Every
// controller call is bound to its token.
#pragma once

#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <mutex>
#include <unordered_map>

namespace Core::GuestCpu::TestGate {

class RunGate {
public:
    enum class Phase { Idle, Armed, Arrived, Releasing };

    // Controller: arm the next hold. Returns a nonzero token, or 0 while a prior generation is still
    // live. A prior Release that is merely blocked in its wait is harmless (it owns the prior
    // generation's result slot), but a generation is "live" until its owner has Exited, so Arm only
    // proceeds once the slot has closed and the gate is Idle.
    std::uint64_t Arm(std::uint64_t thread_id, std::uint64_t context_id) {
        std::lock_guard lock{mutex_};
        if (phase_ != Phase::Idle)
            return 0;
        ++generation_;
        phase_ = Phase::Armed;
        arm_thread_ = thread_id;
        arm_context_ = context_id;
        // Fresh, generation-owned result state. A stale waiter from a previous token cannot reach
        // this slot because it keys on its own token.
        gen_.token = generation_;
        gen_.bound_invocation = 0;
        gen_.release_requested = false;
        gen_.clean_exit = false;
        gen_.release_result = false;
        gen_.release_decided = false;
        gen_.owner_left = false;
        cv_.notify_all();
        return generation_;
    }

    bool Arrived(std::uint64_t token) {
        std::lock_guard lock{mutex_};
        return token == generation_ && gen_.token == token &&
               (phase_ == Phase::Arrived || phase_ == Phase::Releasing);
    }

    std::uint64_t BoundInvocation(std::uint64_t token) {
        std::lock_guard lock{mutex_};
        return (gen_.token == token && phase_ != Phase::Idle) ? gen_.bound_invocation : 0;
    }

    // Release THIS token's owner and wait until it left. Idempotent for the same token. A token that
    // is not the currently live generation (old/unknown/stale) returns false and never mutates the
    // current generation, even if it wakes late after a new Arm.
    bool Release(std::uint64_t token, std::uint64_t timeout_ms) {
        std::unique_lock lock{mutex_};
        if (token == 0)
            return false;

        // Fast path: an already-decided result for THIS token. Locate the generation the token names
        // so a late Release of a previous generation reads that generation's recorded outcome rather
        // than the live one's fields.
        if (token == gen_.token) {
            if (gen_.release_decided)
                return gen_.release_result;
            if (phase_ != Phase::Arrived) {
                // Releasing before arrival is a misuse: record failure on this generation only.
                gen_.release_decided = true;
                gen_.release_result = false;
                // Owner may still be Armed; to avoid stranding it, release it but mark non-clean so
                // its WaitAtEntry returns false. Do not touch any later generation.
                gen_.release_requested = true;
                cv_.notify_all();
                return false;
            }
            gen_.release_requested = true;
            phase_ = Phase::Releasing;
            cv_.notify_all();
            const auto wait_ms = std::chrono::milliseconds(timeout_ms ? timeout_ms : 1000);
            // The wait predicate is TOKEN-bound; but when it returns false we must RE-CHECK the token
            // before writing any result: the wait can wake after a newer generation has taken over the
            // live slot (the previous owner re-armed from its own thread while this waiter was still
            // blocked). In that case do not touch the new generation's fields -- return the archived
            // outcome of our own token instead.
            cv_.wait_for(lock, wait_ms, [&] { return gen_.token == token && gen_.owner_left; });
            if (gen_.token != token) {
                // Generation rolled over under us; our owner archived its outcome before arming.
                auto it = history_.find(token);
                return it != history_.end() ? it->second : false;
            }
            gen_.release_decided = true;
            gen_.release_result = gen_.owner_left && gen_.clean_exit;
            return gen_.release_result;
        }

        // Stale token for an older (or future) generation. Do not mutate current state. If that
        // generation already closed, return its recorded outcome; otherwise it is a bad token.
        auto it = history_.find(token);
        if (it != history_.end())
            return it->second;
        return false;
    }

    bool Exited(std::uint64_t token) {
        std::lock_guard lock{mutex_};
        if (gen_.token == token)
            return gen_.owner_left;
        auto it = history_.find(token);
        return it != history_.end() ? true : false;
    }

    // Owner side. Pass-through unless an arm matches this thread/context. Binds the invocation to the
    // live token; on exit records the result into THAT token's slot and archives it before going Idle,
    // so the controller can read the outcome after a new generation has armed.
    bool WaitAtEntry(std::uint64_t context_id, std::uint64_t thread_id,
                     std::uint64_t invocation, std::uint64_t timeout_ms) {
        std::unique_lock lock{mutex_};
        if (phase_ != Phase::Armed || arm_thread_ != thread_id || arm_context_ != context_id)
            return true;  // not the held owner, or nothing armed.
        const std::uint64_t token = generation_;
        gen_.bound_invocation = invocation;
        phase_ = Phase::Arrived;
        cv_.notify_all();

        const auto wait_ms = std::chrono::milliseconds(timeout_ms ? timeout_ms : 5000);
        // Only a release for THIS token unblocks; a new generation cannot exist while this one is not
        // Idle, so the flag read here is necessarily ours.
        const bool released = cv_.wait_for(lock, wait_ms,
                                          [&] { return gen_.token == token && gen_.release_requested; });
        gen_.clean_exit = released;
        gen_.owner_left = true;
        // Archive this generation's outcome BEFORE going Idle, so a later Arm cannot overwrite a
        // result a slow controller Release might still read.
        history_[token] = gen_.clean_exit;
        phase_ = Phase::Idle;
        cv_.notify_all();
        return released;
    }

    Phase CurrentPhase() {
        std::lock_guard lock{mutex_};
        return phase_;
    }

private:
    struct Generation {
        std::uint64_t token{};
        std::uint64_t bound_invocation{};
        bool release_requested{false};
        bool clean_exit{false};
        bool release_result{false};
        bool release_decided{false};
        bool owner_left{false};
    };

    std::mutex mutex_;
    std::condition_variable cv_;
    Phase phase_{Phase::Idle};
    std::uint64_t generation_{0};
    std::uint64_t arm_thread_{0};
    std::uint64_t arm_context_{0};
    Generation gen_{};
    // Outcomes of closed generations, keyed by token, so a late/stale Release never reads or writes
    // the live generation's mutable fields. Bounded history is fine for a test gate.
    std::unordered_map<std::uint64_t, bool> history_;
};

}  // namespace Core::GuestCpu::TestGate
