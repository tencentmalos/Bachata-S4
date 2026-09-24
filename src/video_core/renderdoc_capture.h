// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once
#include <atomic>
#include <condition_variable>
#include <functional>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <vector>
#include "common/types.h"

namespace VideoCore {
enum class CaptureBoundary { HostPresent, GuestFlip };
enum class CaptureRequestState : u32 {
    Idle, Armed, Starting, Capturing, Writing, Cancelling, Ready, Cancelled, Failed
};
const char* ToString(CaptureRequestState state) noexcept;
struct CaptureTarget {
    u64 generation{};
    void* device{}; // RenderDoc's dispatch pointer from VkInstance, NOT VkDevice.
    void* window{};
    std::string output_directory;
    std::string driver_identity;
};
struct CaptureReceipt {
    u64 request_id{}, generation{};
    CaptureRequestState state{CaptureRequestState::Idle};
    u32 requested_frames{}, completed_frames{}, num_captures_before{}, num_captures_after{};
    // Boundaries to let pass after arming before the capture starts (e.g. to reach a
    // scene after moving the camera), and how many have passed so far.
    u32 delay_boundaries{}, skipped_boundaries{};
    u64 first_present{}, last_present{}, capture_timestamp{}, file_size{};
    u64 first_guest_flip{}, last_guest_flip{};
    CaptureBoundary boundary{CaptureBoundary::HostPresent};
    bool cleanup_pending{};
    // A host present interval is not proof of an exact PS4 logical frame.
    std::string coverage{"host_present_interval"};
    std::string file_path, file_sha256, sidecar_path, run_uuid, capture_uuid, failure_reason;
    std::string command_status{"ok"};
};
class IRenderDocBackend {
public:
    virtual ~IRenderDocBackend() = default;
    virtual bool IsLoaded() const = 0; // Published state only; no driver call.
    virtual bool IsCapturing() = 0;
    virtual u32 GetNumCaptures() = 0;
    virtual bool StartFrameCapture(const CaptureTarget&, const CaptureReceipt&) = 0;
    virtual bool EndFrameCapture(const CaptureTarget&) = 0;
    virtual bool DiscardFrameCapture(const CaptureTarget&) = 0;
    virtual bool GetCapture(u32 index, std::string& path, u64& timestamp) = 0;
    // Verify the file and atomically write its sidecar. Runs outside the state lock.
    virtual bool Finalize(const CaptureTarget&, CaptureReceipt&) = 0;
};

// All backend calls share api_mutex_. Control commands NEVER acquire that mutex
// or call the backend. Query reads an immutable snapshot, including while End or
// Discard is blocked. Teardown retains the target until Unbind has retired calls.
class CaptureCoordinator final {
public:
    using Clock = std::function<u64()>;
    explicit CaptureCoordinator(IRenderDocBackend& backend, Clock clock = {});
    ~CaptureCoordinator();
    bool Bind(CaptureTarget target);
    void RequestStop(u64 generation);
    void Unbind(u64 generation); // Owner teardown only, after renderer workers drain.
    static constexpr u32 MaxDelayBoundaries = 600;
    CaptureReceipt Arm(u32 frames, u64 generation, std::string run_uuid, u64 now_ns,
                       CaptureBoundary boundary = CaptureBoundary::HostPresent, u32 delay = 0);
    CaptureReceipt Cancel(u64 request_id, u64 generation);
    void OnFrameBoundary(u64 generation, u64 present_id, u64 now_ns,
                         CaptureBoundary boundary = CaptureBoundary::HostPresent);
    bool NeedsGuestBoundary(u64 generation) const {
        return generation && guest_boundary_generation_.load(std::memory_order_acquire) == generation;
    }
    void Poll(u64 now_ns); // Also driven by the production watchdog, even without frames/status.
    CaptureReceipt Query(u64 now_ns = 0) const;
    CaptureReceipt Query(u64 request_id, u64 now_ns) const;
    void SetTimeoutNs(u64 timeout_ns);
    void SetWriteTimeoutNs(u64 ns);
    u64 BoundGeneration() const;
private:
    void PublishLocked();
    void RequestCleanupLocked(CaptureRequestState terminal, std::string reason);
    void CleanupUnderApiLock();
    void CheckDeadlineLocked(u64 now_ns);
    IRenderDocBackend& backend_;
    Clock clock_;
    mutable std::mutex state_mutex_;
    std::mutex api_mutex_;
    CaptureTarget target_;
    CaptureReceipt current_;
    std::vector<CaptureReceipt> history_;
    std::shared_ptr<const std::vector<CaptureReceipt>> published_;
    u64 next_request_{1}, deadline_{}, timeout_ns_{5'000'000'000ull};
    u64 write_timeout_ns_{60'000'000'000ull};
    bool backend_active_{}, target_stopping_{};
    CaptureRequestState cleanup_terminal_{CaptureRequestState::Cancelled};
    std::mutex wake_mutex_;
    std::condition_variable wake_;
    std::atomic<bool> stopping_watchdog_{};
    std::atomic<u64> guest_boundary_generation_{};
    std::thread watchdog_;
};
}
