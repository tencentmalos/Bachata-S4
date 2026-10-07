// SPDX-FileCopyrightText: Copyright 2026 shadPS4 Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#include <atomic>
#include <map>
#include <mutex>

#include <fmt/format.h>

#include "video_core/replay/gpu_replay_frames.h"

namespace VideoCore::Replay {

namespace {

struct FrameInfo {
    u64 hash;
    u32 width;
    u32 height;
};

std::atomic<bool> enabled{};
std::atomic<u32> next_index{};
std::mutex mutex;
std::filesystem::path directory;
bool write_png{};
std::map<u32, FrameInfo> frames;

} // namespace

void EnableFrameDump(std::filesystem::path dir, bool png) {
    std::scoped_lock lock{mutex};
    directory = std::move(dir);
    write_png = png;
    frames.clear();
    next_index = 0;
    enabled = true;
}

void DisableFrameDump() {
    enabled = false;
}

bool FrameDumpEnabled() {
    return enabled.load(std::memory_order_relaxed);
}

u32 NextFrameIndex() {
    return next_index.fetch_add(1, std::memory_order_relaxed);
}

std::filesystem::path FramePath(u32 index) {
    std::scoped_lock lock{mutex};
    if (!write_png) {
        return {};
    }
    return directory / fmt::format("frame_{:05}.png", index);
}

void NoteFrame(u32 index, u64 hash, u32 width, u32 height) {
    std::scoped_lock lock{mutex};
    frames[index] = {hash, width, height};
}

u32 FramesStarted() {
    return next_index.load(std::memory_order_relaxed);
}

u32 FramesNoted() {
    std::scoped_lock lock{mutex};
    return static_cast<u32>(frames.size());
}

std::string FrameList() {
    std::scoped_lock lock{mutex};
    std::string out;
    for (const auto& [index, frame] : frames) {
        out += fmt::format("{} {:016x} {} {}\n", index, frame.hash, frame.width, frame.height);
    }
    return out;
}

} // namespace VideoCore::Replay
