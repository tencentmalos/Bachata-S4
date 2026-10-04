// SPDX-FileCopyrightText: Copyright 2024-2026 shadPS4 Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#include <algorithm>
#include <atomic>
#include <bit>
#include <chrono>
#include <cstdint>
#include <memory>
#include <mutex>
#include <string>
#ifndef __ANDROID__
#include <SDL3/SDL_events.h>
#endif
#include <imgui.h>

#include "common/path_util.h"
#include "core/debug_state.h"
#include "core/devtools/layer.h"
#include "core/emulator_settings.h"
#include "font_data.h"
#include "font_stack.h"
#include "frontend/window.h"
#include "imgui/imgui_layer.h"
#include "imgui_core.h"
#include "core/diagnostics/overlay_control.h"
#ifndef __ANDROID__
#include "imgui_impl_sdl3.h"
#endif
#include "imgui_internal.h"
#include "spatial/imgui/VulkanRenderer.hpp"
#include "texture_manager.h"
#include "video_core/renderer_vulkan/vk_presenter.h"

namespace {

// Layers register and unregister themselves from their constructors and destructors, and some
// are static objects in other translation units (dialog UIs, notification layers) destroyed at
// process exit in an unspecified order relative to this file. The registry is created on first
// use and deliberately never destroyed, so those calls always find it alive.
struct LayerRegistry {
    std::vector<ImGui::Layer*> layers;
    // Update layers before rendering to allow layer changes to be applied during rendering.
    // Using deque to keep the order of changes in case a Layer is removed then added again
    // between frames.
    std::deque<std::pair<bool, ImGui::Layer*>> change_layers;
    std::mutex change_layers_mutex;
};

LayerRegistry& Registry() {
    static LayerRegistry* const registry = new LayerRegistry{};
    return *registry;
}

} // Anonymous namespace

static ImGuiID dock_id;
static std::atomic<std::uint32_t> force_gamepad_input_capture_count{0};
static const Frontend::Window* platform_window{}; // Owned by the presenter through Shutdown.
static bool using_sdl{};
static std::chrono::steady_clock::time_point previous_frame;

// The renderer outlives every frame, but texture owners (layers, dialogs, the texture worker) may
// release their textures from any thread, including during shutdown: they take this lock and find
// the renderer gone instead of racing its destruction.
static std::mutex renderer_mutex;
static std::unique_ptr<spatial::imgui::VulkanRenderer> renderer;
static std::atomic<spatial::imgui::VulkanRenderer*> renderer_pointer{nullptr};

namespace ImGui {

namespace Core {

static std::atomic<unsigned> ime_input_capture_count{};
void AcquireImeInputCapture() {
    ime_input_capture_count.fetch_add(1, std::memory_order_relaxed);
    AcquireGamepadInputCapture();
}
void ReleaseImeInputCapture() {
    auto expected = ime_input_capture_count.load(std::memory_order_relaxed);
    while (expected && !ime_input_capture_count.compare_exchange_weak(expected, expected - 1)) {}
    if (expected) ReleaseGamepadInputCapture();
}
bool IsImeInputCaptured() { return ime_input_capture_count.load(std::memory_order_relaxed) != 0; }

void AcquireGamepadInputCapture() {
    force_gamepad_input_capture_count.fetch_add(1, std::memory_order_relaxed);
}

void ReleaseGamepadInputCapture() {
    std::uint32_t expected = force_gamepad_input_capture_count.load(std::memory_order_relaxed);
    while (expected != 0) {
        if (force_gamepad_input_capture_count.compare_exchange_weak(
                expected, expected - 1, std::memory_order_relaxed, std::memory_order_relaxed)) {
            return;
        }
    }
    LOG_WARNING(ImGui, "ReleaseGamepadInputCapture called with no active capture");
}

bool IsGamepadInputCaptured() {
    return force_gamepad_input_capture_count.load(std::memory_order_relaxed) > 0;
}

spatial::imgui::VulkanRenderer* Renderer() {
    return renderer_pointer.load(std::memory_order_acquire);
}

void ReleaseTexture(spatial::imgui::VulkanTexture* texture) {
    std::scoped_lock lock{renderer_mutex};
    if (renderer) {
        renderer->ReleaseTexture(texture);
    }
}

void Initialize(const ::Vulkan::Instance& instance, const Frontend::Window& window,
                vk::Format surface_format, const vk::AllocationCallbacks* allocator) {

    const auto config_path = GetUserPath(Common::FS::PathType::UserDir) / "imgui.ini";
    const auto log_path = GetUserPath(Common::FS::PathType::LogDir) / "imgui_log.txt";

    CreateContext();
    ImGuiIO& io = GetIO();
    io.ConfigFlags |= ImGuiConfigFlags_NavEnableKeyboard;
    io.ConfigFlags |= ImGuiConfigFlags_NavEnableGamepad;
    io.ConfigFlags |= ImGuiConfigFlags_DockingEnable;
    io.DisplaySize = ImVec2((float)window.GetWidth(), (float)window.GetHeight());
    PushStyleVar(ImGuiStyleVar_WindowRounding, 6.0f); // Makes the window edges rounded

    auto path = config_path.u8string();
    char* config_file_buf = new char[path.size() + 1]();
    std::memcpy(config_file_buf, path.c_str(), path.size());
    io.IniFilename = config_file_buf;

    path = log_path.u8string();
    char* log_file_buf = new char[path.size() + 1]();
    std::memcpy(log_file_buf, path.c_str(), path.size());
    io.LogFilename = log_file_buf;

    ImFontConfig font_cfg{};
    font_cfg.OversampleH = 2;
    font_cfg.OversampleV = 1;
    io.BackendFlags |= ImGuiBackendFlags_RendererHasTextures;
    font_cfg.RasterizerDensity = 2.f;
    io.Fonts->TexGlyphPadding = 7;
    io.Fonts->Flags |= ImFontAtlasFlags_NoPowerOfTwoHeight | ImFontAtlasFlags_NoBakedLines;
    const int console_language = EmulatorSettings.GetConsoleLanguage();
    io.FontDefault = FontStack::AddPrimaryUiFont(io.Fonts, 32.0f, console_language, font_cfg, true);

    io.Fonts->AddFontFromMemoryCompressedTTF(imgui_font_proggyvector_regular_compressed_data,
                                             imgui_font_proggyvector_regular_compressed_size,
                                             32.0f);

    // Avoid exploding atlas size on Metal/MoltenVK when CJK fallback is enabled.
    FontStack::AddPrimaryUiFont(io.Fonts, 128.0f, console_language, font_cfg, false);

    // Big Picture
    FontStack::AddPrimaryUiFont(ImGui::GetIO().Fonts, 64.0f, EmulatorSettings.GetConsoleLanguage(),
                                font_cfg, true);

    // Let the atlas size grow to the largest image the device can create,
    // floored to the power of two the packer requires.
    const u32 max_dim = instance.GetPhysicalDevice().getProperties().limits.maxImageDimension2D;
    const int atlas_max = static_cast<int>(std::bit_floor(std::max<u32>(max_dim, 512u)));
    io.Fonts->TexMaxWidth = atlas_max;
    io.Fonts->TexMaxHeight = atlas_max;


    io.FontGlobalScale = 0.5f;

    StyleColorsDark();
    GetStyle().AntiAliasedLinesUseTex = false;

    ::Core::Devtools::Layer::SetupSettings();
    platform_window = &window;
    using_sdl = window.GetSDLWindow() != nullptr;
    previous_frame = std::chrono::steady_clock::now();
#ifndef __ANDROID__
    if (using_sdl)
        Sdl::Init(window.GetSDLWindow());
#endif

    // Uploads are recorded into the frame's command buffer and retired by timeline tick, so this
    // renderer never submits or waits on its own.
    const spatial::imgui::VulkanRendererCreateInfo renderer_info{
        .instance = instance.GetInstance(),
        .physical_device = instance.GetPhysicalDevice(),
        .device = instance.GetDevice(),
        .get_instance_proc_addr = VULKAN_HPP_DEFAULT_DISPATCHER.vkGetInstanceProcAddr,
        .get_device_proc_addr = VULKAN_HPP_DEFAULT_DISPATCHER.vkGetDeviceProcAddr,
        .color_format = static_cast<VkFormat>(surface_format),
        .sdf_fonts = true,
        // Game frames and images may be drawn with UVs outside [0, 1].
        .sampler_address_mode = VK_SAMPLER_ADDRESS_MODE_REPEAT,
        .allocator = reinterpret_cast<const VkAllocationCallbacks*>(allocator),
        .on_error =
            [](VkResult result, const char* operation, void*) {
                LOG_ERROR(ImGui, "Vulkan error {} in {}", vk::to_string(vk::Result{result}),
                          operation);
            },
    };
    std::string error;
    auto created = spatial::imgui::VulkanRenderer::Create(renderer_info, &error);
    if (!created) {
        LOG_CRITICAL(ImGui, "ImGui renderer creation failed: {}", error);
    }
    {
        std::scoped_lock lock{renderer_mutex};
        renderer = std::move(created);
        renderer_pointer.store(renderer.get(), std::memory_order_release);
    }

    TextureManager::StartWorker();

    char label[32];
    ImFormatString(label, IM_ARRAYSIZE(label), "WindowOverViewport_%08X", GetMainViewport()->ID);
    dock_id = ImHashStr(label);

#ifndef __ANDROID__
    const auto dpi = using_sdl ? SDL_GetWindowDisplayScale(window.GetSDLWindow())
                               : window.GetWindowInfo().render_surface_scale;
#else
    const auto dpi = window.GetWindowInfo().render_surface_scale;
#endif
#ifndef __ANDROID__
    ::Core::Diagnostics::StatusOverlayMailbox().SetPixelDensity(dpi);
#endif
    if (dpi > 0.0f) {
        GetIO().FontGlobalScale *= dpi;
    }

#ifndef __ANDROID__
    std::at_quick_exit([] {
        if (GetCurrentContext())
            SaveIniSettingsToDisk(GetIO().IniFilename);
    });
#endif
}

void OnResize() {
#ifndef __ANDROID__
    if (using_sdl)
        Sdl::OnResize();
    else
#endif
    if (platform_window)
        GetIO().DisplaySize = ImVec2(static_cast<float>(platform_window->GetWidth()),
                                     static_cast<float>(platform_window->GetHeight()));
}

void OnSurfaceFormatChange(vk::Format surface_format) {
    if (auto* active = Renderer()) {
        active->SetColorFormat(static_cast<VkFormat>(surface_format));
    }
}

void Shutdown(const vk::Device& device) {
    if (!GetCurrentContext())
        return;
    auto result = device.waitIdle();
    if (result != vk::Result::eSuccess) {
        LOG_WARNING(ImGui, "Failed to wait for Vulkan device idle on shutdown: {}",
                    vk::to_string(result));
    }

    TextureManager::StopWorker();

    const ImGuiIO& io = GetIO();
    const auto ini_filename = (void*)io.IniFilename;
    const auto log_filename = (void*)io.LogFilename;

    {
        std::scoped_lock lock{renderer_mutex};
        renderer_pointer.store(nullptr, std::memory_order_release);
        if (renderer) {
            auto& textures = GetPlatformIO().Textures;
            renderer->DetachImGuiTextures(
                {textures.Data, static_cast<std::size_t>(textures.Size)});
            renderer.reset();
        }
    }
#ifndef __ANDROID__
    if (using_sdl)
        Sdl::Shutdown();
#endif
    platform_window = nullptr;
    using_sdl = false;
    DestroyContext();

    delete[] (char*)ini_filename;
    delete[] (char*)log_filename;
}

bool ProcessEvent(SDL_Event* event) {
#ifdef __ANDROID__
    (void)event;
    return false; // Android input is delivered by its own adapter; no SDL events.
#else
    if (!using_sdl)
        return false; // Android input is delivered by its own adapter.
    if (event->type == SDL_EVENT_KEY_DOWN && event->key.key == SDLK_F10 && !event->key.repeat && !IsImeInputCaptured()) {
        ::Core::Diagnostics::StatusOverlayMailbox().Request("controls");
        return true;
    }
    Sdl::ProcessEvent(event);
    switch (event->type) {
    // Don't block release/up events
    case SDL_EVENT_MOUSE_WHEEL:
    case SDL_EVENT_MOUSE_BUTTON_DOWN: {
        // The status overlay never takes navigation focus, so the check below does not see it.
        // A click or scroll inside its published input regions is still the overlay's, not the
        // game's; pointer motion keeps reaching the game (mouse camera).
        float x = event->type == SDL_EVENT_MOUSE_WHEEL ? event->wheel.mouse_x : event->button.x;
        float y = event->type == SDL_EVENT_MOUSE_WHEEL ? event->wheel.mouse_y : event->button.y;
        int width = 0, height = 0;
        if (SDL_Window* sdl_window = SDL_GetWindowFromID(event->type == SDL_EVENT_MOUSE_WHEEL
                                                             ? event->wheel.windowID
                                                             : event->button.windowID);
            sdl_window && SDL_GetWindowSize(sdl_window, &width, &height) && width > 0 &&
            height > 0 &&
            ::Core::Diagnostics::StatusOverlayMailbox().Contains(x / width, y / height)) {
            return true;
        }
        [[fallthrough]];
    }
    case SDL_EVENT_MOUSE_MOTION: {
        const auto& io = GetIO();
        return io.WantCaptureMouse && io.Ctx->NavWindow != nullptr &&
               (io.Ctx->NavWindow->Flags & ImGuiWindowFlags_NoNav) == 0;
    }
    case SDL_EVENT_TEXT_INPUT:
    case SDL_EVENT_KEY_DOWN: {
        if (IsGamepadInputCaptured()) {
            // Keep keyboard events flowing through the regular input-binding path while IME/OSK
            // captures gamepad input, so keyboard equivalents of pad buttons still update the
            // virtual controller state.
            return false;
        }
        const auto& io = GetIO();
        return io.WantCaptureKeyboard && io.Ctx->NavWindow != nullptr &&
               io.Ctx->NavWindow->ID != dock_id;
    }
    case SDL_EVENT_GAMEPAD_BUTTON_DOWN:
    case SDL_EVENT_GAMEPAD_BUTTON_UP:
    case SDL_EVENT_GAMEPAD_AXIS_MOTION:
    case SDL_EVENT_GAMEPAD_TOUCHPAD_DOWN:
    case SDL_EVENT_GAMEPAD_TOUCHPAD_UP:
    case SDL_EVENT_GAMEPAD_TOUCHPAD_MOTION:
    case SDL_EVENT_GAMEPAD_SENSOR_UPDATE: {
        if (IsGamepadInputCaptured()) {
            // Let controller events continue to input bindings so OSK shortcuts and virtual
            // controller state keep updating; game-side pad reads are blocked in libScePad.
            return false;
        }
        const auto& io = GetIO();
        return io.NavActive && io.Ctx->NavWindow != nullptr && io.Ctx->NavWindow->ID != dock_id;
    }
    default:
        return false;
    }
#endif
}

ImGuiID NewFrame(bool is_reusing_frame) {
    {
        auto& registry = Registry();
        std::scoped_lock lock{registry.change_layers_mutex};
        auto& layers = registry.layers;
        auto& change_layers = registry.change_layers;
        while (!change_layers.empty()) {
            const auto [to_be_added, layer] = change_layers.front();
            if (to_be_added) {
                layers.push_back(layer);
            } else {
                const auto [begin, end] = std::ranges::remove(layers, layer);
                layers.erase(begin, end);
            }
            change_layers.pop_front();
        }
    }

#ifndef __ANDROID__
    if (using_sdl)
        Sdl::NewFrame(is_reusing_frame);
    else
#endif
    {
        OnResize();
        const auto now = std::chrono::steady_clock::now();
        GetIO().DeltaTime =
            std::max(std::chrono::duration<float>(now - previous_frame).count(), 1e-6f);
        previous_frame = now;
    }
    ImGui::NewFrame();
    SetKeyOwner(ImGuiKey_GamepadFaceUp, ImHashStr("shadps4/pad"));

    ImGuiWindowFlags flags =
        ImGuiDockNodeFlags_PassthruCentralNode | ImGuiDockNodeFlags_AutoHideTabBar;
    if (!DebugState.IsShowingDebugMenuBar()) {
        flags |= ImGuiDockNodeFlags_NoTabBar;
    }
    ImGuiID dockId = DockSpaceOverViewport(0, GetMainViewport(), flags);

    for (auto* layer : Registry().layers) {
        layer->Draw();
    }

    return dockId;
}

void Render(const vk::CommandBuffer& cmdbuf, u64 frame_tick, u64 completed_tick,
            const vk::ImageView& image_view, const vk::Extent2D& extent) {
    ImGui::Render();
    ImDrawData* draw_data = GetDrawData();
    auto* active = Renderer();
    if (active == nullptr) {
        return;
    }
    active->BeginFrame(frame_tick, completed_tick);
    active->UpdateTextures(cmdbuf, draw_data);
    if (draw_data->CmdListsCount == 0) {
        return;
    }

    if (EmulatorSettings.IsVkHostMarkersEnabled()) {
        cmdbuf.beginDebugUtilsLabelEXT(vk::DebugUtilsLabelEXT{
            .pLabelName = "ImGui Render",
        });
    }

    vk::RenderingAttachmentInfo color_attachments[1]{
        {
            .imageView = image_view,
            .imageLayout = vk::ImageLayout::eColorAttachmentOptimal,
            .loadOp = vk::AttachmentLoadOp::eClear,
            .storeOp = vk::AttachmentStoreOp::eStore,
        },
    };
    vk::RenderingInfo render_info{};
    render_info.renderArea = vk::Rect2D{
        .offset = {0, 0},
        .extent = extent,
    };
    render_info.layerCount = 1;
    render_info.colorAttachmentCount = 1;
    render_info.pColorAttachments = color_attachments;
    cmdbuf.beginRendering(render_info);
    active->RenderDrawData(cmdbuf, *draw_data);
    cmdbuf.endRendering();
    if (EmulatorSettings.IsVkHostMarkersEnabled()) {
        cmdbuf.endDebugUtilsLabelEXT();
    }
}

bool MustKeepDrawing() {
    return ::Core::Diagnostics::StatusOverlayMailbox().Pending() ||
           ::Core::Diagnostics::status_overlay_enabled.load(std::memory_order_relaxed) ||
           std::ranges::any_of(Registry().layers,
                               [](Layer* layer) { return layer->ShouldKeepDrawing(); }) ||
           Registry().change_layers.size() > 1;
}

} // namespace Core

void Layer::AddLayer(Layer* layer) {
    auto& registry = Registry();
    std::scoped_lock lock{registry.change_layers_mutex};
    registry.change_layers.emplace_back(true, layer);
}

void Layer::RemoveLayer(Layer* layer) {
    auto& registry = Registry();
    std::scoped_lock lock{registry.change_layers_mutex};
    registry.change_layers.emplace_back(false, layer);
}

} // namespace ImGui
