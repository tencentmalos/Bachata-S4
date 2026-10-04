// SPDX-FileCopyrightText: Copyright 2026 shadPS4 Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#pragma once

#include <filesystem>
#include <optional>
#include <thread>

#include "common/singleton.h"
#include "core/diagnostics/diagnostics_service.h"
#include "core/linker.h"
#include "input/controller.h"
#include "sdl_window.h"

namespace Core {

using HLEInitDef = void (*)(Core::Loader::SymbolsResolver* sym);

/// Options of a GPU replay run (docs/specs/gpu-replay-20261004.md, section 2.5).
struct GpuReplayOptions {
    std::filesystem::path trace;
    /// Frame hashes, PNGs and the summary go here; empty: next to the trace.
    std::filesystem::path output_dir;
    bool png = true;
    /// Close the window once the trace is replayed.
    bool exit_when_done = false;
    /// Hash the images the GPU writes after every event (image_hashes.txt).
    bool hash_images = false;
    /// With hash_images: also after every draw and dispatch of this event.
    u64 hash_draws_event = ~u64{0};
};

struct SysModules {
    std::string_view module_name;
    HLEInitDef callback;
};

class Emulator {
public:
    Emulator();
    ~Emulator();

    void Run(std::filesystem::path file, std::vector<std::string> args = {},
             std::optional<std::filesystem::path> game_folder = {},
             std::vector<std::pair<std::filesystem::path, std::string>> mounts = {},
             std::vector<std::string> const& env_vars = {}, bool append_log = false);
    void UpdatePlayTime(const std::string_view serial);
    void Shutdown();

    /// Replays a GPU trace instead of running a game: no guest code runs, the command
    /// processor executes the recorded submissions against the recorded guest memory.
    void RunGpuReplay(const GpuReplayOptions& options);

    /**
     * This will kill the current process and launch a new process with the same configuration
     * (using CLI args) but replacing the eboot image and guest arguments
     */
    void Restart(std::filesystem::path eboot_path, const std::vector<std::string>& guest_args = {});

    /**
     * Launches a new emulator process with the supplied CLI arguments, then terminates this
     * process. The new process waits for this one to exit before initializing.
     */
    [[noreturn]] void Relaunch(std::vector<std::string> args);

    const char* executableName;
    bool waitForDebuggerBeforeRun{false};
    /// Loopback port of the DebugBus TCP service (0 = any free port); nullopt disables it.
    std::optional<u16> debugBusPort{Core::Diagnostics::DefaultDebugBusPort};

private:
    void LoadSystemModules(const std::string& game_serial);

    Core::MemoryManager* memory;
    Input::GameControllers* controllers;
    Core::Linker* linker;
    std::shared_ptr<Frontend::WindowSDL> window;
    std::chrono::steady_clock::time_point start_time;
    std::jthread play_time_thread;
};

} // namespace Core
