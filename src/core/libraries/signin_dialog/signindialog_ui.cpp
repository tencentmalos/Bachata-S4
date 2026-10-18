// SPDX-License-Identifier: GPL-2.0-or-later
#include <atomic>
#include <imgui.h>
#include "core/libraries/ime/ime_ui_shared.h"
#include "spatial/imgui/console/ConsoleUi.hpp"
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
            open.Restart();
            ImGui::OpenPopup(title);
            popup = true;
        }
        if (!running && !popup.load())
            return;
        const auto& io = ImGui::GetIO();
        namespace Console = spatial::imgui::console;
        const float scale = std::clamp(std::min(io.DisplaySize.x / 1280.0f,
                                                io.DisplaySize.y / 720.0f), 0.75f, 2.0f);
        const Console::Units u{scale};
        const float shown_fraction = open.Advance(io.DeltaTime);
        const ImVec2 center = ImGui::GetMainViewport()->GetCenter();
        ImGui::SetNextWindowPos({center.x, center.y + (1.0f - shown_fraction) * u(20)},
                                ImGuiCond_Always, {0.5f, 0.5f});
        ImGui::SetNextWindowSize({std::min(u(560), io.DisplaySize.x * 0.9f), 0.0f});
        const Console::ScopedStyle style{u};
        ImGui::PushStyleVar(ImGuiStyleVar_Alpha, std::max(shown_fraction, 0.02f));
        if (ImGui::BeginPopupModal(title, nullptr,
                                   ImGuiWindowFlags_AlwaysAutoResize | ImGuiWindowFlags_NoMove |
                                       ImGuiWindowFlags_NoTitleBar |
                                       ImGuiWindowFlags_NoSavedSettings |
                                       ImGuiWindowFlags_NoBackground)) {
            Console::WindowFrame(u);
            if (!running) {
                ImGui::CloseCurrentPopup();
                popup = false;
            } else {
                Console::Heading("PlayStation Network", u(26), Console::Icon::User);
                Console::Rule(u);
                const float base_font_size = ImGui::GetFontSize();
                ImGui::SetWindowFontScale(20.0f * scale / base_font_size);
                ImGui::PushStyleColor(ImGuiCol_Text, Console::Color::TextBody);
                ImGui::TextWrapped("PlayStation Network sign-in is unavailable.\n\n"
                                   "You are using a local profile. Close this dialog to return "
                                   "to the game without signing in.");
                ImGui::PopStyleColor();
                ImGui::Dummy({1.0f, u(8)});
                const auto pad = Ime::ReadVirtualPadSnapshot(snapshot.user, io.DeltaTime);
                const u32 buttons = static_cast<u32>(Ime::ImeConfirmButton()) |
                                    static_cast<u32>(Ime::ImeCancelButton());
                const bool key = ImGui::IsKeyDown(ImGuiKey_Enter) ||
                                 ImGui::IsKeyDown(ImGuiKey_Escape) || (pad.buttons & buttons);
                if (!key && !ImGui::IsMouseDown(0))
                    armed = true;
                const bool clicked = Console::Button(
                    "##ContinueOffline", Ime::ImeConfirmGlyph(), "Continue offline",
                    {ImGui::GetContentRegionAvail().x, u(48)}, true, u);
                if (armed && (clicked || key) && dialog->CompleteCancelled(snapshot.request)) {
                    ImGui::CloseCurrentPopup();
                    popup = false;
                }
                ImGui::SetWindowFontScale(1.0f);
            }
            ImGui::EndPopup();
        } else if (!running) {
            popup = false;
        }
        ImGui::PopStyleVar();
    }

private:
    std::mutex mutex;
    std::weak_ptr<Dialog> active, shown;
    std::atomic<bool> popup{};
    u64 generation{};
    bool armed{};
    spatial::imgui::console::OpenAnimation open;
};
SigninLayer layer;
std::once_flag registered;
} // namespace
void ShowDialog(const std::shared_ptr<Dialog>& dialog) {
    layer.Set(dialog);
    std::call_once(registered, [] { ImGui::Layer::AddLayer(&layer); });
}
} // namespace Libraries::SigninDialog
