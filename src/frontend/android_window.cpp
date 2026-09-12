// SPDX-License-Identifier: GPL-2.0-or-later
#include <limits>
#include <stdexcept>
#include <android/native_window.h>
#include "frontend/android_window.h"

namespace Frontend {
AndroidWindow::AndroidWindow(ANativeWindow* window, u64 generation_, KeyboardCapture keyboard_)
    : native_window{window}, generation{generation_}, keyboard{std::move(keyboard_)} {
    if (!native_window || !generation) {
        throw std::invalid_argument("AndroidWindow requires a Surface and nonzero generation");
    }
    ANativeWindow_acquire(native_window);
}

AndroidWindow::~AndroidWindow() {
    if (keyboard_users && keyboard) {
        try {
            keyboard(false);
        } catch (...) { /* Destruction must release the Surface. */
        }
    }
    ANativeWindow_release(native_window);
}

s32 AndroidWindow::GetWidth() const {
    return ANativeWindow_getWidth(native_window);
}
s32 AndroidWindow::GetHeight() const {
    return ANativeWindow_getHeight(native_window);
}
WindowSystemInfo AndroidWindow::GetWindowInfo() const {
    return {.render_surface = native_window, .type = WindowSystemType::Android};
}

bool AndroidWindow::RequestKeyboard() {
    if (!keyboard || keyboard_users == std::numeric_limits<unsigned>::max())
        return false;
    try {
        if (!keyboard_users && !keyboard(true))
            return false;
    } catch (...) {
        return false;
    }
    ++keyboard_users;
    return true;
}

void AndroidWindow::ReleaseKeyboard() {
    if (keyboard_users && --keyboard_users == 0) {
        try {
            keyboard(false);
        } catch (...) { /* UI failure cannot unwind widget destruction. */
        }
    }
}
} // namespace Frontend
