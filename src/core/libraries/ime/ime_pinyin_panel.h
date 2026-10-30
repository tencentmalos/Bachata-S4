// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once
#include <string>
#include <imgui.h>
#include "pinyin_composer.h"

namespace Libraries::Ime {
// Shared by sceIme and sceImeDialog. Composition is separate from the guest text buffer.
struct PinyinPanel {
    PinyinComposer composer;
    bool enabled = false;
    bool initialized = false;
    int page = 0;
    std::string pending;

    void Draw(ImVec2 pos, ImVec2 size, bool allowed, bool activate, int column, int columns);
    bool Letter(const char* label);
    bool Backspace();
    std::string Commit();
    void Reset();
};
} // namespace Libraries::Ime
