// SPDX-FileCopyrightText: Copyright 2026 shadPS4 Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#include <algorithm>
#include <atomic>
#include <mutex>
#include "common/logging/log.h"
#include "core/game_options.h"

namespace Core::GameOptions {
namespace {
std::atomic<u32> frame_limit{0};
std::mutex save_mutex;
SaveHandler save_handler;
} // namespace

u32 FrameLimit() {
    return frame_limit.load(std::memory_order_relaxed);
}

void SetFrameLimit(u32 fps) {
    if (std::find(FrameLimits.begin(), FrameLimits.end(), fps) == FrameLimits.end())
        fps = 0;
    frame_limit.store(fps, std::memory_order_relaxed);
}

std::string FrameLimitName(u32 fps) {
    return fps == 0 ? "off" : std::to_string(fps);
}

u32 ParseFrameLimit(const std::string& name) {
    for (const u32 fps : FrameLimits)
        if (fps != 0 && name == std::to_string(fps))
            return fps;
    return 0;
}

void SetSaveHandler(SaveHandler handler) {
    std::scoped_lock lock{save_mutex};
    save_handler = std::move(handler);
}

void Save(const std::string& key, const std::string& value) {
    std::scoped_lock lock{save_mutex};
    if (!save_handler) {
        LOG_INFO(Config, "Game option {}={} applies to this run only (no per-game store)", key,
                 value);
        return;
    }
    try {
        save_handler(key, value);
        LOG_INFO(Config, "Game option {}={} saved for this game", key, value);
    } catch (const std::exception& e) {
        LOG_WARNING(Config, "Cannot save game option {}={}: {}", key, value, e.what());
    }
}

} // namespace Core::GameOptions
