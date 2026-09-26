// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once
#include <memory>
#include <string>
#include "common/gpu_timing.h"
#include "core/diagnostics/diagnostics_hub.h"
#include "core/diagnostics/frame_history.h"
#include "imgui/runtime_tooltips.h"
#include "imgui/status_overlay.h"
#include "video_core/renderer_vulkan/render_pass_stats.h"
#include "video_core/texture_cache/scale_coverage.h"
#include "video_core/texture_cache/scale_policy.h"

namespace ImGui {
// Native status content modeled on Citron. Shares the existing renderer's ImGui
// context/backend; no second ImGui ABI or app-global performance counter owner.
class StatusLayer {
public:
    explicit StatusLayer(std::shared_ptr<::Core::Diagnostics::DiagnosticsPublisher> publisher,
                         std::shared_ptr<Common::Profiler::GpuTimingState> gpu = {},
                         VideoCore::ScalePolicySnapshot policy = {},
                         std::shared_ptr<VideoCore::ScaleCoverageCounters> coverage = {},
                         std::string gpu_device = {}, bool multiple_gpus = false)
        : publisher{std::move(publisher)}, gpu{std::move(gpu)}, scale_policy{policy},
          coverage{std::move(coverage)}, gpu_device{std::move(gpu_device)},
          multiple_gpus{multiple_gpus} {}
    void Presented(uint64_t now, bool reused) {
        all_presents.Record(now);
        if (!reused)
            game_presents.Record(now);
    }
    void Prepare(uint64_t now, unsigned width, unsigned height);
    void Draw();

private:
    StatusOverlay overlay;
    RuntimeTooltips tooltips;
    std::shared_ptr<::Core::Diagnostics::DiagnosticsPublisher> publisher;
    std::shared_ptr<Common::Profiler::GpuTimingState> gpu;
    ::Core::Diagnostics::FrameHistory game_presents, all_presents;
    ::Core::Diagnostics::DiagnosticsSnapshot snapshot;
    const VideoCore::ScalePolicySnapshot scale_policy;
    std::shared_ptr<VideoCore::ScaleCoverageCounters> coverage;
    // Vulkan device the renderer runs on; with several GPUs it is also a summary item.
    const std::string gpu_device;
    const bool multiple_gpus;
    VideoCore::ScaleCoverageCounters::Snapshot last_coverage{};
    // Windowed (500 ms) Render Scale coverage: share of attachment draws/passes that
    // rendered scaled, plus promotions and upscaled readbacks in the window.
    // Last 500 ms window: attachment draws / render-pass instances and how many of
    // them rendered at the scaled resolution; shown as fractions, not percentages.
    uint64_t window_draws{}, window_scaled_draws{}, window_passes{}, window_scaled_passes{};
    uint64_t window_resumed_passes{}, window_promotions{}, window_readbacks{};
    // Same window: texture re-uploads (count, guest bytes), compute fills turned into
    // image clears, and guest flips to express both per frame.
    uint64_t window_uploads{}, window_upload_bytes{}, window_fill_clears{}, window_flips{};
    double window_seconds{};
    // Same window: render pass tile traffic (Vulkan::render_pass_stats deltas).
    ::Vulkan::RenderPassStats::Snapshot last_tiles{}, window_tiles{};
    bool coverage_sampled{};
    uint64_t sample_ns{}, last_flips{}, last_draws{};
    double guest_flip_fps{}, draws_per_second{};
};
} // namespace ImGui
