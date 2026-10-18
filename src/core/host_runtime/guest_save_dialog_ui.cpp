// SPDX-License-Identifier: GPL-2.0-or-later
#include <algorithm>
#include <atomic>
#include <cfloat>
#include <cmath>
#include <ctime>
#include <fmt/format.h>
#include <imgui.h>
#include "core/libraries/ime/ime_ui_shared.h"
#include "core/libraries/save_data/dialog/savedatadialog_ui.h"
#include "guest_save_dialog.h"
#include "spatial/imgui/console/ConsoleUi.hpp"
#include "imgui/imgui_layer.h"
#include "spatial/imgui/console/PadGlyphs.hpp"

namespace Core::HostRuntime {
namespace {
using Libraries::Pad::OrbisPadButtonDataOffset;
using Libraries::SaveData::Dialog::DialogType;
using Libraries::SaveData::Dialog::SaveDataDialogMode;

constexpr u32 Bit(OrbisPadButtonDataOffset button) {
    return u32(button);
}

namespace Console = spatial::imgui::console;

std::string LocalTime(s64 seconds) {
    const std::time_t value = seconds;
    std::tm local{};
#ifdef _WIN32
    if (localtime_s(&local, &value) != 0)
        return {};
#else
    if (!localtime_r(&value, &local))
        return {};
#endif
    char text[32];
    return std::strftime(text, sizeof(text), "%Y-%m-%d %H:%M", &local) ? text : std::string{};
}

std::string Age(s64 seconds, s64 now) {
    const s64 age = now - seconds;
    if (age < 0)
        return {};
    if (age < 60)
        return "just now";
    if (age < 3600)
        return fmt::format("{} min ago", age / 60);
    if (age < 86400)
        return fmt::format("{} h ago", age / 3600);
    if (age < 7 * 86400)
        return fmt::format("{} d ago", age / 86400);
    return {};
}

// One permanent layer; session dialogs are weakly referenced and every response carries the
// request id it was drawn for. Controller (virtual pad, also fed by XR controllers), touch
// and, on desktop builds, the keyboard drive it; ImGui navigation is not used.
class SaveDialogLayer final : public ImGui::Layer {
public:
    void Set(const std::shared_ptr<GuestSaveDialog>& dialog) {
        std::lock_guard lock(mutex);
        std::erase_if(active, [&](const auto& weak) {
            const auto value = weak.lock();
            return !value || value == dialog;
        });
        active.push_back(dialog);
    }

    bool ShouldKeepDrawing() override {
        GuestSaveDialog::View view;
        return popup.load() || Current(view, false);
    }

    void Draw() override {
        GuestSaveDialog::View view;
        const auto dialog = Current(view, false);
        const bool running = dialog != nullptr;
        constexpr const char* id = "Saved Data##GuestSaveDialog";
        if (running && (shown.lock() != dialog || request != view.request)) {
            GuestSaveDialog::View full;
            if (!dialog->Snapshot(full, true))
                return;
            shown = dialog;
            request = full.request;
            rows = std::move(full.rows);
            selected = rows.empty() ? 0 : std::clamp(full.focus, 0, int(rows.size()) - 1);
            choice = 1;
            previous_buttons = 0;
            held = 0;
            armed = false;
            scroll_to_selected = true;
            frames = 0;
            focus_valid = false;
            open.Restart();
            ImGui::OpenPopup(id);
            popup = true;
        }
        if (!running && !popup.load())
            return;

        const auto& io = ImGui::GetIO();
        const float scale = std::clamp(std::min(io.DisplaySize.x / 1280.0f,
                                                io.DisplaySize.y / 720.0f), 0.75f, 2.0f);
        const Console::Units u{scale};
        const bool list = view.mode == u32(SaveDataDialogMode::LIST);
        const float width = list ? 920.0f : 760.0f;
        const float height = list ? 640.0f : (rows.empty() ? 300.0f : 420.0f);
        // Fades in while sliding up a little.
        const float shown_fraction = open.Advance(io.DeltaTime);
        const ImVec2 center = ImGui::GetMainViewport()->GetCenter();
        ImGui::SetNextWindowPos({center.x, center.y + (1.0f - shown_fraction) * u(20)},
                                ImGuiCond_Always, {0.5f, 0.5f});
        ImGui::SetNextWindowSize({std::min(width * scale, io.DisplaySize.x * 0.94f),
                                  std::min(height * scale, io.DisplaySize.y * 0.9f)});
        const Console::ScopedStyle style{u};
        ImGui::PushStyleVar(ImGuiStyleVar_Alpha, std::max(shown_fraction, 0.02f));
        if (ImGui::BeginPopupModal(id, nullptr,
                                   ImGuiWindowFlags_NoResize | ImGuiWindowFlags_NoMove |
                                       ImGuiWindowFlags_NoTitleBar | ImGuiWindowFlags_NoScrollbar |
                                       ImGuiWindowFlags_NoSavedSettings |
                                       ImGuiWindowFlags_NoBackground)) {
            Console::WindowFrame(u);
            if (!running) {
                ImGui::CloseCurrentPopup();
                popup = false;
            } else {
                DrawDialog(*dialog, view, u, list);
            }
            ImGui::SetWindowFontScale(1.0f);
            ImGui::EndPopup();
        } else if (!running) {
            popup = false;
        }
        ImGui::PopStyleVar();
    }

private:
    std::shared_ptr<GuestSaveDialog> Current(GuestSaveDialog::View& view, bool with_rows) {
        std::vector<std::weak_ptr<GuestSaveDialog>> snapshot;
        {
            std::lock_guard lock(mutex);
            snapshot = active;
        }
        // Never read a dialog under the layer lock: Open takes the reverse order.
        for (auto it = snapshot.rbegin(); it != snapshot.rend(); ++it)
            if (auto dialog = it->lock(); dialog && dialog->Snapshot(view, with_rows))
                return dialog;
        return {};
    }

    // Edge presses plus hold-to-repeat for the direction buttons.
    u32 Presses(u32 buttons, float delta) {
        u32 pressed = buttons & ~previous_buttons;
        previous_buttons = buttons;
        constexpr u32 repeatable = Bit(OrbisPadButtonDataOffset::Up) |
                                   Bit(OrbisPadButtonDataOffset::Down) |
                                   Bit(OrbisPadButtonDataOffset::Left) |
                                   Bit(OrbisPadButtonDataOffset::Right);
        const u32 dir = buttons & repeatable;
        if (dir != held) {
            held = dir;
            repeat_timer = 0.35f;
        } else if (dir) {
            repeat_timer -= delta;
            if (repeat_timer <= 0.0f) {
                pressed |= dir;
                repeat_timer = 0.07f;
            }
        }
        return pressed;
    }

    void DrawDialog(GuestSaveDialog& dialog, const GuestSaveDialog::View& view,
                    const Console::Units& u, bool list) {
        const auto& io = ImGui::GetIO();
        const float scale = u.s;
        ++frames;
        // The window font scale is 1 here (reset at the end of every frame).
        const float base = ImGui::GetFontSize();
        const auto font = [&](float size) { ImGui::SetWindowFontScale(size * scale / base); };

        // Input first, so this frame draws the moved cursor.
        auto pad = Libraries::Ime::ReadVirtualPadSnapshot(view.user, io.DeltaTime);
        // The left stick moves the cursor like the D-pad.
        if (pad.left_stick.y < -0.6f)
            pad.buttons |= Bit(OrbisPadButtonDataOffset::Up);
        if (pad.left_stick.y > 0.6f)
            pad.buttons |= Bit(OrbisPadButtonDataOffset::Down);
        const u32 pressed = Presses(pad.buttons, io.DeltaTime);
        const auto confirm_button = Libraries::Ime::ImeConfirmButton();
        const auto cancel_button = Libraries::Ime::ImeCancelButton();
        const bool confirm_down = ImGui::IsKeyDown(ImGuiKey_Enter) ||
                                  (pad.buttons & Bit(confirm_button));
        const bool cancel_down = ImGui::IsKeyDown(ImGuiKey_Escape) ||
                                 (pad.buttons & Bit(cancel_button));
        // The press that opened the dialog must not answer it.
        if (!confirm_down && !cancel_down && !ImGui::IsMouseDown(0))
            armed = true;
        const bool confirm = armed && ((pressed & Bit(confirm_button)) ||
                                       ImGui::IsKeyPressed(ImGuiKey_Enter, false));
        const bool cancel = armed && ((pressed & Bit(cancel_button)) ||
                                      ImGui::IsKeyPressed(ImGuiKey_Escape, false));

        // Buttons of message modes: 1 = first (OK/Yes), 2 = No, 0 = Cancel.
        std::vector<std::pair<std::string, int>> buttons;
        if (!list) {
            if (view.buttons == 0)
                buttons.push_back({"OK", 1});
            else if (view.buttons == 1)
                buttons = {{"Yes", 1}, {"No", 2}};
            else if (view.buttons == 3)
                buttons = {{"OK", 1}, {"Cancel", 0}};
            if (view.can_cancel && view.buttons != 3 && view.buttons != 1)
                buttons.push_back({"Cancel", 0});
        }

        int action = -1; // -1 none, else 0 cancel / 1 confirm / 2 no
        const int count = int(rows.size());
        if (list && count > 0) {
            const int page = std::max(1, visible_rows);
            int next = selected;
            if ((pressed & Bit(OrbisPadButtonDataOffset::Up)) || ImGui::IsKeyPressed(ImGuiKey_UpArrow))
                next = next > 0 ? next - 1 : count - 1;
            if ((pressed & Bit(OrbisPadButtonDataOffset::Down)) ||
                ImGui::IsKeyPressed(ImGuiKey_DownArrow))
                next = next + 1 < count ? next + 1 : 0;
            if ((pressed & Bit(OrbisPadButtonDataOffset::L1)) || ImGui::IsKeyPressed(ImGuiKey_PageUp))
                next = std::max(0, next - page);
            if ((pressed & Bit(OrbisPadButtonDataOffset::R1)) ||
                ImGui::IsKeyPressed(ImGuiKey_PageDown))
                next = std::min(count - 1, next + page);
            if (next != selected) {
                selected = next;
                scroll_to_selected = true;
            }
            if (confirm)
                action = 1;
        } else if (!buttons.empty()) {
            int index = 0;
            for (int i = 0; i < int(buttons.size()); ++i)
                if (buttons[i].second == choice)
                    index = i;
            if ((pressed & Bit(OrbisPadButtonDataOffset::Left)) ||
                ImGui::IsKeyPressed(ImGuiKey_LeftArrow))
                index = std::max(0, index - 1);
            if ((pressed & Bit(OrbisPadButtonDataOffset::Right)) ||
                ImGui::IsKeyPressed(ImGuiKey_RightArrow))
                index = std::min(int(buttons.size()) - 1, index + 1);
            choice = buttons[index].second;
            if (confirm)
                action = choice;
        }
        if (cancel && view.can_cancel)
            action = 0;

        // Header: title on the left, the entry count on the right.
        const char* heading =
            !list                                          ? "Saved Data"
            : view.disp_type == u32(DialogType::SAVE)      ? "Save"
            : view.disp_type == u32(DialogType::DELETE)    ? "Delete Saved Data"
                                                           : "Load Saved Data";
        const auto mode = SaveDataDialogMode(view.mode);
        const auto icon = mode == SaveDataDialogMode::ERROR_CODE     ? Console::Icon::Error
                          : mode == SaveDataDialogMode::PROGRESS_BAR ? Console::Icon::Progress
                          : !list && view.buttons == 1               ? Console::Icon::Question
                          : view.disp_type == u32(DialogType::SAVE)  ? Console::Icon::Save
                          : view.disp_type == u32(DialogType::DELETE) ? Console::Icon::Delete
                                                                      : Console::Icon::Load;
        const ImVec2 header = ImGui::GetCursorScreenPos();
        const float header_right = header.x + ImGui::GetContentRegionAvail().x;
        Console::Heading(heading, u(26), icon);
        const float header_height = ImGui::GetItemRectSize().y;
        if (list) {
            const s64 data_rows = std::ranges::count_if(rows, [](const auto& r) { return !r.is_new; });
            const auto info = fmt::format("{} saved data, newest first", data_rows);
            const float size = u(18);
            const ImVec2 extent = ImGui::GetFont()->CalcTextSizeA(size, FLT_MAX, 0.0f, info.c_str());
            ImGui::GetWindowDrawList()->AddText(
                ImGui::GetFont(), size,
                {header_right - extent.x, header.y + (header_height - extent.y) * 0.5f},
                Console::C(Console::Color::TextDim), info.c_str());
        }
        Console::Rule(u);

        const float footer = u(64);
        const float body = std::max(1.0f, ImGui::GetContentRegionAvail().y - footer);
        const s64 now = s64(std::time(nullptr));
        // Games put line breaks in the detail text (GoW3: difficulty, then play time): rows grow
        // to fit up to three detail lines, all by the same amount so the list stays regular.
        int detail_lines = 1;
        for (const auto& row : rows)
            detail_lines = std::max(detail_lines, std::min(3, int(std::ranges::count(
                                                                  SecondLine(row), '\n')) + 1));
        const float row_height = u(86) + u(24) * float(detail_lines - 1);
        if (list) {
            ImGui::BeginChild("##SaveRows", {0.0f, body}, ImGuiChildFlags_None);
            visible_rows = std::max(1, int(body / row_height) - 1);
            // Room above the first row for the focus glow.
            const float top = u(6);
            ImGui::Dummy({1.0f, top});
            ImDrawList* draw = ImGui::GetWindowDrawList();
            const ImVec2 start = ImGui::GetCursorScreenPos();
            const float row_width = ImGui::GetContentRegionAvail().x;
            // Rows on the upper channel, the highlight underneath them.
            draw->ChannelsSplit(2);
            draw->ChannelsSetCurrent(1);
            for (int i = 0; i < count; ++i) {
                if (DrawRow(rows[i], i, now, row_height, u) && armed) {
                    // Touch or click: the first tap selects, a tap on the selected row confirms.
                    if (selected == i)
                        action = 1;
                    selected = i;
                }
            }
            if (count == 0) {
                font(20.0f);
                ImGui::TextDisabled("There is no saved data.");
            }
            draw->ChannelsSetCurrent(0);
            if (count > 0)
                DrawHighlight(draw, start, row_width, row_height, u, io.DeltaTime);
            draw->ChannelsMerge();
            ScrollToSelected(top, row_height, io.DeltaTime);
            ImGui::EndChild();
        } else {
            ImGui::BeginChild("##SaveMessage", {0.0f, body}, ImGuiChildFlags_None);
            if (!rows.empty())
                DrawRow(rows[0], -1, now, row_height, u);
            font(20.0f);
            ImGui::PushStyleColor(ImGuiCol_Text, Console::Color::TextBody);
            ImGui::TextWrapped("%s", view.text.c_str());
            ImGui::PopStyleColor();
            if (view.mode == u32(SaveDataDialogMode::PROGRESS_BAR))
                Console::ProgressBar(std::min(view.progress, 100u) / 100.0f, u(10));
            ImGui::EndChild();
        }
        Console::Rule(u);

        // Footer: controller hints on the left, touch buttons on the right. Both carry the
        // controller glyphs so the buttons to press stand out at a glance.
        font(18.0f);
        const float button_height = u(44);
        const float glyph = u(26);
        const auto confirm_glyph = Libraries::Ime::PadButtonGlyph(confirm_button);
        const auto cancel_glyph = Libraries::Ime::PadButtonGlyph(cancel_button);
        const char* verb = view.disp_type == u32(DialogType::SAVE)     ? "Save"
                           : view.disp_type == u32(DialogType::DELETE) ? "Delete"
                                                                       : "Load";
        const float y = ImGui::GetCursorPosY() + (footer - button_height) * 0.5f - u(6);
        ImGui::SetCursorPosY(y + (button_height - std::max(glyph, ImGui::GetTextLineHeight())) * 0.5f);
        ImGui::PushStyleColor(ImGuiCol_Text, ImGui::GetStyleColorVec4(ImGuiCol_TextDisabled));
        if (list && count > 0) {
            Console::PadHint({confirm_glyph}, verb, glyph);
            Console::PadHint({Console::PadGlyph::DPadVertical}, "Move", glyph);
            Console::PadHint({Console::PadGlyph::L1, Console::PadGlyph::R1}, "Page", glyph);
        } else if (!buttons.empty()) {
            Console::PadHint({confirm_glyph}, "Select", glyph);
            Console::PadHint({Console::PadGlyph::DPadHorizontal}, "Choose", glyph);
        }
        if (view.can_cancel)
            Console::PadHint({cancel_glyph}, "Back", glyph);
        ImGui::PopStyleColor();
        ImGui::NewLine();

        std::vector<std::pair<std::string, int>> touch = buttons;
        if (list) {
            if (count > 0)
                touch.push_back({verb, 1});
            if (view.can_cancel)
                touch.push_back({"Cancel", 0});
        }
        const float button_width = u(150);
        float x = ImGui::GetWindowContentRegionMax().x -
                  (button_width + u(12)) * float(touch.size()) + u(12);
        for (const auto& [label, value] : touch) {
            ImGui::SetCursorPos({x, y});
            // Cancel carries the cancel button's glyph; the confirm glyph marks the answer the
            // confirm button gives (the focused one in message dialogs).
            const auto button_glyph = value == 0                     ? cancel_glyph
                                      : list || value == choice ? confirm_glyph
                                                                : Console::PadGlyph::None;
            if (Console::Button(fmt::format("##save_dialog_{}", value).c_str(), button_glyph,
                                label.c_str(), {button_width, button_height},
                                !list && value == choice, u) &&
                armed)
                action = value;
            x += button_width + u(12);
        }

        if (action < 0)
            return;
        const int selection = list ? selected : -1;
        if (list && action == 1 && (count == 0 || selection < 0))
            return;
        if (dialog.Respond(view.request, action, selection)) {
            ImGui::CloseCurrentPopup();
            popup = false;
        }
    }

    // The selection highlight: it glides to the selected row instead of jumping.
    void DrawHighlight(ImDrawList* draw, ImVec2 start, float width, float row_height,
                       const Console::Units& u, float dt) {
        const float target = float(selected) * row_height;
        // Snap when the dialog opens and on wrap-around jumps; glide otherwise.
        if (!focus_valid || std::abs(target - focus_y) > row_height * 3.0f) {
            focus_y = target;
            focus_valid = true;
        } else {
            focus_y = Console::Approach(focus_y, target, 22.0f, dt);
        }
        const float inset = u(8);
        Console::RowHighlight(draw, {start.x + inset, start.y + focus_y},
                              {start.x + width - inset, start.y + focus_y + row_height - u(6)},
                              u);
    }

    // Keeps the selected row in the middle of the list, scrolling smoothly.
    void ScrollToSelected(float top, float row_height, float dt) {
        if (!scroll_to_selected)
            return;
        const float target =
            std::clamp(top + float(selected) * row_height + row_height * 0.5f -
                           ImGui::GetWindowHeight() * 0.5f,
                       0.0f, ImGui::GetScrollMaxY());
        // The first frames jump: the list's size is only known after it was drawn once.
        if (frames <= 2) {
            ImGui::SetScrollY(target);
            return;
        }
        const float y = Console::Approach(ImGui::GetScrollY(), target, 16.0f, dt);
        const bool arrived = std::abs(y - target) < 0.5f;
        ImGui::SetScrollY(arrived ? target : y);
        if (arrived)
            scroll_to_selected = false;
    }

    // One saved data entry. Returns true when it was clicked.
    bool DrawRow(const GuestSaveDialog::Row& row, int index, s64 now, float height,
                 const Console::Units& u) {
        auto* draw = ImGui::GetWindowDrawList();
        const ImVec2 origin = ImGui::GetCursorScreenPos();
        const float width = ImGui::GetContentRegionAvail().x;
        bool clicked = false;
        if (index >= 0) {
            ImGui::PushID(index);
            clicked = ImGui::InvisibleButton("##row", {width, height});
            ImGui::PopID();
        } else {
            ImGui::Dummy({width, height});
        }
        const ImVec2 end{origin.x + width - u(8), origin.y + height - u(6)};
        const bool focused = index == selected;
        if (index < 0) {
            // The entry a message refers to: a plain card, there is nothing to select.
            draw->AddRectFilled({origin.x + u(8), origin.y}, end,
                                Console::C(IM_COL32(255, 255, 255, 14)), u(10));
        } else if (!focused && index + 1 < int(rows.size()) && index + 1 != selected) {
            // Thin rule between rows; the highlight replaces it around the selection.
            draw->AddLine({origin.x + u(24), origin.y + height - u(3)},
                          {end.x - u(16), origin.y + height - u(3)},
                          Console::C(IM_COL32(255, 255, 255, 22)), u.Hair());
        }

        auto* font = ImGui::GetFont();
        const float pad = u(28); // clear of the highlight accent bar
        const float title_size = u(22);
        const float detail_size = u(17);
        const ImU32 text = Console::C(Console::Color::Text);
        const ImU32 dim =
            Console::C(focused ? IM_COL32(190, 198, 210, 255) : Console::Color::TextDim);
        const ImVec2 first_line{origin.x + pad, origin.y + u(16)};
        const ImVec2 second_line{origin.x + pad, origin.y + u(16) + title_size + u(6)};
        if (row.is_new) {
            Console::Label(draw, title_size, first_line, Console::C(Console::Color::AccentLight),
                           ("+  " + row.title).c_str(), nullptr, focused);
            draw->AddText(font, detail_size, second_line, dim, "Create new saved data");
            return clicked;
        }
        // Date on the right: the newest saves are the ones a player usually wants.
        const auto date = LocalTime(row.mtime);
        const auto age = Age(row.mtime, now);
        const float date_width = font->CalcTextSizeA(title_size, FLT_MAX, 0.0f, date.c_str()).x;
        const float age_width = font->CalcTextSizeA(detail_size, FLT_MAX, 0.0f, age.c_str()).x;
        const float right = end.x - u(16);
        Console::Label(draw, title_size, {right - date_width, first_line.y}, text, date.c_str());
        draw->AddText(font, detail_size, {right - age_width, second_line.y}, dim, age.c_str());
        const float text_right = right - std::max(date_width, age_width) - pad;
        const ImVec4 clip{origin.x + pad, origin.y, text_right, end.y};
        Console::Label(draw, title_size, first_line, text, row.title.c_str(), &clip, focused);
        draw->AddText(font, detail_size, second_line, dim, SecondLine(row).c_str(), nullptr, 0.0f,
                      &clip);
        return clicked;
    }

    // Subtitle, detail and directory, as the game wrote them (the detail may span lines).
    static std::string SecondLine(const GuestSaveDialog::Row& row) {
        std::string second = row.subtitle.empty() ? row.detail : row.subtitle;
        if (!second.empty() && !row.detail.empty() && second != row.detail)
            second += "  |  " + row.detail;
        if (second.empty())
            second = row.directory;
        else
            second += "  |  " + row.directory;
        return second;
    }

    std::mutex mutex;
    std::vector<std::weak_ptr<GuestSaveDialog>> active;
    std::weak_ptr<GuestSaveDialog> shown;
    std::atomic<bool> popup{};
    u64 request{};
    std::vector<GuestSaveDialog::Row> rows;
    int selected{};
    int choice{1};
    int visible_rows{4};
    u32 previous_buttons{};
    u32 held{};
    float repeat_timer{};
    bool armed{};
    bool scroll_to_selected{};
    int frames{};
    float focus_y{};
    bool focus_valid{};
    Console::OpenAnimation open;
};
SaveDialogLayer save_dialog_layer;
std::once_flag save_dialog_registered;
} // namespace

void ShowGuestSaveDialog(const std::shared_ptr<GuestSaveDialog>& dialog) {
    save_dialog_layer.Set(dialog);
    std::call_once(save_dialog_registered,
                   [] { ImGui::Layer::AddLayer(&save_dialog_layer); });
}
} // namespace Core::HostRuntime
