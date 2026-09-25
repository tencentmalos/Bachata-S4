// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once

#include <functional>
#include <mutex>
#include "frontend/window.h"

struct ANativeWindow;

namespace Frontend {
// A session keeps one Window while Android replaces its Surface on backgrounding.
// Swapchain holds a snapshot lease until the old VkSurface has retired.
class AndroidWindow final : public Window {
public:
    struct SurfaceSnapshot {
        std::shared_ptr<ANativeWindow> window;
        u64 epoch{};
    };
    // Capture requests run on the caller's thread. The app callback must post to
    // its UI thread and return whether the request was accepted.
    using KeyboardCapture = std::function<bool(bool)>;
    AndroidWindow(ANativeWindow* window, u64 generation, KeyboardCapture keyboard = {});
    ~AndroidWindow() override;
    AndroidWindow(const AndroidWindow&) = delete;
    AndroidWindow& operator=(const AndroidWindow&) = delete;
    s32 GetWidth() const override;
    s32 GetHeight() const override;
    WindowSystemInfo GetWindowInfo() const override;
    SurfaceSnapshot GetSurfaceSnapshot() const;
    void UpdateSurface(ANativeWindow* window);
    bool RequestKeyboard() override;
    void ReleaseKeyboard() override;
    u64 Generation() const {
        return generation;
    }

private:
    mutable std::mutex surface_mutex;
    std::shared_ptr<ANativeWindow> native_window;
    u64 surface_epoch{1};
    const u64 generation;
    KeyboardCapture keyboard;
    // Keyboard calls are serialized by the render/UI owner, not a global lock.
    unsigned keyboard_users{};
};
} // namespace Frontend
