// SPDX-FileCopyrightText: Copyright 2026 shadPS4 Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#include <chrono>
#include <condition_variable>
#include <cstdlib>
#include <fstream>
#include <mutex>
#include <sstream>
#include <stdexcept>
#include <thread>
#if defined(__ANDROID__)
#include <sys/system_properties.h>
#endif

#include "common/elf_info.h"
#include "common/logging/log.h"
#include "core/diagnostics/diagnostics_service.h"
#include "core/emulator_settings.h"
#include "core/host_runtime/guest_gpu_replay.h"
#include "core/memory.h"
#include "video_core/amdgpu/liverpool.h"
#include "video_core/replay/gpu_replay_frames.h"
#include "video_core/replay/gpu_replay_hooks.h"
#include "video_core/replay/gpu_replay_player.h"

extern std::unique_ptr<AmdGpu::Liverpool> liverpool;

namespace Core::HostRuntime {

namespace {

std::string Property(const char* name) {
#if defined(__ANDROID__)
    char value[PROP_VALUE_MAX]{};
    if (__system_property_get(name, value) > 0) {
        return value;
    }
#else
    (void)name;
#endif
    return {};
}

} // namespace

bool GuestGpuReplay::IsTrace(const std::filesystem::path& path) {
    return path.extension() == ".sgpurply";
}

GuestGpuReplay::GuestGpuReplay(std::filesystem::path trace_)
    : trace(std::move(trace_)), player(std::make_unique<VideoCore::Replay::Player>()) {
    std::string error;
    if (!player->Open(trace, error)) {
        throw std::runtime_error("GPU replay: cannot open " + trace.string() + ": " + error);
    }
    const auto& header = player->Header();
    if (!header.direct_memory_size) {
        throw std::runtime_error("GPU replay: the trace does not record the memory sizes");
    }

    std::stringstream options{Property("debug.shadps4.gpu_replay_options")};
    float scale{};
    for (std::string option; std::getline(options, option, ',');) {
        if (option == "nopng") {
            png = false;
        } else if (option == "hash") {
            hash_images = true;
        } else if (option.starts_with("draws=")) {
            hash_images = true;
            hash_draws = std::strtoull(option.c_str() + 6, nullptr, 10);
        } else if (option.starts_with("scale=")) {
            scale = std::strtof(option.c_str() + 6, nullptr);
        } else if (!option.empty()) {
            LOG_WARNING(Render, "GPU replay: unknown option {}", option);
        }
    }
    debugbus = Property("debug.shadps4.gpu_replay_debugbus");
    report = Property("debug.shadps4.gpu_replay_report");

    // The settings the capture ran with, and the ones a replay fixes (as on desktop).
    EmulatorSettings.SetNeo(header.neo_mode != 0);
    EmulatorSettings.SetExtraDmemInMBytes(header.extra_dmem_mb);
    EmulatorSettings.SetExtraFmemInMBytes(header.extra_fmem_mb);
    EmulatorSettings.SetInternalScalePercent(scale ? scale : header.internal_scale_eighths * 12.5f);
    EmulatorSettings.SetReadbacksMode(
        static_cast<u32>(std::stoul(player->InfoValue("readbacks_mode", "0"))));
    EmulatorSettings.SetReadbackLinearImagesEnabled(
        player->InfoValue("readback_linear_images", "0") == "1");
    EmulatorSettings.SetCopyGpuBuffers(false);
    // A draw whose pipeline is not ready must not be skipped.
    EmulatorSettings.SetPipelineCompileMode("sync");
    LOG_INFO(Render, "GPU replay of {} (scale {}%, png {}, hashes {}), captured by {}",
             trace.string(), EmulatorSettings.GetInternalScalePercent(), png, hash_images,
             player->InfoValue("build", "unknown build"));
}

GuestGpuReplay::~GuestGpuReplay() {
    VideoCore::Replay::SetReplaying(false);
    VideoCore::Replay::DisableFrameDump();
}

void GuestGpuReplay::ConfigureMemory(MemoryManager& memory, Common::ElfInfo& elf) {
    const auto& header = player->Header();
    const std::string id{header.title_id.data(),
                         strnlen(header.title_id.data(), header.title_id.size())};
    elf.InitializeGuestMetadata({}, header.sdk_version, id, "GPU replay");
    memory.SetupReplayRegions(header.direct_memory_size, header.flexible_memory_size);
}

void GuestGpuReplay::RunCommands(std::string_view commands, std::ostream* out) {
    while (!commands.empty()) {
        const auto end = commands.find(';');
        const auto command = commands.substr(0, end);
        if (!command.empty()) {
            const auto result = Core::Diagnostics::HandleDebugCommand(command);
            LOG_INFO(Render, "GPU replay: DebugBus {}: {}", command, result);
            if (out) {
                *out << "> " << command << '\n' << result << '\n';
            }
        }
        commands = end == std::string_view::npos ? std::string_view{} : commands.substr(end + 1);
    }
}

GuestGpuReplay::Outcome GuestGpuReplay::Run(const std::atomic<bool>& cancelled,
                                            const std::function<void()>& check_health) {
    auto* gpu = liverpool.get();
    if (!gpu || !gpu->GetRasterizer()) {
        throw std::runtime_error("GPU replay: the session has no command processor");
    }
    std::string error;
    if (!player->RestoreInitialState(*gpu, *gpu->GetRasterizer(), error)) {
        throw std::runtime_error("GPU replay: cannot restore the initial state: " + error);
    }
    const auto output_dir = trace.parent_path() / (trace.stem().string() + "_replay");
    std::filesystem::create_directories(output_dir);
    VideoCore::Replay::EnableFrameDump(output_dir, png);
    if (hash_images) {
        player->EnableImageHashes(output_dir / "image_hashes.txt", hash_draws);
    }
    RunCommands(debugbus, nullptr);

    // A cancelled replay returns while the command processor may still finish the player: the
    // callback owns what it notifies.
    struct Wake {
        std::mutex mutex;
        std::condition_variable finished;
    };
    const auto wake = std::make_shared<Wake>();
    player->SetFinishCallback([wake] {
        std::scoped_lock lock{wake->mutex};
        wake->finished.notify_all();
    });
    if (!report.empty()) {
        // The same commands where the steady window starts, so per-frame deltas leave out the
        // first frames (debug.shadps4.gpu_replay_report_flip, default 8: a quarter of 30).
        const u64 report_flip = std::strtoull(
            Property("debug.shadps4.gpu_replay_report_flip").c_str(), nullptr, 10);
        player->SetFlipCallback([this, gpu, output_dir, at = report_flip ? report_flip : 8](u64 flip) {
            if (flip != at) {
                return;
            }
            gpu->SnapshotMemoryDiagnostics();
            std::ofstream out{output_dir / "replay_report_start.txt"};
            out << "flip=" << flip << '\n';
            RunCommands(report, &out);
        });
    }
    const auto begin = std::chrono::steady_clock::now();
    VideoCore::Replay::SetReplaying(true);
    gpu->StartReplay(player.get());
    {
        std::unique_lock lock{wake->mutex};
        while (!player->Done() && !cancelled.load(std::memory_order_acquire)) {
            lock.unlock();
            check_health();
            lock.lock();
            wake->finished.wait_for(lock, std::chrono::milliseconds{50});
        }
    }
    if (!player->Done()) {
        // Stopping the command processor ends the event loop; the player outlives it.
        LOG_INFO(Render, "GPU replay cancelled");
        return {.cancelled = true};
    }
    const auto seconds =
        std::chrono::duration<double>(std::chrono::steady_clock::now() - begin).count();
    // Frames still being read back when the replay ended finish within a few presents.
    for (int i = 0; i < 100 && VideoCore::Replay::FramesNoted() < VideoCore::Replay::FramesStarted();
         ++i) {
        std::this_thread::sleep_for(std::chrono::milliseconds{20});
    }
    auto summary = player->Summary();
    summary += "replay_seconds=" + std::to_string(seconds) + "\n";
    const auto frames = VideoCore::Replay::FrameList();
    std::ofstream{output_dir / "replay_summary.txt"} << summary;
    std::ofstream{output_dir / "frames.txt"} << frames;
    if (!report.empty()) {
        // No draw publishes allocation snapshots any more: take the final one for gpu_memory.
        gpu->SnapshotMemoryDiagnostics();
        std::ofstream out{output_dir / "replay_report.txt"};
        RunCommands(report, &out);
    }
    LOG_INFO(Render, "GPU replay summary ({}):\n{}frames:\n{}", output_dir.string(), summary,
             frames);
    return {.complete = summary.starts_with("result=complete"), .summary = std::move(summary)};
}

} // namespace Core::HostRuntime
