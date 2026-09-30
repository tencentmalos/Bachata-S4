// SPDX-License-Identifier: GPL-2.0-or-later
#include "imgui/input_capture.h"
// Portable input contracts have no UI. Production links imgui_core.cpp.
namespace ImGui::Core {
bool IsGamepadInputCaptured() {
    return false;
}
} // namespace ImGui::Core
#include "core/libraries/pad/pad_vibration.h"
// Exclude diagnostic logging only; haptic routing and the Foundation queue are real.
namespace Libraries::Pad::Vibration {
void Record(s32, u8, u8, Outcome, const char*) {}
} // namespace Libraries::Pad::Vibration
