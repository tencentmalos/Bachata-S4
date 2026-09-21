// SPDX-License-Identifier: GPL-2.0-or-later
#include <atomic>
#include <imgui.h>
#include "core/libraries/ime/ime_ui_shared.h"
#include "imgui/imgui_layer.h"
#include "signindialog.h"

namespace Libraries::SigninDialog {
namespace {
// Permanent layer identity: the renderer's deferred raw layer queue never
// contains a session object. Only a weak Dialog reference crosses sessions.
class SigninLayer final : public ImGui::Layer {
public:
    void Set(const std::shared_ptr<Dialog>& dialog) {
        std::lock_guard lock(mutex);
        active = dialog;
    }
    std::shared_ptr<Dialog> Get() {
        std::lock_guard lock(mutex);
        return active.lock();
    }
    bool ShouldKeepDrawing() override {
        auto dialog = Get();
        return popup.load() || (dialog && dialog->GetStatus() == Status::RUNNING);
    }
    void Draw() override {
        auto dialog = Get();
        auto snapshot = dialog ? dialog->Read() : Dialog::Snapshot{Status::NONE, 0, 0};
        const bool running = snapshot.status == Status::RUNNING;
        constexpr const char* title = "PlayStation Network##OfflineSignin";
        const bool fresh = running && (shown.lock() != dialog || generation != snapshot.request);
        if (fresh) {
            shown = dialog;
            generation = snapshot.request;
            armed = false; // require release after the press that opened the dialog
            ImGui::OpenPopup(title);
            popup = true;
        }
        if (!running && !popup.load())
            return;
        const auto& io = ImGui::GetIO();
        ImGui::SetNextWindowPos(ImGui::GetMainViewport()->GetCenter(), ImGuiCond_Always,
                                {0.5f, 0.5f});
        ImGui::SetNextWindowSize({std::min(520.0f, io.DisplaySize.x * 0.9f), 0.0f});
        if (ImGui::BeginPopupModal(title, nullptr,
                                   ImGuiWindowFlags_AlwaysAutoResize | ImGuiWindowFlags_NoMove |
                                       ImGuiWindowFlags_NoSavedSettings)) {
            if (!running) {
                ImGui::CloseCurrentPopup();
                popup = false;
            } else {
                ImGui::TextWrapped("PlayStation Network sign-in is unavailable.\n\n"
                                   "You are using a local profile. Close this dialog to return "
                                   "to the game without signing in.");
                ImGui::Spacing();
                const auto pad = Ime::ReadVirtualPadSnapshot(snapshot.user, io.DeltaTime);
                const u32 buttons = static_cast<u32>(Ime::ImeConfirmButton()) |
                                    static_cast<u32>(Ime::ImeCancelButton());
                const bool key = ImGui::IsKeyDown(ImGuiKey_Enter) ||
                                 ImGui::IsKeyDown(ImGuiKey_Escape) || (pad.buttons & buttons);
                if (!key && !ImGui::IsMouseDown(0))
                    armed = true;
                const bool clicked = ImGui::Button("Continue offline", {-1.0f, 40.0f});
                if (armed && (clicked || key) && dialog->CompleteCancelled(snapshot.request)) {
                    ImGui::CloseCurrentPopup();
                    popup = false;
                }
            }
            ImGui::EndPopup();
        } else if (!running) {
            popup = false;
        }
    }

private:
    std::mutex mutex;
    std::weak_ptr<Dialog> active, shown;
    std::atomic<bool> popup{};
    u64 generation{};
    bool armed{};
};
SigninLayer layer;
std::once_flag registered;
} // namespace
void ShowDialog(const std::shared_ptr<Dialog>& dialog) {
    layer.Set(dialog);
    std::call_once(registered, [] { ImGui::Layer::AddLayer(&layer); });
}
} // namespace Libraries::SigninDialog
