// SPDX-FileCopyrightText: Copyright 2026 shadPS4 Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#pragma once

#include <algorithm>
#include <atomic>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <functional>
#include <map>
#include <mutex>
#include <optional>
#include <span>
#include <string>
#include <unordered_map>
#include <vector>

#include "common/types.h"
#include "video_core/replay/gpu_replay_file.h"
#include "video_core/replay/gpu_replay_format.h"

namespace AmdGpu {
struct Liverpool;
}

namespace Vulkan {
class Rasterizer;
}

namespace VideoCore::Replay {

/// Replays a trace written by the Recorder (docs/specs/gpu-replay-20261004.md, section 2.5):
/// the guest memory and command processor state at capture start, then the recorded events in
/// order. The command processor resumes a queue only when the trace does, and a wait packet sees
/// the guest writes recorded before it and takes the outcome the capture recorded.
class Player {
public:
    struct Event {
        RecordType type{};
        std::vector<u8> payload;

        template <TriviallySerializable T>
        T As() const {
            T value{};
            std::memcpy(&value, payload.data(), std::min(sizeof(T), payload.size()));
            return value;
        }
    };

    /// Reads the header and the capture's description.
    bool Open(const std::filesystem::path& path, std::string& error);
    const FileHeader& Header() const {
        return header;
    }
    /// A value from the capture's description.
    std::string InfoValue(std::string_view key, std::string_view fallback = {}) const;

    /// Main thread, once the GPU stack exists and before the command processor replays: maps
    /// the recorded areas at their addresses and physical backing, writes their pages, and
    /// restores the command processor, VideoOut and GDS.
    bool RestoreInitialState(AmdGpu::Liverpool& liverpool, Vulkan::Rasterizer& rasterizer,
                             std::string& error);

    /// Called on the command processor thread when the replay ends.
    void SetFinishCallback(std::function<void()> callback) {
        on_finish = std::move(callback);
    }

    /// After every event, hashes the images the GPU wrote meanwhile into `path` (one line per
    /// image), so two replays can be compared event by event. Waits for the GPU each time.
    /// With draw_event, every draw and dispatch of that event is hashed the same way.
    void EnableImageHashes(const std::filesystem::path& path, u64 draw_event = ~u64{0});

    // Command processor thread.

    /// The next control event (Submit, Resume, WaitPoll, Command, BurstEnd, End). Memory writes,
    /// mapping changes and armed flips recorded before it are applied on the way. Null once the
    /// trace ended or the replay failed.
    const Event* Peek();
    void Pop();
    /// A wait packet's condition, evaluated after Peek applied the writes recorded before it.
    /// Returns the outcome the capture recorded.
    bool Poll(u32 queue, WaitKind kind, u64 address, bool satisfied);
    void RunCommand(const CommandRecord& command);
    /// The command processor is about to run an event / finished it.
    void BeforeEvent(const Event& event);
    void AfterEvent(const Event& event);
    /// A draw or dispatch was recorded while its event's draws are hashed.
    void AfterDraw(const char* kind, u64 hash0, u64 hash1);
    /// Stops replaying; the queues continue without the trace.
    void Fail(std::string reason);
    void Finish();

    bool Done() const {
        return done.load(std::memory_order_acquire);
    }
    std::string Summary() const;

private:
    bool ApplyAreas(PayloadReader& reader, bool keep_contents, std::string& error);
    bool WritePages(std::span<const u8> payload, bool initial, std::string& error);
    bool ApplyMapping(std::span<const u8> payload, std::string& error);

    TraceReader reader;
    FileHeader header{};
    std::map<std::string, std::string, std::less<>> info;
    AmdGpu::Liverpool* liverpool{};
    Vulkan::Rasterizer* rasterizer{};
    std::function<void()> on_finish;
    std::optional<Event> pending;
    bool ended{};
    std::ofstream image_hashes;
    std::unordered_map<u64, u64> hashed_epochs;
    u64 draw_hash_event{~u64{0}};
    u64 current_event{};
    u32 draw_index{};
    std::atomic<bool> done{};

    mutable std::mutex mutex;
    std::string failure;
    std::string first_divergence;
    u64 events{};
    u64 polls{};
    u64 forced_waits{};
    u64 divergences{};
    u64 recorded_flips{};
    u64 delta_records{};
    u64 delta_pages{};
    u64 mappings{};
    u64 commands{};
};

} // namespace VideoCore::Replay
