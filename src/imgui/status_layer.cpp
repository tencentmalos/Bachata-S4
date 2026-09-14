// SPDX-FileCopyrightText: Copyright 2026 Citron Emulator Project
// SPDX-FileCopyrightText: Copyright 2026 shadPS4 Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later
// Status groups and graph appearance adapted from Citron status_layer.cpp.
#include "imgui/status_layer.h"
#include "core/diagnostics/overlay_control.h"
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
        if (count) Text("Frame %.2f ms  |  avg %.2f", values[count - 1], sum / count);
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
        Text("CPU  FEX x86-64    GPU  Vulkan / Turnip");
        Text("Surface %u x %u    Generation %llu", width, height,
             static_cast<unsigned long long>(snapshot.generation));
        Text("All presents %.1f/s    Draw/dispatch %.0f/s", all_presents.Fps(now), draws_per_second);
        TextDisabled("GPU time unavailable | Host frame intervals");
        SetWindowFontScale(1.f);
    }
    End();
    PopStyleVar(); PopStyleColor(2);
    publisher->MarkAvailable(AdvanceSignal::OverlayRedraw, true);
    publisher->Advance(AdvanceSignal::OverlayRedraw, now);
}
} // namespace ImGui
