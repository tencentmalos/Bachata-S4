// SPDX-License-Identifier: GPL-2.0-or-later
#include <atomic>
#include <imgui.h>
#include "core/libraries/ime/ime_ui_shared.h"
#include "guest_msg_dialog.h"
#include "imgui/imgui_layer.h"

namespace Core::HostRuntime {
namespace {
// Only this permanent identity enters ImGui's deferred raw-pointer layer queue.
// Session state is weakly referenced; UI responses carry a request generation.
class MessageLayer final : public ImGui::Layer {
public:
    void Set(const std::shared_ptr<GuestMsgDialog>& dialog) {
        std::lock_guard lock(mutex);
        std::erase_if(active, [&](const auto& weak) {
            const auto value = weak.lock();
            return !value || value == dialog;
        });
        active.push_back(dialog);
    }
    std::shared_ptr<GuestMsgDialog> Get() {
        std::vector<std::weak_ptr<GuestMsgDialog>> snapshot;
        {
            std::lock_guard lock(mutex);
            snapshot = active;
        }
        // ErrorDialog may temporarily cover another local modal. Resume it
        // after dismissal instead of losing its UI and input-capture owner.
        // Never read a dialog under the layer lock: Publish takes the reverse
        // order while installing its weak reference.
        for (auto it = snapshot.rbegin(); it != snapshot.rend(); ++it)
            if (auto dialog = it->lock();
                dialog && dialog->Read().status == GuestMsgDialog::Status::RUNNING)
                return dialog;
        return {};
    }
    bool ShouldKeepDrawing() override {
        auto dialog = Get();
        return popup.load() || (dialog && dialog->Read().status == GuestMsgDialog::Status::RUNNING);
    }
    void Draw() override {
        auto dialog = Get();
        const auto state = dialog ? dialog->Read() : GuestMsgDialog::Snapshot{};
        const bool running = state.status == GuestMsgDialog::Status::RUNNING;
        constexpr const char* title = "Message##GuestMsgDialog";
        if (running && (shown.lock() != dialog || request != state.request)) {
            shown = dialog;
            request = state.request;
            selected = state.focus_second ? 2 : 1;
            previous_buttons = 0;
            armed = false;
            ImGui::OpenPopup(title);
            popup = true;
        }
        if (!running && !popup.load())
            return;
        const auto& io = ImGui::GetIO();
        ImGui::SetNextWindowPos(ImGui::GetMainViewport()->GetCenter(), ImGuiCond_Always,
                                {0.5f, 0.5f});
        // Scale in display pixels, independently of the guest render resolution.
        // Keep the footer visible while long guest messages scroll above it.
        const float scale = std::clamp(std::min(io.DisplaySize.x / 1280.0f,
                                                io.DisplaySize.y / 720.0f), 0.75f, 2.0f);
        const float gap = 16.0f * scale;
        const float button_height = 56.0f * scale;
        ImGui::SetNextWindowSize({std::min(760.0f * scale, io.DisplaySize.x * 0.9f),
                                  std::min(300.0f * scale, io.DisplaySize.y * 0.85f)});
        ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, {24.0f * scale, 20.0f * scale});
        ImGui::PushStyleVar(ImGuiStyleVar_ItemSpacing, {gap, 12.0f * scale});
        ImGui::PushStyleVar(ImGuiStyleVar_WindowRounding, 12.0f * scale);
        ImGui::PushStyleVar(ImGuiStyleVar_FrameRounding, 6.0f * scale);
        if (ImGui::BeginPopupModal(title, nullptr,
                                   ImGuiWindowFlags_NoResize | ImGuiWindowFlags_NoMove |
                                       ImGuiWindowFlags_NoTitleBar | ImGuiWindowFlags_NoScrollbar |
                                       ImGuiWindowFlags_NoSavedSettings)) {
            const float base_font_size = ImGui::GetFontSize();
            ImGui::SetWindowFontScale(24.0f * scale / base_font_size);
            ImGui::TextUnformatted(state.title.c_str());
            ImGui::Separator();
            ImGui::SetWindowFontScale(20.0f * scale / base_font_size);
            if (!running) {
                ImGui::CloseCurrentPopup();
                popup = false;
            } else {
                const bool has_buttons = !state.first.empty() || !state.second.empty() || state.cancel;
                const float footer = has_buttons ? button_height + gap * 2.0f : 0.0f;
                const float progress_height = state.mode == 2 ? ImGui::GetFrameHeightWithSpacing() : 0.0f;
                const float body_height = std::max(1.0f, ImGui::GetContentRegionAvail().y - footer - progress_height);
                ImGui::BeginChild("##MessageBody", {0.0f, body_height}, ImGuiChildFlags_None);
                ImGui::TextWrapped("%s", state.text.c_str());
                ImGui::EndChild();
                if (state.mode == 2)
                    ImGui::ProgressBar(std::min(state.progress, 100u) / 100.0f, {-1.0f, 0.0f});
                if (has_buttons) ImGui::Separator();
                const auto pad = Libraries::Ime::ReadVirtualPadSnapshot(state.user, io.DeltaTime);
                const auto pressed = pad.buttons & ~previous_buttons;
                previous_buttons = pad.buttons;
                const bool confirm = ImGui::IsKeyDown(ImGuiKey_Enter) ||
                                     (pad.buttons & u32(Libraries::Ime::ImeConfirmButton()));
                const bool cancel = ImGui::IsKeyDown(ImGuiKey_Escape) ||
                                    (pad.buttons & u32(Libraries::Ime::ImeCancelButton()));
                if (!confirm && !cancel && !ImGui::IsMouseDown(0))
                    armed = true;
                if (!state.first.empty() &&
                    (ImGui::IsKeyPressed(ImGuiKey_LeftArrow) ||
                     (pressed & u32(Libraries::Pad::OrbisPadButtonDataOffset::Left))))
                    selected = 1;
                if ((!state.second.empty() || state.cancel) &&
                    (ImGui::IsKeyPressed(ImGuiKey_RightArrow) ||
                     (pressed & u32(Libraries::Pad::OrbisPadButtonDataOffset::Right))))
                    selected = state.second.empty() ? 0 : 2;
                u32 action = 3;
                const int button_count = int(!state.first.empty()) + int(!state.second.empty() || state.cancel);
                const float available_width = ImGui::GetContentRegionAvail().x;
                const float button_width = button_count ? std::min(200.0f * scale,
                    (available_width - gap * (button_count - 1)) / button_count) : 0.0f;
                if (button_count) ImGui::SetCursorPosX(ImGui::GetCursorPosX() +
                    (available_width - button_width * button_count - gap * (button_count - 1)) * 0.5f);
                auto button = [&](const std::string& label, u32 id) {
                    if (selected == id)
                        ImGui::PushStyleColor(ImGuiCol_Button,
                                              ImGui::GetStyleColorVec4(ImGuiCol_ButtonActive));
                    const bool clicked = ImGui::Button(label.c_str(), {button_width, button_height});
                    if (selected == id)
                        ImGui::PopStyleColor();
                    if (clicked)
                        action = id;
                };
                if (!state.first.empty())
                    button(state.first + "##first", 1);
                if (!state.second.empty()) {
                    if (!state.first.empty())
                        ImGui::SameLine();
                    button(state.second + "##second", 2);
                } else if (state.cancel) {
                    if (!state.first.empty()) ImGui::SameLine();
                    button("Cancel##cancel", 0);
                }
                if (armed && confirm)
                    action = selected;
                if (armed && cancel && state.cancel)
                    action = 0;
                if (armed && action <= 2 && dialog->Respond(state.request, action)) {
                    ImGui::CloseCurrentPopup();
                    popup = false;
                }
            }
            ImGui::SetWindowFontScale(1.0f);
            ImGui::EndPopup();
        } else if (!running)
            popup = false;
        ImGui::PopStyleVar(4);
    }

private:
    std::mutex mutex;
    std::vector<std::weak_ptr<GuestMsgDialog>> active;
    std::weak_ptr<GuestMsgDialog> shown;
    std::atomic<bool> popup{};
    u64 request{};
    u32 selected{1};
    u32 previous_buttons{};
    bool armed{};
};
MessageLayer layer;
std::once_flag registered;
} // namespace
void ShowGuestMsgDialog(const std::shared_ptr<GuestMsgDialog>& dialog) {
    layer.Set(dialog);
    std::call_once(registered, [] { ImGui::Layer::AddLayer(&layer); });
}
} // namespace Core::HostRuntime
