// SPDX-FileCopyrightText: Copyright 2026 shadPS4 Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#pragma once

#include <span>
#include <string>

#include "common/types.h"
#include "core/libraries/pad/pad.h"
#include "frontend/window.h"
#include "input/controller.h"

struct SDL_Window;
struct SDL_Gamepad;
union SDL_Event;

namespace Input {
class GameController;
}

namespace Frontend {

class WindowSDL : public Window {
    int keyboard_grab = 0;

public:
    explicit WindowSDL(s32 width, s32 height, Input::GameControllers* controllers,
                       std::string_view window_title);
    ~WindowSDL() override;

    s32 GetWidth() const override {
        return width;
    }

    s32 GetHeight() const override {
        return height;
    }

    bool IsOpen() const {
        return is_open;
    }

    [[nodiscard]] SDL_Window* GetSDLWindow() const override {
        return window;
    }

    WindowSystemInfo GetWindowInfo() const override {
        return window_info;
    }

    void SetIcon(std::span<const u8> png_data);

    void WaitEvent();
    void InitTimers();

    bool RequestKeyboard() override;
    void ReleaseKeyboard() override;

private:
    void OnResize();
    void OnKeyboardMouseInput(const SDL_Event* event);
    void OnGamepadEvent(const SDL_Event* event);

private:
    s32 width;
    s32 height;
    Input::GameControllers controllers{};
    WindowSystemInfo window_info{};
    SDL_Window* window{};
    bool is_shown{};
    bool is_open{true};
};

void SetWindowIcon(SDL_Window* window, const std::vector<u8>& png);
void SetDefaultWindowIcon(SDL_Window* window);

} // namespace Frontend
