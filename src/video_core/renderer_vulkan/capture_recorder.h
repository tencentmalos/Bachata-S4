// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once

#include <memory>
#include <string>
#include <vector>

#include "common/types.h"
#include "video_core/renderer_vulkan/vk_common.h"

namespace Vulkan {
class Instance;
struct Frame;
struct SubmitInfo;

// Called by the Android diagnostics command registry. Requests are consumed at a
// presentation boundary; this function never touches Vulkan or MediaCodec.
std::string HandleEmbeddedCaptureCommand(const std::vector<std::string>& args);

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
    void Present();  // Caller holds Instance::QueueMutex and has waited for queue submission.
    void Close();

private:
    class Impl;
    std::unique_ptr<Impl> impl_;
};
}  // namespace Vulkan
