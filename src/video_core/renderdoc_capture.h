// SPDX-FileCopyrightText: Copyright 2026 shadPS4 Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

// RenderDoc capture coordinator (spec docs/specs/android-graphics-debugging-toolkit.md
// §3.2). Turns the old fire-and-forget TriggerCapture into a request/receipt
// transaction: arm N frames, advance at provable frame boundaries, and produce a
// receipt (capture count delta, file path, timestamp, run/capture identity)
// distinguishing capturing / writing / ready / cancelled / failed.
//
// The coordinator core is deliberately backend-injected so it is unit-testable on
// the host with no RenderDoc library and no Vulkan: the actual RENDERDOC_API_1_6_0
// calls live behind IRenderDocBackend, and the production wiring supplies an
// adapter over rdoc_api. §3.2 rules encoded here:
//
//   * One capture transaction at a time. A second Arm while a request is armed or
//     capturing returns busy (the existing request_id), never clobbering it.
//   * A receipt is Ready only when a NEW capture actually appeared
//     (GetNumCaptures increased) AND its file path is retrievable. A non-empty
//     file path is not assumed; "file exists" is not inferred from "Start
//     returned".
//   * Bounded frames: Arm requests a frame count; the coordinator times out to
//     Failed if the boundary never arrives, rather than waiting forever. It never
//     fabricates a frame boundary.
//   * RenderDoc absent -> Arm fails immediately with a clear reason, never a fake
//     success.

#pragma once

#include <cstdint>
#include <mutex>
#include <optional>
#include <string>

#include "common/types.h"

namespace VideoCore {

enum class CaptureRequestState : u32 {
    Idle = 0,     // no request
    Armed,        // request accepted, waiting for the next frame boundary
    Capturing,    // StartFrameCapture issued, waiting for the frame(s) to complete
    Ready,        // a new capture appeared and its file path was retrieved
    Cancelled,    // cancelled before completion
    Failed,       // RenderDoc absent, timed out, or no new capture appeared
};

[[nodiscard]] const char* ToString(CaptureRequestState state) noexcept;

struct CaptureReceipt final {
    u64 request_id{0};
    CaptureRequestState state{CaptureRequestState::Idle};
    u32 requested_frames{0};
    u32 num_captures_before{0};
    u32 num_captures_after{0};
    std::string file_path;       // absolute path from GetCapture, when Ready
    u64 capture_timestamp{0};    // seconds since epoch, from GetCapture
    std::string run_uuid;        // process run identity
    std::string capture_uuid;    // this capture transaction
    std::string failure_reason;  // set on Failed
};

// The RenderDoc operations the coordinator needs, injected so the core is
// testable without the real library. Production supplies an adapter over
// rdoc_api; the host test supplies a fake.
class IRenderDocBackend {
public:
    virtual ~IRenderDocBackend() = default;
    [[nodiscard]] virtual bool IsLoaded() const = 0;
    [[nodiscard]] virtual u32 GetNumCaptures() = 0;
    virtual void StartFrameCapture() = 0;
    // Returns true if a frame capture was in progress and ended.
    virtual bool EndFrameCapture() = 0;
    // Fills path/timestamp for capture index idx; returns false if idx invalid.
    [[nodiscard]] virtual bool GetCapture(u32 idx, std::string& path, u64& timestamp) = 0;
};

class CaptureCoordinator final {
public:
    explicit CaptureCoordinator(IRenderDocBackend& backend);

    CaptureCoordinator(const CaptureCoordinator&) = delete;
    CaptureCoordinator& operator=(const CaptureCoordinator&) = delete;

    // Arms a capture of `frames` (>=1) frames. Returns the receipt: on acceptance,
    // state Armed with a fresh request_id; if RenderDoc is absent, Failed; if a
    // request is already active, the existing request's receipt (busy). run_uuid is
    // stamped into the receipt for correlation.
    CaptureReceipt Arm(u32 frames, std::string run_uuid, std::string capture_uuid,
                       u64 now_ns);

    // Advances the capture at a provable frame boundary (called from the presenter
    // after a successful present). Armed -> Capturing (StartFrameCapture) on the
    // first boundary; after `frames` boundaries, ends the capture and resolves the
    // receipt from the GetNumCaptures delta + GetCapture.
    void OnFrameBoundary(u64 now_ns);

    // Cancels the active request (if any). A capture in progress is ended and
    // discarded; the receipt becomes Cancelled.
    void Cancel();

    // Non-blocking snapshot of the current/last receipt. now_ns lets Query apply
    // the arm timeout (an Armed/Capturing request with no frame boundary within the
    // budget resolves to Failed), so a stuck capture is visible without a boundary.
    [[nodiscard]] CaptureReceipt Query(u64 now_ns);
    [[nodiscard]] CaptureReceipt Query(u64 request_id, u64 now_ns);

    // Arm timeout: if no frame boundary advances an Armed/Capturing request within
    // this budget, it becomes Failed. Checked on Query and OnFrameBoundary.
    void SetTimeoutNs(u64 timeout_ns);

private:
    void ResolveReadyLocked(u64 now_ns);
    void CheckTimeoutLocked(u64 now_ns);

    IRenderDocBackend& backend_;
    mutable std::mutex mtx_;
    CaptureReceipt current_{};
    u64 next_request_id_{1};
    u32 frames_seen_{0};
    u64 armed_ns_{0};
    u64 timeout_ns_{5'000'000'000ull};  // 5s default
};

}  // namespace VideoCore
