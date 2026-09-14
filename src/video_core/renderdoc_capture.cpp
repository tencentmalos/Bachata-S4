// SPDX-FileCopyrightText: Copyright 2026 shadPS4 Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#include "video_core/renderdoc_capture.h"

namespace VideoCore {

const char* ToString(CaptureRequestState state) noexcept {
    switch (state) {
    case CaptureRequestState::Idle:
        return "idle";
    case CaptureRequestState::Armed:
        return "armed";
    case CaptureRequestState::Capturing:
        return "capturing";
    case CaptureRequestState::Ready:
        return "ready";
    case CaptureRequestState::Cancelled:
        return "cancelled";
    case CaptureRequestState::Failed:
        return "failed";
    }
    return "invalid";
}

CaptureCoordinator::CaptureCoordinator(IRenderDocBackend& backend) : backend_{backend} {}

void CaptureCoordinator::SetTimeoutNs(u64 timeout_ns) {
    std::lock_guard lock(mtx_);
    timeout_ns_ = timeout_ns;
}

CaptureReceipt CaptureCoordinator::Arm(u32 frames, std::string run_uuid,
                                       std::string capture_uuid, u64 now_ns) {
    std::lock_guard lock(mtx_);
    // One transaction at a time: a request already armed/capturing is busy.
    if (current_.state == CaptureRequestState::Armed ||
        current_.state == CaptureRequestState::Capturing) {
        return current_;
    }
    CaptureReceipt r{};
    r.request_id = next_request_id_++;
    r.requested_frames = frames < 1 ? 1 : frames;
    r.run_uuid = std::move(run_uuid);
    r.capture_uuid = std::move(capture_uuid);
    if (!backend_.IsLoaded()) {
        r.state = CaptureRequestState::Failed;
        r.failure_reason = "RenderDoc API not loaded";
        current_ = r;
        return current_;
    }
    r.num_captures_before = backend_.GetNumCaptures();
    r.state = CaptureRequestState::Armed;
    frames_seen_ = 0;
    armed_ns_ = now_ns;
    current_ = r;
    return current_;
}

void CaptureCoordinator::OnFrameBoundary(u64 now_ns) {
    std::lock_guard lock(mtx_);
    if (current_.state == CaptureRequestState::Armed) {
        // First boundary: begin the capture.
        backend_.StartFrameCapture();
        current_.state = CaptureRequestState::Capturing;
        frames_seen_ = 0;
    }
    if (current_.state != CaptureRequestState::Capturing) {
        return;
    }
    ++frames_seen_;
    if (frames_seen_ >= current_.requested_frames) {
        // End the capture and resolve the receipt from the actual capture list.
        const bool ended = backend_.EndFrameCapture();
        if (!ended) {
            current_.state = CaptureRequestState::Failed;
            current_.failure_reason = "EndFrameCapture reported no capture in progress";
            return;
        }
        ResolveReadyLocked(now_ns);
    }
}

void CaptureCoordinator::ResolveReadyLocked(u64 /*now_ns*/) {
    // A capture is Ready only if a NEW capture appeared and its path is retrievable.
    current_.num_captures_after = backend_.GetNumCaptures();
    if (current_.num_captures_after <= current_.num_captures_before) {
        current_.state = CaptureRequestState::Failed;
        current_.failure_reason = "no new capture appeared after EndFrameCapture";
        return;
    }
    std::string path;
    u64 timestamp = 0;
    // The newest capture is at index num_captures_after - 1.
    if (!backend_.GetCapture(current_.num_captures_after - 1, path, timestamp) ||
        path.empty()) {
        current_.state = CaptureRequestState::Failed;
        current_.failure_reason = "capture appeared but its file path was not retrievable";
        return;
    }
    current_.file_path = std::move(path);
    current_.capture_timestamp = timestamp;
    current_.state = CaptureRequestState::Ready;
}

void CaptureCoordinator::Cancel() {
    std::lock_guard lock(mtx_);
    if (current_.state == CaptureRequestState::Capturing) {
        // End and discard an in-progress capture so we do not leave RenderDoc mid-frame.
        (void)backend_.EndFrameCapture();
    }
    if (current_.state == CaptureRequestState::Armed ||
        current_.state == CaptureRequestState::Capturing) {
        current_.state = CaptureRequestState::Cancelled;
    }
}

void CaptureCoordinator::CheckTimeoutLocked(u64 now_ns) {
    // armed_ns_ is the arm time; a request in Armed/Capturing with no boundary for
    // longer than the budget fails. Guard on state, not armed_ns_ != 0, so a valid
    // arm at time 0 is not exempt from the timeout.
    if ((current_.state == CaptureRequestState::Armed ||
         current_.state == CaptureRequestState::Capturing) &&
        now_ns > armed_ns_ && (now_ns - armed_ns_) > timeout_ns_) {
        current_.state = CaptureRequestState::Failed;
        current_.failure_reason = "no frame boundary within capture timeout";
    }
}

CaptureReceipt CaptureCoordinator::Query(u64 now_ns) {
    std::lock_guard lock(mtx_);
    CheckTimeoutLocked(now_ns);
    return current_;
}

CaptureReceipt CaptureCoordinator::Query(u64 request_id, u64 now_ns) {
    std::lock_guard lock(mtx_);
    CheckTimeoutLocked(now_ns);
    if (current_.request_id == request_id) {
        return current_;
    }
    CaptureReceipt r{};
    r.request_id = request_id;
    r.state = CaptureRequestState::Idle;
    r.failure_reason = "unknown request_id";
    return r;
}

}  // namespace VideoCore
