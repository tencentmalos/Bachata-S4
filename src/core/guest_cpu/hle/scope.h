// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once

#include <chrono>
#include <condition_variable>
#include <stdexcept>
#include <stop_token>
#include <thread>
#include "core/guest_cpu/api/context.h"

namespace Core::GuestCpu::Hle {

// A host HLE invocation entered only after its JIT dispatcher frame returned.
// Stack scoped, owner-thread only; the backend validates its live frame as well
// as this scope before allowing callbacks or a self-excluding VM transaction.
class HleScope final {
public:
    HleScope(CpuContext& context, ThreadHandle thread, std::uint64_t invocation,
             std::stop_token cancel);
    ~HleScope();
    HleScope(const HleScope&) = delete;
    HleScope& operator=(const HleScope&) = delete;

    static HleScope* Current() noexcept;
    CpuContext& Context() const noexcept {
        return context_;
    }
    ThreadHandle Thread() const noexcept {
        return thread_;
    }
    std::uint64_t InvocationId() const noexcept {
        return invocation_;
    }
    unsigned Depth() const noexcept {
        return depth_;
    }
    std::stop_token CancellationToken() const noexcept {
        return cancel_;
    }
    bool CallbackAdmission() const noexcept {
        return allow_callback_;
    }
    const std::optional<GuestCallResult>& FailedCallback() const noexcept {
        return failed_callback_;
    }
    // pthread_exit terminates every nested callback on this guest owner after
    // ordinary C++ cleanup. It never longjmps over host HLE frames.
    void ExitThread(std::uint64_t value);
    std::optional<std::uint64_t> ThreadExitResult() const noexcept {
        return thread_exit_;
    }

    // Native wait primitives should use CancellationToken. This bounded wait is
    // useful for timed HLE operations; false means cancellation interrupted it.
    bool WaitFor(std::chrono::nanoseconds duration) const;
    // Preserve the original wait/deadline and its queue position while an OS
    // callback runs. Callers must release every other lock/pin before waiting.
    template <class Lock, class Clock, class Duration, class Predicate>
    static bool WaitUntil(std::condition_variable_any& changed, Lock& lock,
                          std::stop_token cancel,
                          const std::chrono::time_point<Clock, Duration>& deadline,
                          Predicate ready) {
        auto* scope = Current();
        if (!scope)
            return changed.wait_until(lock, cancel, deadline, ready);
        while (!ready() && !cancel.stop_requested()) {
            const auto pending = scope->context_.OwnerCallbackToken(scope->thread_);
            std::stop_source wake;
            std::stop_callback cancelled(cancel, [&] { wake.request_stop(); });
            std::stop_callback queued(pending, [&] { wake.request_stop(); });
            changed.wait_until(lock, wake.get_token(), deadline, ready);
            if (cancel.stop_requested()) break;
            if (pending.stop_requested()) {
                auto status = [&] {
                    lock.unlock();
                    struct Relock { Lock& lock; ~Relock() { lock.lock(); } } relock{lock};
                    return scope->context_.PollOwnerCallbacks(scope->thread_);
                }();
                if (!status) throw std::runtime_error(Describe(status.GetError()));
            }
            if (Clock::now() >= deadline) break;
        }
        return ready();
    }
    Result<GuestCallResult> InvokeGuest(GuestCodeAddress entry, const GuestCallArgs& args,
                                        const GuestCallOptions& options = {});

private:
    CpuContext& context_;
    ThreadHandle thread_;
    std::uint64_t invocation_;
    std::stop_token cancel_;
    HleScope* previous_{};
    std::thread::id owner_;
    unsigned depth_{};
    bool allow_callback_{};
    std::optional<GuestCallResult> failed_callback_;
    std::optional<std::uint64_t> thread_exit_;
};
} // namespace Core::GuestCpu::Hle
