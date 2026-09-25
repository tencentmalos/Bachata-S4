// SPDX-License-Identifier: GPL-2.0-or-later
#include <limits>
#include <stdexcept>
#include <android/native_window.h>
#include "frontend/android_window.h"

namespace Frontend {
AndroidWindow::AndroidWindow(ANativeWindow* window, u64 generation_, KeyboardCapture keyboard_)
    : generation{generation_}, keyboard{std::move(keyboard_)} {
    if (!window || !generation) {
        throw std::invalid_argument("AndroidWindow requires a Surface and nonzero generation");
    }
    ANativeWindow_acquire(window);
    native_window = std::shared_ptr<ANativeWindow>(window, ANativeWindow_release);
}

AndroidWindow::~AndroidWindow() {
    if (keyboard_users && keyboard) {
        try {
            keyboard(false);
        } catch (...) { /* Destruction must release the Surface. */
        }
    }
}

s32 AndroidWindow::GetWidth() const {
    const auto surface = GetSurfaceSnapshot().window;
    return surface ? ANativeWindow_getWidth(surface.get()) : 0;
}
s32 AndroidWindow::GetHeight() const {
    const auto surface = GetSurfaceSnapshot().window;
    return surface ? ANativeWindow_getHeight(surface.get()) : 0;
}
WindowSystemInfo AndroidWindow::GetWindowInfo() const {
    const auto surface = GetSurfaceSnapshot().window;
    return {.render_surface = surface.get(), .type = WindowSystemType::Android};
}

AndroidWindow::SurfaceSnapshot AndroidWindow::GetSurfaceSnapshot() const {
    std::scoped_lock lock(surface_mutex);
    return {native_window, surface_epoch};
}

void AndroidWindow::UpdateSurface(ANativeWindow* window) {
    std::scoped_lock lock(surface_mutex);
    if (native_window.get() == window) return;
    if (window) ANativeWindow_acquire(window);
    if (window) native_window = std::shared_ptr<ANativeWindow>(window, ANativeWindow_release);
    else native_window.reset();
    ++surface_epoch;
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
