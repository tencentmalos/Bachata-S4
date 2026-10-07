// SPDX-FileCopyrightText: Copyright 2024 shadPS4 Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#pragma once

#include <imgui.h>
#include "imgui/input_capture.h"

#include "video_core/renderer_vulkan/vk_instance.h"
#include "vulkan/vulkan_handles.hpp"

union SDL_Event;

namespace Vulkan {
struct Frame;
}

namespace spatial::imgui {
class VulkanRenderer;
struct VulkanTexture;
} // namespace spatial::imgui

namespace ImGui::Core {

void Initialize(const ::Vulkan::Instance& instance, const Frontend::Window& window,
                vk::Format surface_format, const vk::AllocationCallbacks* allocator = nullptr);

/// Foundation's renderer for the host context. Null outside Initialize and Shutdown.
spatial::imgui::VulkanRenderer* Renderer();

/// Releases a renderer texture, or does nothing once the renderer is gone: it destroyed every
/// texture with it. Safe from any thread, including destructors running during shutdown.
void ReleaseTexture(spatial::imgui::VulkanTexture* texture);

void OnResize();

void OnSurfaceFormatChange(vk::Format surface_format);

void Shutdown(const vk::Device& device);

bool ProcessEvent(SDL_Event* event);

void AcquireGamepadInputCapture();
void ReleaseGamepadInputCapture();
// PS4 IME retains its own ABI, keyboard and gamepad mappings. Its modal pointer
// capture only routes host touch events into the shared ImGui context.
void AcquireImeInputCapture();
void ReleaseImeInputCapture();
bool IsImeInputCaptured();

ImGuiID NewFrame(bool is_reusing_frame = false);

// ImGui::Layer UI (system dialogs, notifications, devtools) does not belong to one
// output. By default the window frame (NewFrame) draws the layers over the game;
// while another host owns them -- the XR layer panel on a headset -- NewFrame skips
// them and that host draws them into its own context with DrawLayers. Exactly one
// host at a time, so a dialog's input edge detection runs once per frame.
void SetExternalLayerHost(bool external);
bool ExternalLayerHost();
// Applies queued Layer::AddLayer/RemoveLayer, then draws every layer into the
// current ImGui context (inside its NewFrame/Render).
void DrawLayers();

/// Records the frame into `cmdbuf`, which completes at timeline tick `frame_tick`; every tick up to
/// `completed_tick` has completed, so the renderer can reuse what those frames used.
void Render(const vk::CommandBuffer& cmdbuf, u64 frame_tick, u64 completed_tick,
            const vk::ImageView& image_view, const vk::Extent2D& extent);

bool MustKeepDrawing(); // Force the emulator redraw

} // namespace ImGui::Core
