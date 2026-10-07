// SPDX-License-Identifier: GPL-2.0-or-later
// Before the XR headers: their macros break nlohmann/json.
#include "core/emulator_settings.h"
#include "video_core/renderer_vulkan/openxr/layer_panel.h"
#include <algorithm>
#include <cfloat>
#include <cmath>
#include "common/profiler.h"
#include "imgui.h"
#include "imgui/renderer/font_data.h"
#include "imgui/renderer/font_stack.h"
#include "imgui/renderer/imgui_core.h"
#include "spatial/xr/XrMath.h"

namespace Vulkan::OpenXr {
namespace {
using Clock = std::chrono::steady_clock;
using Sensor = Core::HostRuntime::GuestVrSensor;
struct RestoreContext {
    ImGuiContext* previous{ImGui::GetCurrentContext()};
    ~RestoreContext() { ImGui::SetCurrentContext(previous); }
};
XrPosef ToXr(const Sensor::Pose& p) {
    return {{p.orientation[0], p.orientation[1], p.orientation[2], p.orientation[3]},
            {p.position[0], p.position[1], p.position[2]}};
}
// In front of the head, upright, turned only about the vertical axis.
constexpr float Distance = 1.4f;
constexpr float Drop = 0.15f;
} // namespace

LayerPanel::LayerPanel() {
    RestoreContext restore;
    config.canvas_width = 1600;
    config.canvas_height = 900;
    config.space = spatial::xr::XrImguiLayerSpace::Local;
    config.width_meters = 1.28f;
    config.height_meters = 0.72f;
    config.input_enabled = true;
    context = ImGui::CreateContext();
    ImGui::SetCurrentContext(context);
    auto& io = ImGui::GetIO();
    io.IniFilename = io.LogFilename = nullptr;
    io.BackendFlags |= ImGuiBackendFlags_RendererHasTextures;
    io.DisplaySize = {float(config.canvas_width), float(config.canvas_height)};
    // The same fonts in the same order as the window context: layers index them
    // (IMGUI_FONT_MONO, IMGUI_FONT_TEXT_BIG) and expect the console language's glyphs.
    ImFontConfig font_cfg{};
    font_cfg.OversampleH = 2;
    font_cfg.OversampleV = 1;
    font_cfg.RasterizerDensity = 2.f;
    io.Fonts->TexGlyphPadding = 7;
    io.Fonts->Flags |= ImFontAtlasFlags_NoPowerOfTwoHeight | ImFontAtlasFlags_NoBakedLines;
    const int language = EmulatorSettings.GetConsoleLanguage();
    io.FontDefault = ImGui::FontStack::AddPrimaryUiFont(io.Fonts, 32.0f, language, font_cfg, true);
    io.Fonts->AddFontFromMemoryCompressedTTF(imgui_font_proggyvector_regular_compressed_data,
                                             imgui_font_proggyvector_regular_compressed_size,
                                             32.0f);
    ImGui::FontStack::AddPrimaryUiFont(io.Fonts, 128.0f, language, font_cfg, false);
    ImGui::FontStack::AddPrimaryUiFont(io.Fonts, 64.0f, language, font_cfg, true);
    io.FontGlobalScale = 0.8f;
    ImGui::StyleColorsDark();
    ImGui::GetStyle().AntiAliasedLinesUseTex = false;
}

LayerPanel::~LayerPanel() {
    RestoreContext restore;
    ImGui::SetCurrentContext(context);
    renderer.Destroy(); // the backend releases its textures before the context and atlas
    ImGui::DestroyContext(context);
}

bool LayerPanel::Create(XrSession session, const spatial::xr::SwapchainFunctions& f,
                        const spatial::xr::XrImguiVulkanBinding& binding) {
    return renderer.Create(session, f.createSwapchain, f.destroySwapchain,
                           f.enumerateSwapchainFormats, f.enumerateSwapchainImages,
                           f.acquireSwapchainImage, f.waitSwapchainImage,
                           f.releaseSwapchainImage, binding, config);
}

void LayerPanel::Update(const Sensor::HardwareFrame& input) {
    const auto now = Clock::now();
    const float delta = last_update == Clock::time_point{}
                            ? 1.f / 60.f
                            : std::chrono::duration<float>(now - last_update).count();
    last_update = now;
    Common::Profiler::Scope scope{"XR.Layers.Update"};
    RestoreContext restore;
    ImGui::SetCurrentContext(context);
    auto& io = ImGui::GetIO();
    io.DeltaTime = std::max(delta, 1e-4f);

    // Pointer: the first controller whose aim ray meets the panel; trigger clicks.
    bool pointing = false, down = false;
    float x = -FLT_MAX, y = -FLT_MAX;
    if (visible && anchored && input.focused) {
        for (unsigned hand : {1u, 0u}) {
            const auto& h = input.hands[hand];
            if (!h.active || !h.aim.position_valid || !h.aim.orientation_valid) continue;
            const auto hit = spatial::xr::math::RayCastQuadLayer(
                spatial::xr::math::MakeRayFromPose(ToXr(h.aim)), anchor,
                {config.width_meters, config.height_meters});
            if (!hit.hit) continue;
            x = (hit.x_offset + .5f) * float(config.canvas_width);
            y = (.5f - hit.y_offset) * float(config.canvas_height);
            pointing = true;
            down = h.trigger > .5f;
            break;
        }
    }
    // A trigger already held when the ray arrives does not click.
    if (!pointing) pointer_armed = false;
    if (pointing && !down) pointer_armed = true;
    io.AddMousePosEvent(x, y);
    io.AddMouseButtonEvent(0, pointing && pointer_armed && down);

    ImGui::NewFrame();
    ImGui::Core::DrawLayers();
    ImGui::Render();
    ImDrawData* draw_data = ImGui::GetDrawData();
    if (!draw_data || draw_data->TotalVtxCount == 0) {
        visible = false;
        anchored = false; // the next dialog appears where the wearer then looks
        return;
    }
    if (!anchored) {
        const auto& head = input.head;
        if (!head.orientation_valid || !head.position_valid) return;
        const auto pose = ToXr(head);
        const auto forward = spatial::xr::math::Rotate(pose.orientation, XrVector3f{0, 0, -1});
        const float yaw = std::atan2(-forward.x, -forward.z);
        anchor.orientation = {0, std::sin(yaw * .5f), 0, std::cos(yaw * .5f)};
        anchor.position = {pose.position.x - std::sin(yaw) * Distance, pose.position.y - Drop,
                           pose.position.z - std::cos(yaw) * Distance};
        anchored = true;
    }
    spatial::xr::XrImguiRenderedLayer layer{"layers", config, context, draw_data, io.Fonts};
    // Nonblocking: while the previous image still renders, the runtime keeps it.
    renderer.Render(layer, true);
    visible = renderer.HasReleasedImage();
}

bool LayerPanel::Fill(XrSpace local, XrCompositionLayerQuad& layer) const {
    if (!visible || !anchored || !renderer.HasReleasedImage()) return false;
    renderer.FillQuadLayer(local, false, layer);
    layer.pose = anchor;
    layer.size = {config.width_meters, config.height_meters};
    return true;
}
} // namespace Vulkan::OpenXr
