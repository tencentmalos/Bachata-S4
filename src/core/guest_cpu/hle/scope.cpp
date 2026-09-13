// SPDX-License-Identifier: GPL-2.0-or-later
#include <condition_variable>
#include <mutex>
#include "core/guest_cpu/hle/scope.h"

namespace Core::GuestCpu::Hle {
namespace {
thread_local HleScope* current{};
}
HleScope::HleScope(CpuContext& context, ThreadHandle thread, std::uint64_t invocation,
                   std::stop_token cancel)
    : context_(context), thread_(thread), invocation_(invocation), cancel_(cancel),
      previous_(current), owner_(std::this_thread::get_id()),
      depth_(previous_ ? previous_->depth_ + 1 : 1) {
    current = this;
}
HleScope::~HleScope() {
    current = previous_;
}
HleScope* HleScope::Current() noexcept {
    return current;
}
void HleScope::ExitThread(std::uint64_t value) {
    for (auto* scope = this; scope && &scope->context_ == &context_ && scope->thread_ == thread_;
         scope = scope->previous_)
        scope->thread_exit_ = value;
}
bool HleScope::WaitFor(std::chrono::nanoseconds duration) const {
    std::mutex mutex;
    std::condition_variable_any changed;
    std::unique_lock lock(mutex);
    changed.wait_for(lock, cancel_, duration, [] { return false; });
    return !cancel_.stop_requested();
}
Result<GuestCallResult> HleScope::InvokeGuest(GuestCodeAddress entry, const GuestCallArgs& args,
                                              const GuestCallOptions& options) {
    if (Current() != this || owner_ != std::this_thread::get_id())
        return MakeError(ErrorCategory::WrongThread, "HleScope::InvokeGuest",
                         "scope is not current on its owner");
    if (depth_ >= 8 || allow_callback_)
        return MakeError(ErrorCategory::WrongState, "HleScope::InvokeGuest",
                         "callback depth/admission is invalid");
    struct Admission {
        bool& flag;
        explicit Admission(bool& flag) : flag(flag) {
            flag = true;
        }
        ~Admission() {
            flag = false;
        }
    } admission(allow_callback_);
    auto call = context_.InvokeGuest(thread_, entry, args, options);
    if (call && call.Value().reason != StopReason::Returned)
        failed_callback_ = call.Value();
    return call;
}
} // namespace Core::GuestCpu::Hle
