// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once
#include <deque>
#include <string>
#include <unordered_map>
#include "core/diagnostics/diagnostics_hub.h"
#include "imgui/imgui_layer.h"

namespace ImGui {
// Transient runtime notices, independent of the persistent status/detail shell.
// Presenter-thread owned; the same transition is written once to the guest log.
class RuntimeTooltips final : public Layer {
public:
    RuntimeTooltips();
    ~RuntimeTooltips() override;
    void Update(const ::Core::Diagnostics::DiagnosticsSnapshot& snapshot, bool stalled,
                bool high_uploads);
    void Draw() override;
    bool ShouldKeepDrawing() override;

private:
    struct Notice {
        std::string tag, message;
        bool warning;
        uint64_t expires_ns;
    };
    void Transition(const char* tag, std::string value, std::string message, bool warning = false);
    ::Core::Diagnostics::DiagnosticsSnapshot identity;
    std::unordered_map<std::string, std::string> previous;
    std::deque<Notice> visible;
};
} // namespace ImGui
