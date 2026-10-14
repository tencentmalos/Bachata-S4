// SPDX-License-Identifier: GPL-2.0-or-later
#include "imgui/online_content/online_content.h"
#include <SDL3/SDL.h>
#include <imgui.h>

namespace OnlineContent {
bool Open() {
    return SDL_OpenURL("emupsp://browse?platform=ps4");
}
void Page::Draw() {
    ImGui::TextUnformatted("EmuPSP / ROM Library");
    ImGui::Spacing();
    ImGui::TextWrapped("Online content is managed by the separate EmuPSP app. "
                       "Download and prepare files there, then add their folder to this game library.");
    if (ImGui::Button("Open EmuPSP", ImVec2(240, 48))) launch_failed = !Open();
    if (launch_failed) {
        ImGui::TextWrapped("Could not launch EmuPSP. Install it and register its emupsp URL handler.");
    }
}
}
