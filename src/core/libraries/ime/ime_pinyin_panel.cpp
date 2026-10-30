// SPDX-License-Identifier: GPL-2.0-or-later
#include "ime_pinyin_panel.h"
#include <algorithm>
#include <filesystem>
#include <fstream>
#include <span>
#include <vector>
#include "common/path_util.h"

namespace Libraries::Ime {
std::span<const unsigned char> BundledPinyinDictionary();
namespace {
bool PrepareDictionary() {
    try {
        const auto dir = Common::FS::GetUserPath(Common::FS::PathType::UserDir) / "pinyin";
        std::filesystem::create_directories(dir);
        const auto file = dir / "dict-49aebad1.dat";
        const auto bytes = BundledPinyinDictionary();
        std::ifstream in(file, std::ios::binary);
        std::vector<unsigned char> previous((std::istreambuf_iterator<char>(in)), {});
        if (!std::equal(previous.begin(), previous.end(), bytes.begin(), bytes.end())) {
            const auto tmp = dir / "dict-49aebad1.tmp";
            std::ofstream out(tmp, std::ios::binary | std::ios::trunc);
            out.write(reinterpret_cast<const char*>(bytes.data()), bytes.size());
            out.close();
            if (!out) return false;
            std::filesystem::rename(tmp, file);
        }
        SetPinyinDictionary(file.string(), (dir / "user-lp64.dat").string());
        return true;
    } catch (...) { return false; }
}
}

void PinyinPanel::Draw(ImVec2 pos, ImVec2 size, bool allowed, bool activate, int column, int columns) {
    if (!initialized) { initialized = true; PrepareDictionary(); }
    const bool available = allowed && composer.available();
    if (!allowed) { enabled = false; Reset(); }
    constexpr int per_page = 4;
    const int count = static_cast<int>(composer.candidateCount());
    page = std::clamp(page, 0, std::max(0, (count - 1) / per_page));
    const float top = size.y * 0.42f;
    ImGui::SetCursorScreenPos(pos);
    ImGui::BeginDisabled(!available);
    const int selected = column < 0 ? -1 : std::clamp(column * 7 / std::max(columns, 1), 0, 6);
    if (selected == 0) ImGui::PushStyleColor(ImGuiCol_Button, ImGui::GetStyleColorVec4(ImGuiCol_ButtonHovered));
    if (ImGui::Button(enabled ? "中 / EN" : "EN / 中", {size.x * 0.2f, top}) || (activate && selected == 0 && available)) {
        enabled = !enabled; Reset();
    }
    if (selected == 0) ImGui::PopStyleColor();
    ImGui::EndDisabled();
    auto* draw = ImGui::GetWindowDrawList();
    draw->PushClipRect(pos, {pos.x + size.x, pos.y + size.y}, true);
    const std::string prompt = !available ? "拼音不可用" : enabled ?
        (composer.composing() ? composer.preedit() : "输入拼音 · 空格选词") : "English";
    draw->AddText({pos.x + size.x * 0.22f, pos.y}, ImGui::GetColorU32(ImGuiCol_Text), prompt.c_str());
    draw->PopClipRect();
    if (!enabled) return;
    ImGui::PushID("pinyin-candidates");
    const float cell = size.x / 6;
    for (int i = 0; i < 6; ++i) {
        ImGui::SetCursorScreenPos({pos.x + cell * i, pos.y + top});
        std::string label;
        const int candidate = page * per_page + i - 1;
        if (i == 0) label = "<";
        else if (i == 5) label = ">";
        else if (candidate < count) label = composer.candidate(candidate);
        ImGui::PushID(i);
        const bool focused = selected == i + 1;
        if (focused) ImGui::PushStyleColor(ImGuiCol_Button, ImGui::GetStyleColorVec4(ImGuiCol_ButtonHovered));
        const bool chosen = ImGui::Button(label.empty() ? "##empty" : label.c_str(), {cell, size.y - top}) ||
            (activate && selected == i + 1);
        if (focused) ImGui::PopStyleColor();
        if (chosen) {
            if (i == 0) page = std::max(0, page - 1);
            else if (i == 5) page = std::min(std::max(0, (count - 1) / per_page), page + 1);
            else if (candidate < count) { pending = composer.choose(candidate); page = 0; }
        }
        ImGui::PopID();
    }
    ImGui::PopID();
}

bool PinyinPanel::Letter(const char* label) {
    if (!enabled || !label || !label[0] || label[1]) return false;
    const char c = label[0] >= 'A' && label[0] <= 'Z' ? label[0] + ('a' - 'A') : label[0];
    if (c < 'a' || c > 'z') return false;
    composer.addLetter(c); page = 0; return true;
}
bool PinyinPanel::Backspace() { page = 0; return enabled && composer.backspace(); }
std::string PinyinPanel::Commit() {
    page = 0;
    return composer.candidateCount() ? composer.choose(0) : composer.commitLetters();
}
void PinyinPanel::Reset() { composer.reset(); page = 0; pending.clear(); }
} // namespace Libraries::Ime
