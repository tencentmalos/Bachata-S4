// SPDX-FileCopyrightText: Copyright 2026 shadPS4 Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once

#include <atomic>
#include <filesystem>
#include <functional>
#include <iosfwd>
#include <memory>
#include <string>
#include <string_view>

namespace Common {
class ElfInfo;
}
namespace Core {
class MemoryManager;
}
namespace VideoCore::Replay {
class Player;
}

namespace Core::HostRuntime {

/// GPU replay inside an Android session (the desktop runs it from Emulator::RunGpuReplay): the
/// session prepares no game, rebuilds guest memory from a .sgpurply trace and lets the command
/// processor replay its events. Frames, summary and optional image hashes go next to the trace
/// in <stem>_replay/. Options (debug.shadps4.gpu_replay_options, comma separated): nopng, hash,
/// draws=<event>, scale=<percent>; debug.shadps4.gpu_replay_debugbus runs ';'-separated DebugBus
/// commands before the first event, debug.shadps4.gpu_replay_report others after the last one
/// (their output goes to replay_report.txt).
class GuestGpuReplay {
public:
    static bool IsTrace(const std::filesystem::path& path);

    /// Opens the trace and applies the settings it was captured with. Throws on failure.
    explicit GuestGpuReplay(std::filesystem::path trace);
    ~GuestGpuReplay();
    GuestGpuReplay(const GuestGpuReplay&) = delete;
    GuestGpuReplay& operator=(const GuestGpuReplay&) = delete;

    /// Before graphics exist: guest metadata and the memory sizes of the capture.
    void ConfigureMemory(MemoryManager& memory, Common::ElfInfo& elf);

    struct Outcome {
        bool cancelled{};
        bool complete{};
        std::string summary;
    };
    /// After the session created its graphics (Liverpool, rasterizer, presenter, VideoOut):
    /// restores the initial state, replays every event and writes the results. Polls
    /// `cancelled` and `check_health` (which throws on a GPU fault) while it waits.
    Outcome Run(const std::atomic<bool>& cancelled, const std::function<void()>& check_health);

private:
    static void RunCommands(std::string_view commands, std::ostream* out);

    std::filesystem::path trace;
    std::unique_ptr<VideoCore::Replay::Player> player;
    bool png{true};
    bool hash_images{};
    bool dump_images{};
    unsigned long long dump_first{};
    unsigned long long dump_last{~0ULL};
    unsigned long long hash_draws{~0ULL};
    std::string debugbus;
    std::string report;
};

} // namespace Core::HostRuntime
