// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once

#include <atomic>
#include <memory>
#include <string>
#include <vector>

namespace Core::HostRuntime::WakeProxy {

// Issues the native wakes of guest synchronization (the condition notify, i.e. the futex wake,
// that makes a selected waiter runnable) on behalf of the guest thread that selected the
// waiter. On Android phones a wake costs the waker 12-20 us of CPU inside the kernel's wakeup
// path, and a game thread that feeds a job system wakes its workers about 5000 times a second
// (Bloodborne's main thread). Waking is moved to one proxy thread on the lowest-capacity CPUs:
// after each batch it stays awake for a short window (WFE on arm64), and a post that finds it
// awake costs the poster no system call; a post that finds it asleep wakes it, which costs the
// poster one wake as before. Enabled with each session unless `debug.shadps4.wake_proxy=0`;
// DebugBus `wake_proxy` switches it at run time (guest_wake_proxy.cpp).
//
// The waiter's state must already be published (under its own lock) when posting: the proxy
// only performs the notify, after the poster has returned. The posted pointer keeps the waiter
// alive until then.
using NotifyFn = void (*)(void* waiter);
using PostFn = bool (*)(NotifyFn notify, std::shared_ptr<void>& waiter);

// Set while the proxy is on. Builds without the proxy (unit tests) leave it null.
inline std::atomic<PostFn> post{nullptr};

/// Queue notify(waiter.get()); false when the proxy is off or its queue is full, in which case
/// the caller notifies itself.
template <class T>
bool Post(NotifyFn notify, const std::shared_ptr<T>& waiter) {
    const PostFn fn = post.load(std::memory_order_acquire);
    if (fn == nullptr) {
        return false;
    }
    std::shared_ptr<void> held = waiter;
    return fn(notify, held);
}

/// Start the proxy thread if needed and route posts to it, or stop routing (queued posts are
/// still issued).
void Enable(bool on);

/// DebugBus: on | off | spin <us> | status (guest_wake_proxy.cpp).
std::string Command(const std::vector<std::string>& args);

} // namespace Core::HostRuntime::WakeProxy
