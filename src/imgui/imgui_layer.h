// SPDX-FileCopyrightText: Copyright 2024 shadPS4 Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#pragma once

#include <mutex>

namespace ImGui {

class Layer {
public:
    virtual ~Layer() = default;
    // Register after initialization; unregister before destroying callback-visible state.
    // Removal waits for Draw/ShouldKeepDrawing on other threads. Callbacks may remove themselves.
    static void AddLayer(Layer* layer);
    static void RemoveLayer(Layer* layer);
    // For replacing registered state or taking an owner's mutex around registration/removal:
    // acquire this first, then the owner's mutex (the same order as callback dispatch).
    static std::unique_lock<std::recursive_mutex> LockRegistry();

    virtual void Draw() = 0;
    virtual bool ShouldKeepDrawing() {
        return true;
    }
};

} // namespace ImGui
