// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once

#include <memory>
#include <string>
#include <utility>
#include <vector>

#include "common/types.h"
#include "video_core/renderer_vulkan/vk_common.h"
#include "scrcpy_capture/snapshot_session.h"

namespace Vulkan {
class Instance;
struct Frame;
struct SubmitInfo;

// Called by the Android diagnostics command registry. Requests are consumed at a
// presentation boundary; this function never touches Vulkan or MediaCodec.
std::string HandleEmbeddedCaptureCommand(const std::vector<std::string>& args);
std::string HandleEmbeddedScreenshotCommand(const std::vector<std::string>& args, u64 generation);
scrcpy::capture::SnapshotSession& EmbeddedScreenshots();

// What screenshots and recordings encode: the game's final render target
// (the canvas, both eyes side by side for a stereo game), or the XR cinema as
// the wearer sees it (room + screen + PSV, one undistorted image per eye).
enum class CaptureSource : u8 { Canvas, Xr };
CaptureSource CurrentCaptureSource();
// capture_source [canvas|xr]; switching is refused while a capture runs.
std::string HandleCaptureSourceCommand(const std::vector<std::string>& args);

class CaptureRecorder final {
public:
    // A recorder only encodes while `source` is the selected capture source.
    explicit CaptureRecorder(const Instance& instance, CaptureSource source = CaptureSource::Canvas);
    ~CaptureRecorder();
    CaptureRecorder(const CaptureRecorder&) = delete;
    CaptureRecorder& operator=(const CaptureRecorder&) = delete;

    void SyncRequest(const Frame& frame, vk::Format source_format, u32 frame_pool_size);
    bool Acquire(const Frame& frame);
    void Record(vk::CommandBuffer cmd, const Frame& frame);
    void AddSubmitSync(SubmitInfo& info) const;
    // Raw semaphores for a caller with its own vkQueueSubmit: wait / signal,
    // both null when nothing was acquired.
    std::pair<vk::Semaphore, vk::Semaphore> SubmitSemaphores() const;
    void Present();  // Caller holds Instance::QueueMutex and has waited for queue submission.
    void Close();

private:
    class Impl;
    std::unique_ptr<Impl> impl_;
};
}  // namespace Vulkan
