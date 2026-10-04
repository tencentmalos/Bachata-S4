// SPDX-FileCopyrightText: Copyright 2026 shadPS4 Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#pragma once

#include <filesystem>
#include <string>

#include "common/types.h"

/// Frame output of a GPU replay: every guest frame the presenter prepares is read back before
/// any host scaling, hashed, and optionally written as PNG. Off unless a replay enables it.
namespace VideoCore::Replay {

/// Enables the output; png also writes each frame as <dir>/frame_NNNNN.png.
void EnableFrameDump(std::filesystem::path dir, bool png);
bool FrameDumpEnabled();

/// Presenter, when it prepares a frame: the frame's index.
u32 NextFrameIndex();
/// The PNG path for a frame, or empty when frames are only hashed.
std::filesystem::path FramePath(u32 index);
/// Presenter, once the frame's readback completed: its XXH3 over RGBA8 pixels.
void NoteFrame(u32 index, u64 hash, u32 width, u32 height);

/// Frames whose readback started, and those whose hash is noted.
u32 FramesStarted();
u32 FramesNoted();
/// "index hash width height" per frame, in index order.
std::string FrameList();

} // namespace VideoCore::Replay
