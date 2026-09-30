// SPDX-License-Identifier: GPL-2.0-or-later
#include "status_panel.h"
#include <algorithm>
#include <array>
#include <stdexcept>
#include <fmt/format.h>
#include "common/profiler.h"
#include "status_scene.h"

namespace Vulkan::OpenXr {
namespace {
struct RestoreContext {
    ImGuiContext* previous{ImGui::GetCurrentContext()};
    ~RestoreContext() { ImGui::SetCurrentContext(previous); }
};
using Clock = std::chrono::steady_clock;
}
StatusPanel::StatusPanel(int layout) : vertical(layout == 1) {
    RestoreContext restore;
    config.canvas_width = vertical ? 384 : 1152;
    config.canvas_height = vertical ? 576 : 192;
    config.space = spatial::xr::XrImguiLayerSpace::View;
    config.width_meters = vertical ? .4f : 1.2f;
    config.height_meters = vertical ? .6f : .2f;
    // Both layouts share the same upper-left corner in VIEW space.
    config.position = {-1.7f + config.width_meters * .5f,
                       1.1f - config.height_meters * .5f, -2.5f};
    config.input_enabled = false;
    context = ImGui::CreateContext();
    ImGui::SetCurrentContext(context);
    auto& io = ImGui::GetIO();
    io.IniFilename = io.LogFilename = nullptr;
    io.BackendFlags |= ImGuiBackendFlags_RendererHasTextures;
    io.DisplaySize = {static_cast<float>(config.canvas_width),
                      static_cast<float>(config.canvas_height)};
    io.ConfigFlags |= ImGuiConfigFlags_NoMouse | ImGuiConfigFlags_NoKeyboard;
    ImFontConfig font;
    font.SizePixels = 32;
    io.FontDefault = io.Fonts->AddFontDefault(&font);
    ImGui::StyleColorsDark();
}
StatusPanel::~StatusPanel() {
    RestoreContext restore;
    ImGui::SetCurrentContext(context);
    renderer.Destroy();
    ImGui::DestroyContext(context);
}
bool StatusPanel::Create(XrSession session, const spatial::xr::SwapchainFunctions& f,
                          const spatial::xr::XrImguiVulkanBinding& binding) {
    return renderer.Create(session, f.createSwapchain, f.destroySwapchain,
        f.enumerateSwapchainFormats, f.enumerateSwapchainImages,
        f.acquireSwapchainImage, f.waitSwapchainImage, f.releaseSwapchainImage,
        binding, config);
}
void StatusPanel::Update(const spatial::imgui::overlay::StatusSnapshot& status,
                          uint32_t width, uint32_t height, int selected_theme) {
    const auto now = Clock::now();
    if (now - last_update < std::chrono::milliseconds(250)) return;
    Common::Profiler::Scope scope{"XR.Status.ImGuiUpdate"};
    RestoreContext restore;
    ImGui::SetCurrentContext(context);
    auto& io = ImGui::GetIO();
    io.DeltaTime = last_update == Clock::time_point{} ? .25f : std::chrono::duration<float>(now - last_update).count();
    last_update = now;
    snapshot = status; output_width = width; output_height = height; theme = selected_theme;
    ImGui::NewFrame();
    // Bake the full compact HUD alphabet on first use, including degree signs.
    // This avoids synchronous font texture uploads when a new digit first occurs.
    if (updates == 0) {
        auto* baked = io.FontDefault->GetFontBaked(32);
        for (unsigned ch=32;ch<127;++ch) baked->FindGlyph(ch);
        for (unsigned ch : {0xb0u, 0x2013u, 0x2014u}) baked->FindGlyph(ch);
    }
    Draw();
    ImGui::Render();
    spatial::xr::XrImguiRenderedLayer layer{"status", config, context, ImGui::GetDrawData(), io.Fonts};
    const bool ok = renderer.Render(layer, true);
    if (!ok && !renderer.LastRenderReused()) throw std::runtime_error("XR status ImGui render failed");
    if (ok && !renderer.LastRenderReused()) ++updates;
    else ++reused;
    const auto micros = std::chrono::duration_cast<std::chrono::microseconds>(Clock::now()-now).count();
    ReportStatusLayer(fmt::format("mode=psvr-imgui model=disabled layout={} anchor=top-left canvas={}x{} space=VIEW distance=2.5m "
        "updates={} reused={} update_us={} max_hz=4 nonblocking=true released={}",
        vertical ? "vertical" : "horizontal", config.canvas_width, config.canvas_height,
        updates, reused, micros, renderer.HasReleasedImage()));
}
void StatusPanel::Draw() {
    const bool stale = snapshot.sampled_at == Clock::time_point{} || Clock::now()-snapshot.sampled_at > std::chrono::seconds(2);
    const ImU32 accent = theme == 2 ? IM_COL32(97,228,219,255) : theme == 1 ? IM_COL32(166,193,255,255) : IM_COL32(107,200,255,255);
    auto* draw = ImGui::GetBackgroundDrawList();
    const ImVec2 size = ImGui::GetIO().DisplaySize;
    draw->AddRectFilled({0,0}, size, IM_COL32(10,17,28,240), 12);
    draw->AddRect({1,1}, {size.x-1,size.y-1}, accent, 12);
    const auto metric = [&](std::string_view id) {
        const auto it = std::ranges::find_if(snapshot.summary_items, [&](const auto& item) {return item.id.value() == id;});
        return stale || it == snapshot.summary_items.end() ? std::string("--") : it->value;
    };
    const auto fps = snapshot.presentation_fps;
    const std::array<std::string, 6> labels{"GAME FPS", "CPU", "GPU", "MEMORY", "BATTERY", "OUTPUT / EYE"};
    const std::array<std::string, 6> values{
        stale ? "Waiting for frames" : fps ? fmt::format("{:.1f}", *fps) : "--",
        metric("perf.cpu"), metric("perf.gpu"), metric("perf.ram"), metric("perf.battery"),
        fmt::format("{} x {}", output_width, output_height)};
    for (unsigned i=0;i<labels.size();++i) {
        const unsigned columns = vertical ? 1 : 3;
        const float x = 20 + (i%columns)*384, y = 10 + (i/columns)*94;
        draw->PushClipRect({x,y}, {x+352,y+90}, true);
        draw->AddText({x,y}, accent, labels[i].c_str());
        draw->AddText({x,y+38}, IM_COL32(235,242,250,255), values[i].c_str());
        draw->PopClipRect();
    }
}
bool StatusPanel::Fill(XrSpace view, XrCompositionLayerQuad& layer) const {
    if (!renderer.HasReleasedImage()) return false;
    renderer.FillQuadLayer(view, false, layer);
    return true;
}
} // namespace Vulkan::OpenXr
