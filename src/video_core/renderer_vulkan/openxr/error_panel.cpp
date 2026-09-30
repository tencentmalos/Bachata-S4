// SPDX-License-Identifier: GPL-2.0-or-later
#include "video_core/renderer_vulkan/openxr/error_panel.h"
#include <atomic>
#include <cfloat>
#include "frontend/window.h"
#include "imgui.h"

#include "video_core/renderer_vulkan/vk_instance.h"
#include "video_core/renderer_vulkan/openxr/runtime.h"

namespace Vulkan::OpenXr {
ErrorPanel::ErrorPanel() {
    config.canvas_width = 1280;
    config.canvas_height = 720;
    config.space = spatial::xr::XrImguiLayerSpace::Local;
    config.position = {0, 0, -2.5f};
    config.width_meters = 3.2f;
    config.height_meters = 1.8f;
    config.input_enabled = true;
    context = ImGui::CreateContext();
    auto& io = ImGui::GetIO();
    io.IniFilename = nullptr;
    io.ConfigFlags |= ImGuiConfigFlags_NavEnableGamepad;
    io.BackendFlags |= ImGuiBackendFlags_HasGamepad | ImGuiBackendFlags_RendererHasTextures;
    ImFontConfig font;
    font.SizePixels = 26;
    io.Fonts->AddFontDefault(&font);
    ImGui::StyleColorsDark();
    ImGui::GetStyle().ScaleAllSizes(1.7f);
}
ErrorPanel::~ErrorPanel() {
    ImGui::SetCurrentContext(context);
    renderer.Destroy(); // backend must release textures before context/atlas
    ImGui::DestroyContext(context);
}
int ErrorPanel::Draw(bool left, bool right, bool up, bool down, bool accept, bool cancel, bool focused,
                     const Pointer& pointer, float delta_time) {
    ImGui::SetCurrentContext(context);
    auto& io = ImGui::GetIO();
    if (!focused) armed = false;
    if (focused && !left && !right && !up && !down && !accept && !cancel) armed = true;
    io.DisplaySize = {1280, 720};
    io.DeltaTime = delta_time > 0 ? delta_time : 1.f / 72;
    if (!focused || !pointer.visible) pointer_armed = false;
    if (focused && pointer.visible && !pointer.down) pointer_armed = true;
    io.AddMousePosEvent(focused && pointer.visible ? pointer.x : -FLT_MAX,
                        focused && pointer.visible ? pointer.y : -FLT_MAX);
    io.AddMouseButtonEvent(0, focused && pointer.visible && pointer_armed && pointer.down);
    if (focused && pointer.visible) io.AddMouseWheelEvent(0, pointer.scroll * io.DeltaTime * 10);
    io.AddKeyEvent(ImGuiKey_GamepadDpadLeft, armed && focused && left);
    io.AddKeyEvent(ImGuiKey_GamepadDpadRight, armed && focused && right);
    io.AddKeyEvent(ImGuiKey_GamepadDpadUp, armed && focused && up);
    io.AddKeyEvent(ImGuiKey_GamepadDpadDown, armed && focused && down);
    io.AddKeyEvent(ImGuiKey_GamepadFaceDown, armed && focused && accept);
    io.AddKeyEvent(ImGuiKey_GamepadFaceRight, armed && focused && cancel);
    ImGui::NewFrame();
    ImGui::SetNextWindowPos({0, 0});
    ImGui::SetNextWindowSize(io.DisplaySize);
    ImGui::Begin("Session error", nullptr, ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_NoMove |
                 ImGuiWindowFlags_NoSavedSettings);
    ImGui::TextColored({1.f, .55f, .4f, 1.f}, "Unable to continue the game");
    ImGui::Spacing();
    ImGui::BeginChild("Details", {0, 470}, ImGuiChildFlags_Borders);
    // Keep the actual failure readable; the import inventory is diagnostic
    // context and does not prove which import caused the failure.
    const auto line_end = detail.find('\n');
    const auto summary = detail.substr(0, line_end);
    ImGui::TextWrapped("%s", summary.c_str());
    ImGui::Spacing();
    if (line_end != std::string::npos && ImGui::CollapsingHeader("Technical details")) {
        ImGui::PushFont(nullptr, 20);
        ImGui::TextWrapped("%s", detail.c_str() + line_end + 1);
        ImGui::PopFont();
    }
    ImGui::EndChild();
    ImGui::TextUnformatted("Aim + trigger, or D-pad + A/Cross.  B/Circle: library");
    int action = 0;
    if (ImGui::Button("Retry", {300, 65})) action = 1;
    ImGui::SetItemDefaultFocus();
    ImGui::SameLine();
    if (ImGui::Button("Return to library", {450, 65}) ||
        (armed && focused && ImGui::IsKeyPressed(ImGuiKey_GamepadFaceRight, false))) action = 2;
    ImGui::End();
    if (focused && pointer.visible) {
        ImGui::GetForegroundDrawList()->AddCircle({pointer.x, pointer.y}, 10,
            pointer.down ? IM_COL32(255, 120, 70, 255) : IM_COL32(80, 220, 255, 255), 24, 2);
    }
    ImGui::Render();
    spatial::xr::XrImguiRenderedLayer frame{"error", config, context, ImGui::GetDrawData(), io.Fonts};
    if (!renderer.Render(frame)) throw std::runtime_error("OpenXR ImGui error layer render failed");
    return action;
}
namespace {
class XrWindow final : public Frontend::Window {
    s32 GetWidth() const override { return 1280; }
    s32 GetHeight() const override { return 720; }
    // The shared ImGui backend loads WSI entry points even for an XR-only
    // swapchain. Enable the Android surface extensions; no Surface is created.
    Frontend::WindowSystemInfo GetWindowInfo() const override {
        return {.type = Frontend::WindowSystemType::Android};
    }
    bool RequestKeyboard() override { return false; }
    void ReleaseKeyboard() override {}
};
std::mutex owner_mutex;
std::unique_ptr<Instance> owner;
uint64_t owner_token{};
std::atomic_uint key_bits{};
}
unsigned ErrorKeys() { return key_bits.load(); }
void ShowError(std::string detail, DriverLease driver, uint64_t token) {
    std::scoped_lock lock(owner_mutex);
    owner.reset();
    key_bits = 0;
    XrWindow window;
    // Reuse the process-pinned loader; switching ICDs in-process is unsafe.
    if (!driver) throw std::runtime_error("No initialized Vulkan driver for XR error screen");
    auto next = std::make_unique<Instance>(window, -1, false, false, std::move(driver));
    if (!next->Xr()) throw std::runtime_error("XR Activity is unavailable");
    next->Xr()->StartError(*next, std::move(detail));
    owner = std::move(next);
    owner_token = token;
}
void HideError(uint64_t token) {
    std::scoped_lock lock(owner_mutex);
    if (token && token != owner_token) return;
    owner.reset();
    owner_token = 0;
    key_bits = 0;
}
int PollErrorAction(uint64_t token) {
    std::scoped_lock lock(owner_mutex);
    return owner && token == owner_token ? owner->Xr()->ErrorAction() : -1;
}
std::string ErrorStatus() {
    std::unique_lock lock(owner_mutex, std::try_to_lock);
    if (!lock.owns_lock()) return "XR error panel initializing/stopping";
    return owner ? owner->Xr()->ErrorStatus() : "XR error panel inactive";
}
bool ErrorKey(int key, bool down) {
    std::unique_lock lock(owner_mutex, std::try_to_lock);
    if (!lock.owns_lock() || !owner) return false;
    unsigned bit{};
    switch (key) {
    case 21: bit = 1; break; // Android DPAD_LEFT
    case 22: bit = 2; break;
    case 19: bit = 16; break; // Android DPAD_UP
    case 20: bit = 32; break;
    case 23: case 96: bit = 4; break; // DPAD_CENTER / BUTTON_A
    case 4: case 97: bit = 8; break; // BACK / BUTTON_B
    default: return false;
    }
    if (down) key_bits.fetch_or(bit); else key_bits.fetch_and(~bit);
    return true;
}
} // namespace Vulkan::OpenXr
