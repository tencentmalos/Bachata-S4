// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once

#include <functional>
#include "frontend/window.h"

struct ANativeWindow;

namespace Frontend {
// One immutable Surface identity. Construct a new adapter for a replacement
// Surface; the old reference survives until its renderer has retired.
class AndroidWindow final : public Window {
public:
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
    bool RequestKeyboard() override;
    void ReleaseKeyboard() override;
    u64 Generation() const {
        return generation;
    }

private:
    ANativeWindow* const native_window;
    const u64 generation;
    KeyboardCapture keyboard;
    // Keyboard calls are serialized by the render/UI owner, not a global lock.
    unsigned keyboard_users{};
};
} // namespace Frontend
