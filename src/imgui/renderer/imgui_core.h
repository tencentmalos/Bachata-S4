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

// Side-by-side stereo on a 2D screen (PSVR SBS): the overlays lay out in the left eye and
// RecordDraw draws them again over the right eye, so both eyes see the status layer, dialogs and
// notices. The game frame still fills the whole screen. Present thread, before NewFrame.
void SetEyeSplit(bool split);

// ImGui::Layer UI (system dialogs, notifications, devtools) does not belong to one
// output. By default the window frame (NewFrame) draws the layers over the game;
// while another host owns them -- the XR layer panel on a headset -- NewFrame skips
// them and that host draws them into its own context with DrawLayers. Exactly one
// host at a time, so a dialog's input edge detection runs once per frame.
void SetExternalLayerHost(bool external);
bool ExternalLayerHost();
// Draws the registered layers into the current ImGui context (inside its NewFrame/Render).
// Removal synchronizes with this traversal and MustKeepDrawing, including between sessions.
void DrawLayers();

/// The rectangle of the window the game frame occupies this frame, in framebuffer pixels of the
/// presentation (logical) orientation: the dock space's central node, i.e. the window minus the
/// desktop menu/status bars and docked tool windows. The node passes through, so nothing of ImGui
/// covers it unless a window is placed over it. After NewFrame.
struct GameArea {
    float x, y, width, height;
};
GameArea GetGameArea(ImGuiID dock_id);

/// Ends the ImGui frame and records its texture uploads into `cmdbuf`, outside any render pass.
/// `cmdbuf` completes at timeline tick `frame_tick`; every tick up to `completed_tick` has
/// completed, so the renderer can reuse what those frames used. Returns whether the frame has
/// anything to draw.
bool PrepareDraw(const vk::CommandBuffer& cmdbuf, u64 frame_tick, u64 completed_tick);

/// Records the prepared frame's draws into the caller's dynamic rendering scope (the presenter's
/// single pass over the window image, after the game frame). `rotation` quarter turns clockwise
/// the window image stores the picture turned by (a pre-rotated swapchain); the vertices and clip
/// rectangles are turned in place, the draw count is unchanged.
void RecordDraw(const vk::CommandBuffer& cmdbuf, u32 rotation);

/// Something on screen animates or takes input without a new game frame (a dialog, the status
/// overlay being operated): the presenter redraws the last frame every vblank.
bool MustKeepDrawing();
/// Only stale content to refresh (the visible status overlay): redraw the last frame at a low
/// rate when the game has presented nothing for a while.
bool WantsPeriodicRedraw();

} // namespace ImGui::Core
