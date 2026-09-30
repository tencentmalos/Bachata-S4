// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once
namespace ImGui::Core {
// Input consumers need capture ownership, without depending on the Vulkan renderer.
bool IsGamepadInputCaptured();
} // namespace ImGui::Core
