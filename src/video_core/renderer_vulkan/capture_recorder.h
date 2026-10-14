// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once

// shadPS4's use of Foundation's capture producers (foundation/modules/capture,
// foundation/docs/guides/emulator-capture.md): stills and H.264 recordings that tools fetch
// through DebugBus (capture_source, capture_screenshot, capture_video, capture_config). The
// presenter's final render target is Foundation FrameCapture (this recorder); the XR cinema is
// Foundation XrLayerCapture (openxr/xr_capture.h), which also serves the canvas source while an
// XR session runs.

#include <memory>
#include <optional>
#include <string>
#include <utility>
#include <vector>

#include "common/types.h"
#include "video_core/renderer_vulkan/vk_common.h"
#include "spatial/capture/FrameCapture.h"
// SelectEye for the presenter's still readback.
#include "scrcpy_capture/snapshot_session.h"

namespace Vulkan {
class Instance;
struct Frame;
struct SubmitInfo;

// Names shadPS4 in Foundation's answers (shadps4.final_render_target, shadps4.xr_composite);
// once, before the first command or producer. The functions below call it.
void ConfigureEmbeddedCapture();
// The Android diagnostics registry's capture commands; any thread, never touches Vulkan.
std::string HandleEmbeddedCaptureCommand(const std::string& command, const std::vector<std::string>& args);
// Foundation's view of `instance`: the graphics queue, or the present queue for the presenter.
spatial::capture::VulkanBinding EmbeddedCaptureBinding(const Instance& instance, bool present_queue);

class CaptureRecorder final {
public:
    explicit CaptureRecorder(const Instance& instance);
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

    // A pending still of the final render target (the presenter reads it back and writes the PNG),
    // then StillDone from any thread.
    std::optional<spatial::capture::FrameStill> TakeStill();
    static void StillDone(const std::string& token, u32 width, u32 height, const std::string& error,
                          u32 views);

private:
    std::unique_ptr<spatial::capture::FrameCapture> frames;
};
}  // namespace Vulkan
