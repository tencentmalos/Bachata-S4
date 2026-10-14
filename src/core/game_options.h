// SPDX-FileCopyrightText: Copyright 2026 shadPS4 Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#pragma once

#include <array>
#include <functional>
#include <string>
#include "common/types.h"

// Settings the in-game Controls panel changes while a game runs. Each change is applied at once
// and saved for the running game: Android hands it to the app, which writes the game's profile
// (files/settings/games/<TITLE_ID>.json); the desktop writes CustomConfigs/<serial>.json. The
// keys and values are the Android profile's (`gpu.frame_limit` = "off" | "50" | ...).
namespace Core::GameOptions {

// Choices of the frame-rate limit, in guest flips per second; 0 is off (the game's own rate).
inline constexpr std::array<u32, 6> FrameLimits{0, 50, 45, 40, 30, 20};

// At most this many guest frames are presented a second; 0 = no limit. Read every vblank.
u32 FrameLimit();
void SetFrameLimit(u32 fps);
// "off" or the number, as stored in the profile. Text that is not one of FrameLimits reads as off.
std::string FrameLimitName(u32 fps);
u32 ParseFrameLimit(const std::string& name);

// Saves one change for the running game. Without a handler (tests, tools) changes stay in this
// run only. The handler may be called on the presenting thread; it must not block for long.
using SaveHandler = std::function<void(const std::string& key, const std::string& value)>;
void SetSaveHandler(SaveHandler handler);
void Save(const std::string& key, const std::string& value);

} // namespace Core::GameOptions
