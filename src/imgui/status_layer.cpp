// SPDX-FileCopyrightText: Copyright 2026 Citron Emulator Project
// SPDX-FileCopyrightText: Copyright 2026 shadPS4 Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later
// Status groups and graph appearance adapted from Citron status_layer.cpp.
#include <algorithm>
#include <array>
#include <fmt/format.h>
#include <imgui.h>
#include "core/diagnostics/diagnostics_hub_registry.h"
#include "core/diagnostics/overlay_control.h"
#include "core/emulator_settings.h"
#include "imgui/status_layer.h"

namespace ImGui {
void StatusLayer::Prepare(uint64_t now, unsigned width, unsigned height) {
    using namespace ::Core::Diagnostics;
    namespace ov = spatial::imgui::overlay;
    overlay.Begin(width, height);
    ov::StatusSnapshot model;
    model.sampled_at = std::chrono::steady_clock::now();
    model.presentation_fps = game_presents.Fps(now);
    if (!publisher) {
        overlay.Prepare(std::move(model));
        return;
    }
    if (!sample_ns || now - sample_ns >= 500000000) {
        publisher->CopyInto(snapshot, now);
        const auto flips = snapshot.Counter(AdvanceSignal::GuestFlip).count;
        const auto draws = snapshot.Counter(AdvanceSignal::HostDraw).count;
        if (sample_ns && now > sample_ns && flips >= last_flips && draws >= last_draws) {
            const double scale = 1e9 / (now - sample_ns);
            guest_flip_fps = (flips - last_flips) * scale;
            draws_per_second = (draws - last_draws) * scale;
        }
        if (coverage) {
            // Relaxed snapshot of producer counters; shares are window deltas.
            const auto cov = coverage->Read();
            if (sample_ns) {
                window_draws = cov.draws - last_coverage.draws;
                window_scaled_draws = cov.scaled_draws - last_coverage.scaled_draws;
                window_passes = cov.passes - last_coverage.passes;
                window_scaled_passes = cov.scaled_passes - last_coverage.scaled_passes;
                window_resumed_passes = cov.resumed_passes - last_coverage.resumed_passes;
                window_promotions = cov.native_promotions - last_coverage.native_promotions;
                window_readbacks = cov.upscaled_readbacks - last_coverage.upscaled_readbacks;
                window_uploads = cov.image_uploads - last_coverage.image_uploads;
                window_upload_bytes = cov.image_upload_bytes - last_coverage.image_upload_bytes;
                window_fill_clears = cov.fill_clears - last_coverage.fill_clears;
                window_flips = flips >= last_flips ? flips - last_flips : 0;
                window_seconds = now > sample_ns ? (now - sample_ns) / 1e9 : 0.0;
                coverage_sampled = true;
            }
            last_coverage = cov;
        }
        {
            const auto tiles = ::Vulkan::render_pass_stats.Read();
            if (sample_ns) {
                window_tiles = {tiles.passes - last_tiles.passes,
                                tiles.loads - last_tiles.loads,
                                tiles.clears - last_tiles.clears,
                                tiles.stores - last_tiles.stores,
                                tiles.load_pixels - last_tiles.load_pixels,
                                tiles.store_pixels - last_tiles.store_pixels,
                                tiles.empty - last_tiles.empty,
                                tiles.single - last_tiles.single,
                                tiles.few - last_tiles.few,
                                tiles.many - last_tiles.many,
                                tiles.hoisted - last_tiles.hoisted,
                                tiles.hoist_conflicts - last_tiles.hoist_conflicts,
                                tiles.hoist_unavailable - last_tiles.hoist_unavailable,
                                tiles.hoist_interrupted - last_tiles.hoist_interrupted};
            }
            last_tiles = tiles;
        }
        last_flips = flips;
        last_draws = draws;
        sample_ns = now;
        const auto last_present = game_presents.LastPresentNs();
        tooltips.Update(snapshot,
                        last_present && now >= last_present && now - last_present >= 1000000000,
                        coverage_sampled && window_flips && window_uploads >= 4 * window_flips);
    }
    if (!overlay.WantsMetrics()) {
        overlay.Prepare(std::move(model));
        return;
    }
    const ov::ThemeColor green{.6f, .9f, .7f, 1.f}, orange{1.f, .65f, .25f, 1.f};
    const ov::ThemeColor dim{.62f, .72f, .82f, 1.f};
    auto field = [&](const char* group, const char* id, const char* label, std::string value,
                     bool summary = false, std::optional<ov::ThemeColor> color = {}) {
        ov::PropertyStyle style{color, {}};
        if (summary)
            model.summary_items.push_back({ov::StableId(id), ov::LocalizedText(label), value,
                                           ov::StatusSeverity::Normal, style});
        if (!overlay.WantsDetail())
            return;
        auto section =
            std::find_if(model.detail_sections.begin(), model.detail_sections.end(),
                         [&](const auto& item) { return item.id == ov::StableId(group); });
        if (section == model.detail_sections.end()) {
            model.detail_sections.push_back({ov::StableId(group), ov::LocalizedText(group)});
            section = std::prev(model.detail_sections.end());
        }
        section->properties.push_back(
            {ov::LocalizedText(label), value, 0, ov::StatusSeverity::Normal, {}, style});
    };
    ov::MetricSeries frames;
    frames.id = ov::StableId("frame_time");
    frames.label = ov::LocalizedText("New game frame interval");
    frames.unit = ov::LocalizedText("ms");
    frames.reference_value = 33.333f;
    frames.reserveCapacity(240);
    for (size_t i = 0; i < game_presents.Count(); ++i)
        frames.push(game_presents.At(i).milliseconds);
    if (frames.count())
        model.frame_time_ms = frames.current();
    model.graphs.push_back(std::move(frames));
    const auto last = game_presents.LastPresentNs();
    if (last && now >= last && now - last >= 1000000000) {
        model.severity = ov::StatusSeverity::Warning;
        field("Frame timing", "stalled", "No new frame",
              fmt::format("{:.1f} s", (now - last) / 1e9), true, orange);
    }
    field("Frame timing", "fps_meaning", "FPS source", "New game frames / successful present",
          false, dim);
    field("Frame timing", "guest_flip", "Guest flip", fmt::format("{:.1f}/s", guest_flip_fps),
          true);
    field("Frame timing", "all_present", "All presents",
          fmt::format("{:.1f}/s", all_presents.Fps(now)));
    field("Frame timing", "draws", "Draw / dispatch", fmt::format("{:.0f}/s", draws_per_second));
#ifdef __ANDROID__
    constexpr const char* cpu = "FEX x86-64";
    constexpr const char* default_driver = "Unknown";
#else
    // Desktop runs guest code natively and always goes through the system Vulkan loader.
    constexpr const char* cpu = "Native x86-64";
    constexpr const char* default_driver = "System";
#endif
    const char* driver =
        snapshot.driver_identity.find("source=system") != std::string::npos   ? "System"
        : snapshot.driver_identity.find("source=turnip") != std::string::npos ? "Turnip"
                                                                              : default_driver;
    field("Renderer", "backend", "CPU / GPU", fmt::format("{} / Vulkan {}", cpu, driver), true);
    if (!gpu_device.empty())
        field("Renderer", "gpu_device", "GPU", gpu_device, multiple_gpus);
    field("Renderer", "surface", "Surface", fmt::format("{} x {}", width, height));
    field("Renderer", "scale", "Render scale",
          fmt::format("x{:g}", scale_policy.render_eighths / 8.0));
    field("Renderer", "texture", "Texture quality",
          std::string(VideoCore::TextureQualityName(scale_policy.texture)) +
              (scale_policy.legacy ? " (legacy)" : ""));
    field("Renderer", "generation", "Session generation", fmt::format("{}", snapshot.generation));
    if (coverage && scale_policy.render_eighths != 8) {
        field("Scaling", "coverage", "Scaled draws / passes",
              coverage_sampled && window_draws
                  ? fmt::format("{}/{} draws; {}/{} passes", window_scaled_draws, window_draws,
                                window_scaled_passes, window_passes)
                  : "Sampling",
              true,
              !coverage_sampled || !window_draws       ? dim
              : window_scaled_draws * 2 < window_draws ? orange
                                                       : green);
        const auto guest = window_passes - std::min(window_passes, window_resumed_passes);
        field("Scaling", "split", "Guest / split passes",
              fmt::format("{} / +{}", guest, window_resumed_passes), false,
              window_resumed_passes > guest ? orange : green);
        field("Scaling", "promotions", "Native promotions", fmt::format("+{}", window_promotions));
        field("Scaling", "readbacks", "Upscaled readbacks", fmt::format("+{}", window_readbacks));
    }
    if (coverage && coverage_sampled) {
        const double divisor = window_flips ? double(window_flips) : std::max(window_seconds, 1e-3);
        const char* unit = window_flips ? "frame" : "s";
        const double uploads = window_uploads / divisor;
        field("Texture uploads", "uploads", "Re-uploads",
              fmt::format("{:.1f}/{} ({:.1f} MiB)", uploads, unit,
                          window_upload_bytes / divisor / (1024. * 1024.)),
              true, window_flips && uploads >= 4 ? orange : green);
        field("Texture uploads", "fills", "Fill clears",
              fmt::format("{:.1f}/{}", window_fill_clears / divisor, unit), true, green);
        field("Render passes", "tiles", "Tile load / clear / store",
              fmt::format("{:.0f} / {:.0f} / {:.0f} per {}", window_tiles.loads / divisor,
                          window_tiles.clears / divisor, window_tiles.stores / divisor, unit));
        field("Render passes", "pixels", "Load / store pixels",
              fmt::format("{:.1f} / {:.1f} MPix/{}", window_tiles.load_pixels / divisor / 1e6,
                          window_tiles.store_pixels / divisor / 1e6, unit));
        field("Render passes", "passes", "Passes",
              fmt::format("{:.1f}/{}", window_tiles.passes / divisor, unit), false,
              (window_tiles.empty + window_tiles.single) * 2 > window_tiles.passes ? orange
                                                                                   : green);
        field("Render passes", "histogram", "Draws 0 / 1 / 2-7 / 8+",
              fmt::format("{:.0f} / {:.0f} / {:.0f} / {:.0f} per {}", window_tiles.empty / divisor,
                          window_tiles.single / divisor, window_tiles.few / divisor,
                          window_tiles.many / divisor, unit));
        field("Render passes", "hoisted", "Hoisted",
              fmt::format("{:.1f}/{}", window_tiles.hoisted / divisor, unit));
        field("Render passes", "conflicts", "Kept break: conflict / no hold",
              fmt::format("{:.1f} / {:.1f} per {}", window_tiles.hoist_conflicts / divisor,
                          window_tiles.hoist_unavailable / divisor, unit));
    }
    if (!Common::Profiler::GpuTimingEnabled())
        field("GPU timing", "gpu_state", "GPU timing", "Off", true, dim);
    else if (gpu) {
        const auto timing = gpu->Read();
        using Stage = Common::Profiler::GpuStage;
        const auto& guest = timing.stages[size_t(Stage::GuestFrame)];
        const bool fresh =
            guest.count && now >= guest.observed_ns && now - guest.observed_ns <= 1000000000;
        field("GPU timing", "gpu_guest", "GPU guest",
              !timing.supported ? "Unavailable"
              : !fresh          ? "Awaiting completed frame"
                                : fmt::format("{:.2f} ms", guest.last_ms),
              true, fresh ? green : dim);
        if (timing.supported && fresh) {
            field("GPU timing", "gpu_prepare", "Prepare",
                  fmt::format("{:.2f} ms", timing.stages[size_t(Stage::Prepare)].last_ms));
            field("GPU timing", "gpu_present", "Present / redraw",
                  fmt::format("{:.2f} / {:.2f} ms", timing.stages[size_t(Stage::Present)].last_ms,
                              timing.stages[size_t(Stage::Redraw)].last_ms));
            if (overlay.WantsDetail()) {
                ov::MetricSeries series;
                series.id = ov::StableId("gpu_guest");
                series.label = ov::LocalizedText("GPU guest elapsed");
                series.unit = ov::LocalizedText("ms");
                series.reference_value = 33.333f;
                const auto n =
                    std::min<uint64_t>(timing.guest_history_count, timing.guest_history.size());
                for (uint64_t i = 0; i < n; ++i)
                    series.push(timing.guest_history[(timing.guest_history_count - n + i) %
                                                     timing.guest_history.size()]);
                model.detail_sections.back().graphs.push_back(std::move(series));
            }
        }
        field("GPU timing", "gpu_queries", "Pending / dropped / errors",
              fmt::format("{} / {} / {}", timing.pending,
                          timing.dropped_batches + timing.dropped_zones, timing.errors),
              false, timing.errors ? orange : dim);
        field("GPU timing", "gpu_alignment", "Clock alignment",
              timing.estimated_alignment
                  ? fmt::format("Estimated +/- {:.2f} ms", timing.calibration_deviation_ns / 1e6)
              : timing.calibrated ? "Calibrated"
                                  : "Duration only",
              false, dim);
    }
    overlay.Prepare(std::move(model));
}
void StatusLayer::Draw() {
    overlay.Draw();
    if (publisher && ::Core::Diagnostics::status_overlay_enabled.load(std::memory_order_relaxed)) {
        publisher->MarkAvailable(::Core::Diagnostics::AdvanceSignal::OverlayRedraw, true);
        publisher->Advance(::Core::Diagnostics::AdvanceSignal::OverlayRedraw,
                           ::Core::Diagnostics::DiagnosticNowNs());
    }
}
} // namespace ImGui
