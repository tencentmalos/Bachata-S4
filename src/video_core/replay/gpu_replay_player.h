// SPDX-FileCopyrightText: Copyright 2026 shadPS4 Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#pragma once

#include <algorithm>
#include <boost/icl/interval_set.hpp>
#include <atomic>
#include <memory>
#include <thread>
#include <deque>
#include <condition_variable>
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
    Player() = default;
    ~Player();
    Player(const Player&) = delete;
    Player& operator=(const Player&) = delete;

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
    /// Called on the command processor thread at each recorded flip (1-based count). Its CPU
    /// time counts as applying recorded writes, so it stays out of the command figures.
    void SetFlipCallback(std::function<void(u64)> callback) {
        on_flip = std::move(callback);
    }

    /// After every event, hashes the images the GPU wrote meanwhile into `path` (one line per
    /// image), so two replays can be compared event by event. Waits for the GPU each time.
    /// With draw_event, every draw and dispatch of that event is hashed the same way.
    void EnableImageHashes(const std::filesystem::path& path, u64 draw_event = ~u64{0});

    /// Also save image bytes for this inclusive event range (requires image hashes).
    void EnableImageDump(std::filesystem::path directory, u64 first, u64 last);

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
    /// Command processor time over the event stream: thread CPU (0 where not measured), wall.
    void NoteProcessorTime(u64 cpu_ns, u64 wall_ns) {
        processor_cpu_ns = cpu_ns;
        processor_wall_ns = wall_ns;
    }
    void Finish();

    bool Done() const {
        return done.load(std::memory_order_acquire);
    }
    std::string Summary() const;

private:
    /// Event stream: a reader thread and decompression workers run ahead of the command
    /// processor (bounded); NextRecord hands records out in order.
    void StartPrefetch();
    bool NextRecord(RecordHeader& record, std::vector<u8>& payload);

    bool ApplyAreas(PayloadReader& reader, bool keep_contents, std::string& error);
    bool WritePages(std::span<const u8> payload, bool initial, std::string& error);
    bool ApplyMapping(std::span<const u8> payload, std::string& error);

    TraceReader reader;
    FileHeader header{};
    std::map<std::string, std::string, std::less<>> info;
    AmdGpu::Liverpool* liverpool{};
    Vulkan::Rasterizer* rasterizer{};
    std::function<void()> on_finish;
    std::function<void(u64)> on_flip;
    std::optional<Event> pending;
    bool ended{};
    std::ofstream image_hashes;
    std::filesystem::path image_dump_directory;
    u64 image_dump_first{};
    u64 image_dump_last{~u64{0}};
    std::unordered_map<u64, u64> hashed_epochs;
    u64 draw_hash_event{~u64{0}};
    u64 current_event{};
    u32 draw_index{};
    std::atomic<bool> done{};
    u64 processor_cpu_ns{};
    std::vector<u64> frame_cpu_ns; ///< Command processor thread CPU at each recorded flip.
    u64 apply_cpu_ns{};              ///< CPU spent applying recorded guest writes and mappings.
    std::vector<u64> frame_apply_ns; ///< apply_cpu_ns at each recorded flip.

    struct Prefetched {
        RecordHeader header{};
        std::vector<u8> data;
        std::vector<u8> payload;
        bool ready{};
        bool ok{true};
    };
    std::mutex prefetch_mutex;
    std::condition_variable_any prefetch_cv;
    std::deque<std::shared_ptr<Prefetched>> prefetch_order;
    std::deque<std::shared_ptr<Prefetched>> prefetch_work;
    u64 prefetch_bytes{};
    bool prefetch_end{};
    std::string prefetch_error;
    std::vector<std::jthread> prefetch_threads;
    u64 processor_wall_ns{};
    /// Areas whose contents exist before the initial state (the driver objects).
    boost::icl::interval_set<VAddr> prefilled;

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
    /// Areas the trace holds that this process cannot map at their address; their pages are
    /// dropped. A GPU access there reads nothing.
    boost::icl::interval_set<VAddr> skipped;
    u64 skipped_bytes{};
    u64 skipped_pages{};
};

} // namespace VideoCore::Replay
