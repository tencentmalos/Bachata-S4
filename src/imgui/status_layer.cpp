// SPDX-FileCopyrightText: Copyright 2026 Citron Emulator Project
// SPDX-FileCopyrightText: Copyright 2026 shadPS4 Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later
// Status groups and graph appearance adapted from Citron status_layer.cpp.
#include "imgui/status_layer.h"
#include "core/diagnostics/overlay_control.h"
#include "core/emulator_settings.h"
#include <imgui.h>
#include <algorithm>
#include <array>

namespace ImGui {
void StatusLayer::Draw(uint64_t now, unsigned width, unsigned height) {
    using namespace ::Core::Diagnostics;
    if (!status_overlay_enabled.load(std::memory_order_relaxed) || !publisher) return;
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
                window_tiles = {tiles.passes - last_tiles.passes, tiles.loads - last_tiles.loads,
                                tiles.clears - last_tiles.clears, tiles.stores - last_tiles.stores,
                                tiles.load_pixels - last_tiles.load_pixels,
                                tiles.store_pixels - last_tiles.store_pixels,
                                tiles.empty - last_tiles.empty, tiles.single - last_tiles.single,
                                tiles.few - last_tiles.few, tiles.many - last_tiles.many,
                                tiles.hoisted - last_tiles.hoisted,
                                tiles.hoist_conflicts - last_tiles.hoist_conflicts,
                                tiles.hoist_unavailable - last_tiles.hoist_unavailable,
                                tiles.hoist_interrupted - last_tiles.hoist_interrupted};
            }
            last_tiles = tiles;
        }
        last_flips = flips; last_draws = draws; sample_ns = now;
    }
    const float panel_width = std::min(480.f, std::max(260.f, width * .32f));
    const float margin = std::max(8.f, width * .008f);
    SetNextWindowPos({width - margin, margin}, ImGuiCond_Always, {1, 0});
    SetNextWindowSize({std::min(panel_width, width - 2 * margin), 0});
    SetNextWindowBgAlpha(.9f);
    PushStyleColor(ImGuiCol_WindowBg, {0.015f, .06f, .10f, 1});
    PushStyleColor(ImGuiCol_Border, {.14f, .45f, .58f, 1});
    PushStyleVar(ImGuiStyleVar_WindowPadding, {14.f, 10.f});
    constexpr auto flags = ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_AlwaysAutoResize |
        ImGuiWindowFlags_NoSavedSettings | ImGuiWindowFlags_NoInputs |
        ImGuiWindowFlags_NoFocusOnAppearing | ImGuiWindowFlags_NoDocking;
    if (Begin("Status###shadps4_status", nullptr, flags)) {
        SetWindowFontScale(width >= 1600 ? 1.3f : 1.f);
        TextColored({.4f, .85f, 1.f, 1.f}, "shadPS4  |  Status");
        const double fps = game_presents.Fps(now);
        Text("FPS  %.1f    Guest flip  %.1f", fps, guest_flip_fps);
        TextDisabled("New game frames / successful present");
        std::array<float, 240> values{};
        const size_t count = std::min(values.size(), game_presents.Count());
        float sum{}, minimum = 0, maximum = 0;
        for (size_t i = 0; i < count; ++i) {
            const auto& s = game_presents.At(game_presents.Count() - count + i);
            values[i] = s.milliseconds; sum += values[i];
            minimum = i ? std::min(minimum, values[i]) : values[i];
            maximum = std::max(maximum, values[i]);
        }
        const auto last_present = game_presents.LastPresentNs();
        if (last_present && now >= last_present && now - last_present >= 1000000000)
            TextColored({1.f, .65f, .25f, 1.f}, "No new game frame for %.1f s", (now - last_present) / 1e9);
        if (count) Text("Last frame %.2f ms  |  avg %.2f", values[count - 1], sum / count);
        else TextDisabled("Frame time: waiting for presents");
        const float axis = std::max(33.334f, maximum * 1.05f);
        const ImVec2 size{GetContentRegionAvail().x, std::clamp(height * .095f, 65.f, 110.f)};
        InvisibleButton("##frame_pacing", size);
        auto* list = GetWindowDrawList();
        const auto lo = GetItemRectMin(), hi = GetItemRectMax();
        list->AddRectFilled(lo, hi, IM_COL32(3, 15, 26, 235), 4);
        list->AddRect(lo, hi, IM_COL32(35, 108, 148, 225), 4);
        for (float reference : {16.667f, 33.333f}) {
            const float y = hi.y - reference / axis * size.y;
            for (float x = lo.x; x < hi.x; x += 10)
                list->AddLine({x, y}, {std::min(x + 5, hi.x), y}, IM_COL32(180, 210, 230, 100));
        }
        std::array<ImVec2, 240> points;
        for (size_t i = 0; i < count; ++i)
            points[i] = {lo.x + size.x * i / std::max<size_t>(1, count - 1),
                         hi.y - values[i] / axis * size.y};
        if (count > 1) list->AddPolyline(points.data(), static_cast<int>(count),
            IM_COL32(74, 215, 178, 255), ImDrawFlags_None, 1.7f);
        TextDisabled("Min %.1f / Max %.1f ms | 16.7 / 33.3 guides", minimum, maximum);
        Separator();
        const char* driver_label = snapshot.driver_identity.find("source=system") != std::string::npos
                                       ? "System" : snapshot.driver_identity.find("source=turnip") != std::string::npos
                                       ? "Turnip" : "Unknown";
        Text("CPU  FEX x86-64    GPU  Vulkan / %s", driver_label);
        const auto internal_scale = scale_policy.render_eighths * 12.5f;
        const char* scale_label = internal_scale == 25 ? "0.25" : internal_scale == 37.5f ? "0.375" : internal_scale == 50 ? "0.5" : internal_scale == 75 ? "0.75" : "1.0";
        Text("Surface %u x %u (x%s)    Generation %llu", width, height, scale_label,
             static_cast<unsigned long long>(snapshot.generation));
        Text("Texture %s%s", VideoCore::TextureQualityName(scale_policy.texture).data(), scale_policy.legacy ? " (legacy)" : "");
        if (coverage && scale_policy.render_eighths != 8) {
            // Attachment draws and render-pass instances of the last 500 ms window that
            // actually rendered at x<scale>, as scaled/total; orange below half.
            if (coverage_sampled && window_draws) {
                const bool low = window_scaled_draws * 2 < window_draws;
                TextColored(low ? ImVec4{1.f, .65f, .25f, 1.f} : ImVec4{.6f, .9f, .7f, 1.f},
                            "Scale x%s  draws %llu/%llu  passes %llu/%llu", scale_label,
                            static_cast<unsigned long long>(window_scaled_draws),
                            static_cast<unsigned long long>(window_draws),
                            static_cast<unsigned long long>(window_scaled_passes),
                            static_cast<unsigned long long>(window_passes));
            } else TextDisabled("Scale x%s coverage: sampling", scale_label);
            if (coverage_sampled && window_passes) {
                // Pass instances the guest expressed (target changes) vs. fragments that
                // re-opened the same targets after an emulator-imposed break.
                const auto guest = window_passes - std::min(window_passes, window_resumed_passes);
                const bool split = window_resumed_passes > guest;
                TextColored(split ? ImVec4{1.f, .65f, .25f, 1.f} : ImVec4{.6f, .9f, .7f, 1.f},
                            "passes: guest %llu  +%llu split", static_cast<unsigned long long>(guest),
                            static_cast<unsigned long long>(window_resumed_passes));
            }
            if (window_promotions || window_readbacks)
                TextDisabled("native promotions +%llu | upscaled readbacks +%llu",
                             static_cast<unsigned long long>(window_promotions),
                             static_cast<unsigned long long>(window_readbacks));
        }
        if (coverage && coverage_sampled) {
            // Texture re-uploads of the last window per guest frame (per second without
            // flips); orange from 4 per frame. Fill clears: compute fills replaced by clears.
            const bool per_frame = window_flips != 0;
            const double divisor = per_frame ? double(window_flips) : std::max(window_seconds, 1e-3);
            const double uploads = window_uploads / divisor;
            TextColored(per_frame && uploads >= 4 ? ImVec4{1.f, .65f, .25f, 1.f} : ImVec4{.6f, .9f, .7f, 1.f},
                        "Re-uploads %.1f/%s (%.1f MB)  fill clears %.1f", uploads,
                        per_frame ? "frame" : "s", window_upload_bytes / divisor / (1024.0 * 1024.0),
                        window_fill_clears / divisor);
        }
        if (coverage_sampled && window_tiles.passes) {
            // Tile traffic per guest frame (per second without flips): attachments loaded
            // into / stored from tile memory, in megapixels, and passes by draw count.
            const bool per_frame = window_flips != 0;
            const double d = per_frame ? double(window_flips) : std::max(window_seconds, 1e-3);
            Text("Tiles/%s  load %.0f (%.1f MPix)  clear %.0f  store %.0f (%.1f MPix)",
                 per_frame ? "frame" : "s", window_tiles.loads / d, window_tiles.load_pixels / d / 1e6,
                 window_tiles.clears / d, window_tiles.stores / d, window_tiles.store_pixels / d / 1e6);
            const bool tiny = (window_tiles.empty + window_tiles.single) * 2 > window_tiles.passes;
            TextColored(tiny ? ImVec4{1.f, .65f, .25f, 1.f} : ImVec4{.6f, .9f, .7f, 1.f},
                        "Passes/%s %.0f  draws 0:%.0f 1:%.0f 2-7:%.0f 8+:%.0f", per_frame ? "frame" : "s",
                        window_tiles.passes / d, window_tiles.empty / d, window_tiles.single / d,
                        window_tiles.few / d, window_tiles.many / d);
            if (window_tiles.hoisted || window_tiles.hoist_conflicts || window_tiles.hoist_unavailable)
                Text("Hoisted/%s %.1f  kept break: conflict %.1f  no hold %.1f",
                     per_frame ? "frame" : "s", window_tiles.hoisted / d,
                     window_tiles.hoist_conflicts / d, window_tiles.hoist_unavailable / d);
        }
        Text("All presents %.1f/s    Draw/dispatch %.0f/s", all_presents.Fps(now), draws_per_second);
        if (!Common::Profiler::GpuTimingEnabled()) TextDisabled("GPU timing off | gpu_timing start");
        else if (gpu) {
            const auto timing = gpu->Read();
            using Stage = Common::Profiler::GpuStage;
            const auto& guest = timing.stages[size_t(Stage::GuestFrame)];
            if (!timing.supported) TextDisabled("GPU timestamps unavailable");
            else if (!guest.count || now < guest.observed_ns || now - guest.observed_ns > 1000000000)
                TextDisabled("GPU: awaiting completed guest frame (stale/partial)");
            else {
                Text("GPU guest %.2f ms | prepare %.2f ms", guest.last_ms, timing.stages[size_t(Stage::Prepare)].last_ms);
                Text("GPU present %.2f / redraw %.2f ms", timing.stages[size_t(Stage::Present)].last_ms, timing.stages[size_t(Stage::Redraw)].last_ms);
                std::array<float, 120> history{};
                const auto n = std::min<uint64_t>(timing.guest_history_count, history.size());
                for (uint64_t i = 0; i < n; ++i) history[i] = timing.guest_history[(timing.guest_history_count - n + i) % history.size()];
                PlotLines("##gpu_times", history.data(), int(n), 0, "GPU guest elapsed ms", 0.f,
                    std::max(33.334f, *std::max_element(history.begin(), history.end()) * 1.05f), {GetContentRegionAvail().x, 58});
            }
            TextDisabled("GPU queries: pending %u / dropped %llu / errors %llu", timing.pending,
                (unsigned long long)(timing.dropped_batches + timing.dropped_zones), (unsigned long long)timing.errors);
            if (timing.estimated_alignment) TextDisabled("CPU/GPU alignment estimated +/- %.2f ms", timing.calibration_deviation_ns / 1e6);
            else if (!timing.calibrated) TextDisabled("Duration only; clock alignment pending");
        }
        SetWindowFontScale(1.f);
    }
    End();
    PopStyleVar(); PopStyleColor(2);
    publisher->MarkAvailable(AdvanceSignal::OverlayRedraw, true);
    publisher->Advance(AdvanceSignal::OverlayRedraw, now);
}
} // namespace ImGui
