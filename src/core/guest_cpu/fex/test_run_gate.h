// Test-only deterministic Run-entry delay protocol for coordinator tests (G24).
//
// This header depends ONLY on the C++ standard library (no FEX types), so it is shared verbatim by:
//   - src/core/guest_cpu/fex/fex_context.cpp (the FEX backend, gated behind GUEST_CPU_TEST_HOOKS),
//   - the host unit test test_run_gate_protocol.py/test-runner-equivalent that verifies the state
//     machine without linking FEX.
// Keeping the protocol in one place means a fix here is exercised by both the on-device suite and a
// host determinism test; there is no copied-from-production implementation under test.
//
// Protocol: an owner thread, already marked running and holding its execution lease but after every
// coordinator/context/address-space lock was released and before entering FEX ExecuteThread, calls
// WaitAtEntry(). The controller Arms a hold and receives a generation token; Arrived waits for the
// matching owner, then releases it through the token. Every controller call names a token, so:
//   * Arm rejects a new generation until the previous one has fully Exited (a new arm cannot take
//     over an owner that has not left);
//   * Release is idempotent per token and a stale/unknown token is rejected without touching the
//     current generation, so one owner's exit cannot confirm two releases;
//   * an owner that times out still leaves but reports a TimedOut (clean=false) result and the gate
//     does not fake a clean exit.
// Release products never compile this header in (it is only included under GUEST_CPU_TEST_HOOKS in
// production; the host test is a test target).
#pragma once

#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <mutex>

namespace Core::GuestCpu::TestGate {

class RunGate {
public:
    enum class Phase { Idle, Armed, Arrived, Releasing };

    // Controller side: arm the next hold. Returns a nonzero token, or 0 while a prior generation is
    // still live (controller must wait for its Exited first).
    std::uint64_t Arm(std::uint64_t thread_id, std::uint64_t context_id) {
        std::lock_guard lock{mutex_};
        if (phase_ != Phase::Idle)
            return 0;
        ++generation_;
        phase_ = Phase::Armed;
        arm_thread_ = thread_id;
        arm_context_ = context_id;
        bound_to_token_ = 0;
        present_invocation_ = 0;
        release_requested_ = false;
        clean_exit_ = false;
        release_result_ = false;
        release_decided_ = false;
        cv_.notify_all();
        return generation_;
    }

    bool Arrived(std::uint64_t token) {
        std::lock_guard lock{mutex_};
        return token == generation_ && (phase_ == Phase::Arrived || phase_ == Phase::Releasing)
               && bound_to_token_ == token;
    }

    std::uint64_t BoundInvocation(std::uint64_t token) {
        std::lock_guard lock{mutex_};
        return (bound_to_token_ == token) ? present_invocation_ : 0;
    }

    bool Release(std::uint64_t token, std::uint64_t timeout_ms) {
        std::unique_lock lock{mutex_};
        if (token == 0 || token != generation_)
            return false;  // stale/unknown token: never touch the current generation.
        if (release_decided_)
            return release_result_;  // idempotent: one recorded result per token.
        // Releasing before the owner arrived is a misuse; fail rather than fake an exit.
        if (phase_ != Phase::Arrived) {
            release_decided_ = true;
            release_result_ = false;
            cv_.notify_all();
            return false;
        }
        release_requested_ = true;
        phase_ = Phase::Releasing;
        cv_.notify_all();
        const auto wait_ms = std::chrono::milliseconds(timeout_ms ? timeout_ms : 1000);
        const bool left = cv_.wait_for(lock, wait_ms,
                                      [&] { return phase_ == Phase::Idle && last_exited_token_ == token; });
        release_decided_ = true;
        release_result_ = left && clean_exit_;
        return release_result_;
    }

    bool Exited(std::uint64_t token) {
        std::lock_guard lock{mutex_};
        return last_exited_token_ == token;
    }

    // Owner side. Blocks only for an arm matching this thread/context; returns true on clean release,
    // false on timeout (Run must not hang). The matching token's exit always closes the generation.
    bool WaitAtEntry(std::uint64_t context_id, std::uint64_t thread_id,
                     std::uint64_t invocation, std::uint64_t timeout_ms) {
        std::unique_lock lock{mutex_};
        if (phase_ != Phase::Armed || arm_thread_ != thread_id || arm_context_ != context_id)
            return true;  // not the held owner, or nothing armed: pass straight through.
        bound_to_token_ = generation_;
        present_invocation_ = invocation;
        phase_ = Phase::Arrived;
        cv_.notify_all();

        const auto wait_ms = std::chrono::milliseconds(timeout_ms ? timeout_ms : 5000);
        const bool released = cv_.wait_for(lock, wait_ms, [&] { return release_requested_; });
        clean_exit_ = released;
        last_exited_token_ = generation_;
        phase_ = Phase::Idle;
        bound_to_token_ = 0;
        release_requested_ = false;
        cv_.notify_all();
        return released;
    }

    // Test observation of the current phase (no mutation).
    Phase CurrentPhase() {
        std::lock_guard lock{mutex_};
        return phase_;
    }

private:
    std::mutex mutex_;
    std::condition_variable cv_;
    Phase phase_{Phase::Idle};
    std::uint64_t generation_{0};
    std::uint64_t arm_thread_{0};
    std::uint64_t arm_context_{0};
    std::uint64_t bound_to_token_{0};
    std::uint64_t present_invocation_{0};
    std::uint64_t last_exited_token_{0};
    bool release_requested_{false};
    bool clean_exit_{false};
    bool release_result_{false};
    bool release_decided_{false};
};

}  // namespace Core::GuestCpu::TestGate
