// SPDX-FileCopyrightText: Copyright 2024 shadPS4 Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#pragma once

#include <imgui.h>

#include "video_core/renderer_vulkan/vk_instance.h"
#include "vulkan/vulkan_handles.hpp"

union SDL_Event;

namespace Vulkan {
struct Frame;
}

namespace ImGui::Core {

void Initialize(const ::Vulkan::Instance& instance, const Frontend::Window& window, u32 image_count,
                vk::Format surface_format, const vk::AllocationCallbacks* allocator = nullptr);

void OnResize();

void OnSurfaceFormatChange(vk::Format surface_format);

void Shutdown(const vk::Device& device);

bool ProcessEvent(SDL_Event* event);

void AcquireGamepadInputCapture();
void ReleaseGamepadInputCapture();
bool IsGamepadInputCaptured();
// PS4 IME retains its own ABI, keyboard and gamepad mappings. Its modal pointer
// capture only routes host touch events into the shared ImGui context.
void AcquireImeInputCapture();
void ReleaseImeInputCapture();
bool IsImeInputCaptured();

ImGuiID NewFrame(bool is_reusing_frame = false);

void Render(const vk::CommandBuffer& cmdbuf, const vk::ImageView& image_view,
            const vk::Extent2D& extent);

bool MustKeepDrawing(); // Force the emulator redraw

} // namespace ImGui::Core
