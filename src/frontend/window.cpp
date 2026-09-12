// SPDX-License-Identifier: GPL-2.0-or-later
#include <mutex>
#include "frontend/window.h"

namespace Frontend {
namespace {
std::mutex window_mutex;
std::shared_ptr<Window> current_window;
} // namespace

bool BindWindow(const std::shared_ptr<Window>& window) {
    if (!window)
        return false;
    std::scoped_lock lock{window_mutex};
    if (current_window)
        return false;
    current_window = window;
    return true;
}

bool UnbindWindow(const std::shared_ptr<Window>& expected) {
    std::shared_ptr<Window> retired;
    {
        std::scoped_lock lock{window_mutex};
        if (!expected || current_window != expected)
            return false;
        retired = std::move(current_window);
    }
    // Destructors/platform callbacks must not run under the publication lock.
    return true;
}

std::shared_ptr<Window> AcquireWindow() {
    std::scoped_lock lock{window_mutex};
    return current_window;
}
} // namespace Frontend
