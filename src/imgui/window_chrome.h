// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once
// Menu bar and status bar of the windowed game window (desktop), after citron's main window.
// Both are hidden in fullscreen. They are drawn before the dock space that holds the game display,
// so they shrink the viewport work area and the game image is fitted between them.
#include <optional>

namespace ImGui::WindowChrome {

// What the status bar shows; published by the status layer on the present thread.
struct StatusInfo {
    std::optional<double> fps; // new game frames per second
    double render_scale = 1.0; // internal render scale
    int overlay_mode = 0;      // status overlay: 0 off, 1 only FPS, 2 summary
};

// Main thread: the window entered or left fullscreen.
void SetFullscreen(bool fullscreen);
// Present thread, once per frame; drawn with the next frame's bars.
void SetStatus(const StatusInfo& info);
// Present thread, right after ImGui::NewFrame and before the dock space.
void Draw();
// Window-coordinate heights the bars cover (0 in fullscreen). Any thread.
float TopInset();
float BottomInset();
// Main thread: a click at window coordinates belongs to the bars, not the game.
bool Contains(float x, float y, float window_height);

} // namespace ImGui::WindowChrome
