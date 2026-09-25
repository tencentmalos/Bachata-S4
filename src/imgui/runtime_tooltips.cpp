// SPDX-License-Identifier: GPL-2.0-or-later
#include <algorithm>
#include <fmt/format.h>
#include <imgui.h>
#include "common/logging/log.h"
#include "core/diagnostics/diagnostics_hub_registry.h"
#include "imgui/runtime_tooltips.h"

namespace ImGui {
RuntimeTooltips::RuntimeTooltips() {
    AddLayer(this);
}
RuntimeTooltips::~RuntimeTooltips() {
    RemoveLayer(this);
}
void RuntimeTooltips::Transition(const char* tag, std::string value, std::string message,
                                 bool warning) {
    const auto it = previous.find(tag);
    if (it != previous.end() && it->second == value)
        return;
    previous[tag] = std::move(value);
    if (message.empty())
        return;
    // Keep each record on one line, and bound externally supplied terminal details.
    message.resize(std::min<std::size_t>(message.size(), 512));
    for (char& c : message)
        if (c == '\n' || c == '\r' || c == '\t')
            c = ' ';
    const auto log = fmt::format(
        "[OVERLAY_EVENT] tag={} pid={} generation={} run_uuid={} mono_ns={} severity={} message={}",
        tag, identity.pid, identity.generation, identity.run_uuid, identity.snapshot_ns,
        warning ? "warning" : "info", message);
    // Dedicated guest diagnostic sink bypasses the ordinary category filter.
    Common::Log::WriteOverlayEvent(log);
    std::erase_if(visible, [&](const Notice& n) { return n.tag == tag; });
    if (visible.size() >= 3)
        visible.pop_front();
    visible.push_back({tag, std::move(message), warning,
                       identity.snapshot_ns + (warning ? 7000000000ULL : 4000000000ULL)});
}
void RuntimeTooltips::Update(const ::Core::Diagnostics::DiagnosticsSnapshot& next, bool stalled,
                             bool high_uploads) {
    const bool fresh = next.pid != identity.pid || next.generation != identity.generation ||
                       next.run_uuid != identity.run_uuid;
    if (fresh) {
        previous.clear();
        visible.clear();
    }
    identity = next;
    Transition("session.stage", next.stage, next.stage.empty() ? "" : "Session: " + next.stage);
    Transition("session.stop", next.stop_reason,
               next.stop_reason.empty() ? "" : "Stop: " + next.stop_reason);
    Transition("session.fault", next.terminal_detail,
               next.terminal_detail.empty() ? "" : "Fault: " + next.terminal_detail, true);
    const bool had_stall = previous.contains("present.stall") && previous["present.stall"] == "1";
    Transition("present.stall", stalled ? "1" : "0",
               stalled     ? "No new game frame for at least 1 second"
               : had_stall ? "Game frames resumed"
                           : "",
               stalled);
    const bool had_uploads =
        previous.contains("texture.reuploads") && previous["texture.reuploads"] == "1";
    Transition("texture.reuploads", high_uploads ? "1" : "0",
               high_uploads  ? "Texture re-uploads: at least 4 per frame"
               : had_uploads ? "Texture re-uploads returned below 4 per frame"
                             : "",
               high_uploads);
}
bool RuntimeTooltips::ShouldKeepDrawing() {
    const auto now = ::Core::Diagnostics::DiagnosticNowNs();
    return std::any_of(visible.begin(), visible.end(),
                       [&](const Notice& n) { return n.expires_ns > now; });
}
void RuntimeTooltips::Draw() {
    const auto now = ::Core::Diagnostics::DiagnosticNowNs();
    std::erase_if(visible, [&](const Notice& n) { return n.expires_ns <= now; });
    if (visible.empty())
        return;
    const auto size = GetIO().DisplaySize;
    const float width = std::min(600.f, std::max(1.f, size.x - 24.f));
    float y = 16.f;
    for (const auto& n : visible) {
        SetNextWindowPos({size.x * .5f, y}, ImGuiCond_Always, {.5f, 0.f});
        SetNextWindowSize({width, 0.f});
        SetNextWindowBgAlpha(.90f);
        PushStyleColor(ImGuiCol_Text,
                       n.warning ? ImVec4{1.f, .65f, .25f, 1.f} : ImVec4{.7f, .9f, 1.f, 1.f});
        if (Begin(("##runtime_notice_" + n.tag).c_str(), nullptr,
                  ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_AlwaysAutoResize |
                      ImGuiWindowFlags_NoInputs | ImGuiWindowFlags_NoNav |
                      ImGuiWindowFlags_NoSavedSettings | ImGuiWindowFlags_NoFocusOnAppearing)) {
            TextWrapped("[%s] %s", n.tag.c_str(), n.message.c_str());
            y += GetWindowHeight() + 8.f;
        }
        End();
        PopStyleColor();
    }
}
} // namespace ImGui
