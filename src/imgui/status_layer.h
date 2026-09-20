// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once
#include <memory>
#include "common/gpu_timing.h"
#include "video_core/texture_cache/scale_policy.h"
#include "core/diagnostics/diagnostics_hub.h"
#include "core/diagnostics/frame_history.h"

namespace ImGui {
// Native status content modeled on Citron. Shares the existing renderer's ImGui
// context/backend; no second ImGui ABI or app-global performance counter owner.
class StatusLayer {
public:
    explicit StatusLayer(std::shared_ptr<::Core::Diagnostics::DiagnosticsPublisher> publisher,
        std::shared_ptr<Common::Profiler::GpuTimingState> gpu = {},
        VideoCore::ScalePolicySnapshot policy = {})
        : publisher{std::move(publisher)}, gpu{std::move(gpu)}, scale_policy{policy} {}
    void Presented(uint64_t now, bool reused) {
        all_presents.Record(now);
        if (!reused) game_presents.Record(now);
    }
    void Draw(uint64_t now, unsigned width, unsigned height);
private:
    std::shared_ptr<::Core::Diagnostics::DiagnosticsPublisher> publisher;
    std::shared_ptr<Common::Profiler::GpuTimingState> gpu;
    ::Core::Diagnostics::FrameHistory game_presents, all_presents;
    ::Core::Diagnostics::DiagnosticsSnapshot snapshot;
    const VideoCore::ScalePolicySnapshot scale_policy;
    uint64_t sample_ns{}, last_flips{}, last_draws{};
    double guest_flip_fps{}, draws_per_second{};
};
} // namespace ImGui
