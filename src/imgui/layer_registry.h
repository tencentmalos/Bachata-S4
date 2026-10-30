// SPDX-FileCopyrightText: Copyright 2026 shadPS4 Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#pragma once

#include <algorithm>
#include <memory>
#include <mutex>
#include <vector>
#include "imgui/imgui_layer.h"

namespace ImGui {

// Non-owning: owners must unregister before destroying callback-visible state. Removal is a
// barrier for callbacks on other threads, and is also allowed from inside a callback (including
// a callback that destroys itself). Never hold an owner's mutex while acquiring this lock.
class LayerRegistry {
public:
    auto Lock() {
        return std::unique_lock{mutex};
    }

    void Add(Layer* layer) {
        auto lock = Lock();
        if (!layer || std::ranges::any_of(layers, [layer](const auto& entry) {
                return entry->layer == layer;
            })) {
            return;
        }
        layers.push_back(std::make_shared<Entry>(Entry{layer}));
        redraw_pending = true;
    }

    void Remove(Layer* layer) {
        auto lock = Lock();
        std::erase_if(layers, [&](const auto& entry) {
            if (entry->layer != layer) {
                return false;
            }
            // Invalidate snapshots too. Re-registering at the same address creates a new entry,
            // so neither a removed layer nor its replacement is called by an old traversal.
            entry->layer = nullptr;
            redraw_pending = true;
            return true;
        });
    }

    void Draw() {
        auto lock = Lock();
        redraw_pending = false;
        const auto snapshot = layers;
        for (const auto& entry : snapshot) {
            if (auto* layer = entry->layer) {
                layer->Draw();
            }
        }
    }

    bool MustKeepDrawing() {
        auto lock = Lock();
        if (redraw_pending) {
            return true;
        }
        const auto snapshot = layers;
        for (const auto& entry : snapshot) {
            if (auto* layer = entry->layer; layer && layer->ShouldKeepDrawing()) {
                return true;
            }
        }
        return redraw_pending;
    }

private:
    struct Entry {
        Layer* layer;
    };
    std::recursive_mutex mutex;
    std::vector<std::shared_ptr<Entry>> layers;
    bool redraw_pending{};
};

} // namespace ImGui
