// SPDX-License-Identifier: GPL-2.0-or-later
#include "video_core/renderdoc_capture.h"
#include <algorithm>
#include <chrono>
#include <limits>
#include <stdexcept>

namespace VideoCore {
namespace {
bool Terminal(CaptureRequestState s) {
    return s == CaptureRequestState::Idle || s == CaptureRequestState::Ready ||
           s == CaptureRequestState::Cancelled || s == CaptureRequestState::Failed;
}
}
const char* ToString(CaptureRequestState s) noexcept {
    switch (s) {
    case CaptureRequestState::Idle: return "idle";
    case CaptureRequestState::Armed: return "armed";
    case CaptureRequestState::Starting: return "starting";
    case CaptureRequestState::Capturing: return "capturing";
    case CaptureRequestState::Writing: return "writing";
    case CaptureRequestState::Cancelling: return "cancelling";
    case CaptureRequestState::Ready: return "ready";
    case CaptureRequestState::Cancelled: return "cancelled";
    case CaptureRequestState::Failed: return "failed";
    }
    return "invalid";
}
CaptureCoordinator::CaptureCoordinator(IRenderDocBackend& backend, Clock clock)
    : backend_(backend), clock_(std::move(clock)) {
    PublishLocked();
    if (clock_) watchdog_ = std::thread([this] {
        while (!stopping_watchdog_.load()) {
            Poll(clock_());
            std::unique_lock lock(wake_mutex_);
            wake_.wait_for(lock, std::chrono::milliseconds(20));
        }
    });
}
CaptureCoordinator::~CaptureCoordinator() {
    stopping_watchdog_.store(true);
    wake_.notify_all();
    if (watchdog_.joinable()) watchdog_.join();
    Unbind(BoundGeneration());
}
void CaptureCoordinator::PublishLocked() {
    auto records = std::make_shared<std::vector<CaptureReceipt>>(history_);
    records->push_back(current_);
    std::shared_ptr<const std::vector<CaptureReceipt>> immutable = std::move(records);
    std::atomic_store_explicit(&published_, std::move(immutable), std::memory_order_release);
}
CaptureReceipt CaptureCoordinator::Query(u64) const {
    return std::atomic_load_explicit(&published_, std::memory_order_acquire)->back();
}
CaptureReceipt CaptureCoordinator::Query(u64 request, u64) const {
    const auto records = std::atomic_load_explicit(&published_, std::memory_order_acquire);
    for (const auto& r : *records) if (r.request_id == request) return r;
    CaptureReceipt r;
    r.request_id = request;
    r.command_status = "unknown_request";
    return r;
}
u64 CaptureCoordinator::BoundGeneration() const {
    std::lock_guard lock(state_mutex_);
    return target_.generation;
}
void CaptureCoordinator::SetTimeoutNs(u64 ns) {
    std::lock_guard lock(state_mutex_);
    timeout_ns_ = std::max<u64>(1, ns);
}
bool CaptureCoordinator::Bind(CaptureTarget target) {
    std::scoped_lock api(api_mutex_);
    std::lock_guard lock(state_mutex_);
    if (!target.generation || target_.generation || backend_active_ || current_.cleanup_pending)
        return false;
    target_ = std::move(target);
    target_stopping_ = false;
    return true;
}
CaptureReceipt CaptureCoordinator::Arm(u32 frames, u64 generation, std::string run, u64 now) {
    std::lock_guard lock(state_mutex_);
    auto reject = [&](const char* status) {
        auto r = current_;
        r.command_status = status;
        return r;
    };
    if (frames < 1 || frames > 8) return reject("invalid_frames");
    if (!generation || generation != target_.generation || target_stopping_)
        return reject("no_matching_renderer");
    if (!Terminal(current_.state) || backend_active_ || current_.cleanup_pending)
        return reject("busy");
    if (current_.request_id) {
        history_.push_back(current_);
        if (history_.size() > 16) history_.erase(history_.begin());
    }
    current_ = {};
    current_.request_id = next_request_++;
    current_.generation = generation;
    current_.run_uuid = std::move(run);
    current_.capture_uuid = current_.run_uuid + ":" + std::to_string(generation) + ":" +
                            std::to_string(current_.request_id);
    current_.requested_frames = frames;
    if (!backend_.IsLoaded()) {
        current_.state = CaptureRequestState::Failed;
        current_.failure_reason = "RenderDoc API not loaded";
    } else {
        current_.state = CaptureRequestState::Armed;
    }
    deadline_ = now > std::numeric_limits<u64>::max() - timeout_ns_
                    ? std::numeric_limits<u64>::max() : now + timeout_ns_;
    PublishLocked();
    wake_.notify_all();
    return current_;
}
void CaptureCoordinator::RequestCleanupLocked(CaptureRequestState terminal, std::string reason) {
    if (Terminal(current_.state) && !backend_active_) return;
    current_.cleanup_pending = true;
    current_.state = CaptureRequestState::Cancelling;
    current_.failure_reason = std::move(reason);
    cleanup_terminal_ = terminal;
    PublishLocked();
    wake_.notify_all();
}
CaptureReceipt CaptureCoordinator::Cancel(u64 request, u64 generation) {
    std::lock_guard lock(state_mutex_);
    if (!request || request != current_.request_id || generation != current_.generation) {
        auto r = current_;
        r.command_status = "stale_request";
        return r;
    }
    RequestCleanupLocked(CaptureRequestState::Cancelled, "cancel_requested");
    return current_;
}
void CaptureCoordinator::RequestStop(u64 generation) {
    std::lock_guard lock(state_mutex_);
    if (!generation || target_.generation != generation) return;
    target_stopping_ = true;
    RequestCleanupLocked(CaptureRequestState::Cancelled, "session_stopping");
}
void CaptureCoordinator::CheckDeadlineLocked(u64 now) {
    if (!Terminal(current_.state) && !current_.cleanup_pending && now >= deadline_)
        RequestCleanupLocked(CaptureRequestState::Failed, "capture_deadline_exceeded");
}
void CaptureCoordinator::CleanupUnderApiLock() {
    CaptureTarget target;
    bool active;
    {
        std::lock_guard lock(state_mutex_);
        if (!current_.cleanup_pending) return;
        target = target_;
        active = backend_active_;
    }
    bool clean = !active;
    try {
        if (active) { (void)backend_.DiscardFrameCapture(target); clean = !backend_.IsCapturing(); }
    } catch (...) { clean = false; }
    std::lock_guard lock(state_mutex_);
    if (clean) {
        backend_active_ = false;
        current_.cleanup_pending = false;
        current_.state = cleanup_terminal_;
    } else {
        current_.failure_reason = "capture_cleanup_failed; target retained";
    }
    PublishLocked();
}
void CaptureCoordinator::Poll(u64 now) {
    {
        std::lock_guard lock(state_mutex_);
        CheckDeadlineLocked(now);
    }
    std::unique_lock api(api_mutex_, std::try_to_lock);
    if (api.owns_lock()) CleanupUnderApiLock();
}
void CaptureCoordinator::Unbind(u64 generation) {
    if (!generation) return;
    RequestStop(generation);
    for (;;) {
        std::unique_lock api(api_mutex_); // Owner teardown; status and Stop stay responsive.
        CleanupUnderApiLock();
        {
            std::lock_guard lock(state_mutex_);
            if (target_.generation != generation) return;
            if (!backend_active_ && !current_.cleanup_pending) { target_ = {}; return; }
        }
        // A failed discard must not release Vulkan objects still used by capture.
        // Retain ownership, publish cleanup_pending, and retry on the owner thread.
        api.unlock();
        std::this_thread::sleep_for(std::chrono::milliseconds(20));
    }
}
void CaptureCoordinator::OnFrameBoundary(u64 generation, u64 present, u64 now) {
    std::unique_lock api(api_mutex_, std::try_to_lock);
    if (!api.owns_lock()) return;
    CaptureTarget target;
    CaptureReceipt receipt;
    bool start{};
    {
        std::lock_guard lock(state_mutex_);
        if (generation != target_.generation || target_stopping_ || !present) return;
        CheckDeadlineLocked(now);
        if (current_.cleanup_pending) {
            // Clean up below without retaining the state lock.
        } else if (current_.state == CaptureRequestState::Armed) {
            current_.state = CaptureRequestState::Starting;
            current_.first_present = current_.last_present = present;
            start = true;
        } else if (current_.state == CaptureRequestState::Capturing) {
            if (present <= current_.last_present) return; // Duplicate/stale callbacks never count.
            if (present != current_.last_present + 1) {
                RequestCleanupLocked(CaptureRequestState::Failed, "present_boundary_gap");
            } else {
                current_.last_present = present;
                ++current_.completed_frames;
                if (current_.completed_frames < current_.requested_frames) {
                    PublishLocked();
                    return;
                }
                current_.state = CaptureRequestState::Writing;
            }
        } else return;
        target = target_;
        receipt = current_;
        PublishLocked();
    }
    if (receipt.cleanup_pending) { CleanupUnderApiLock(); return; }
    try {
        if (start) {
            if (backend_.IsCapturing()) {
                std::lock_guard lock(state_mutex_);
                current_.state = CaptureRequestState::Failed;
                current_.failure_reason = "another capture already active";
                PublishLocked();
                return;
            }
            const u32 before = backend_.GetNumCaptures();
            const bool started = backend_.StartFrameCapture(target, receipt);
            const bool active = backend_.IsCapturing();
            std::lock_guard lock(state_mutex_);
            backend_active_ = active;
            current_.num_captures_before = before;
            if (!started || !active) {
                RequestCleanupLocked(CaptureRequestState::Failed, "StartFrameCapture failed");
            } else if (!current_.cleanup_pending) {
                current_.state = CaptureRequestState::Capturing;
            }
            PublishLocked();
            return; // The opening boundary NEVER completes a captured frame.
        }
        const bool ended = backend_.EndFrameCapture(target);
        const bool active = backend_.IsCapturing();
        receipt.num_captures_after = backend_.GetNumCaptures();
        bool ready = ended && !active && receipt.num_captures_after == receipt.num_captures_before + 1 &&
            backend_.GetCapture(receipt.num_captures_before, receipt.file_path, receipt.capture_timestamp);
        if (ready) ready = backend_.Finalize(target, receipt);
        {
            std::lock_guard lock(state_mutex_);
            backend_active_ = active;
            if (current_.cleanup_pending) {
                // End may have produced a file before Cancel won. Retain its identity
                // as a partial/cancelled artifact, never promote that request to Ready.
                const auto state = current_.state;
                const auto reason = current_.failure_reason;
                current_ = std::move(receipt);
                current_.state = state;
                current_.failure_reason = reason;
                current_.cleanup_pending = true;
            } else if (ready) {
                current_ = std::move(receipt);
                current_.state = CaptureRequestState::Ready;
            } else {
                RequestCleanupLocked(CaptureRequestState::Failed, "capture_end_or_artifact_verification_failed");
            }
            PublishLocked();
        }
        CleanupUnderApiLock();
    } catch (...) {
        {
            std::lock_guard lock(state_mutex_);
            // A failing API may already have started capture: conservatively retain ownership.
            backend_active_ = true;
            RequestCleanupLocked(CaptureRequestState::Failed, "capture_backend_exception");
        }
        CleanupUnderApiLock();
    }
}
}
