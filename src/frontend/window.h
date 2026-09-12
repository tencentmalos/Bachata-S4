// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once

#include <memory>
#include "common/types.h"

struct SDL_Window;

namespace Frontend {

enum class WindowSystemType : u8 { Headless, Windows, X11, Wayland, Metal, Android };

struct WindowSystemInfo {
    void* display_connection = nullptr;
    void* render_surface = nullptr;
    float render_surface_scale = 1.0f;
    WindowSystemType type = WindowSystemType::Headless;
};

// Platform window owned by a session. A presenter retains a shared reference;
// withdrawing the published window does not invalidate an in-flight renderer.
class Window {
public:
    virtual ~Window() = default;
    virtual s32 GetWidth() const = 0;
    virtual s32 GetHeight() const = 0;
    virtual WindowSystemInfo GetWindowInfo() const = 0;
    // Only the SDL frontend supplies this optional platform handle.
    virtual SDL_Window* GetSDLWindow() const {
        return nullptr;
    }
    virtual bool RequestKeyboard() = 0;
    virtual void ReleaseKeyboard() = 0;
};

// Bind before HLE/renderer initialization. Unbind after stopping input admission;
// the session must still drain all workers before starting another session.
// Expected identity prevents a late unbind from withdrawing a newer window.
bool BindWindow(const std::shared_ptr<Window>& window);
bool UnbindWindow(const std::shared_ptr<Window>& expected);
std::shared_ptr<Window> AcquireWindow();

} // namespace Frontend
