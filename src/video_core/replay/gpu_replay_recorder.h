// SPDX-FileCopyrightText: Copyright 2026 shadPS4 Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#pragma once

#include <array>
#include <atomic>
#include <filesystem>
#include <memory>
#include <mutex>
#include <span>
#include <string>
#include <string_view>
#include <unordered_map>
#include <vector>

#include <boost/icl/interval_set.hpp>

#include "common/types.h"
#include "video_core/replay/gpu_replay_format.h"
#include "video_core/replay/gpu_replay_hooks.h"

namespace AmdGpu {
struct Liverpool;
}

namespace Vulkan {
class Rasterizer;
}

namespace VideoCore::Replay {

class TraceWriter;

/// Captures a deterministic GPU replay trace (see docs/specs/gpu-replay-20261004.md).
///
/// Arm, Status and Cancel run on any thread (the DebugBus network thread). The capture starts
/// at the end of a frame's processing burst and then records, on the command processor thread,
/// an event at every point where the guest CPU can affect what the GPU does: before a queue is
/// resumed, after a wait condition is evaluated, before injected commands run, and at the end
/// of a burst. Each event is preceded by the guest memory written since the previous one.
class Recorder {
public:
    static Recorder& Instance();

    /// Arms a capture of `frames` guest frames, starting at the next frame boundary. The trace
    /// goes to CapturesDir/gpu_replay/<name>.sgpurply; an existing file is refused.
    std::string Arm(u32 frames, std::string name);
    std::string Status();
    std::string Cancel();
    /// Diagnostic: runs one step of a capture start at the next frame end without capturing
    /// (buffers | images | protect -- protect write-protects like a capture for one frame).
    std::string Probe(std::string_view step);

    /// Command processor thread: the queues ran out of work. submit_done is set when the burst
    /// ended a frame (it saw the guest's SubmitDone), so no command buffer is in progress and
    /// the guest waits at the submission gate.
    void OnBurstEnd(AmdGpu::Liverpool& liverpool, Vulkan::Rasterizer* rasterizer,
                    bool submit_done) {
        if (state.load(std::memory_order_acquire) != State::Idle ||
            probe.load(std::memory_order_acquire) != 0) [[unlikely]] {
            BurstEnd(liverpool, rasterizer, submit_done);
        }
    }

    // Command processor thread, only while CaptureHooksActive().

    /// Before a queue's front task resumes; first_submit is set on the task's first resume.
    void OnResume(u32 queue, u64 submission, const SubmitRecord* first_submit);
    /// After a wait packet's condition was evaluated.
    void OnWaitPoll(u32 queue, WaitKind kind, u64 address, bool satisfied);
    /// Before the commands other threads queued for the command processor run.
    void OnCommands();
    /// A queued command started.
    void OnCommand(const CommandRecord& command);
    /// A guest frame was handed to the presenter.
    void OnFlip(s32 index, u64 address, bool is_eop);

    // Any thread (see gpu_replay_hooks.h).
    void NoteGuestStack(VAddr base, u64 size);
    void NoteMappingChange(VAddr base, u64 size, bool protect_only);
    void NoteEopFlipArmed(s32 handle, s32 index, s64 flip_arg);
    void NoteSubmitContents(u64 submission, std::span<const u32> dcb, std::span<const u32> ccb);

private:
    enum class State : u32 { Idle, Armed, Capturing, Finished, Failed };

    using IntervalSet = boost::icl::interval_set<VAddr>;

    struct PendingMapping {
        VAddr base;
        u64 size;
        bool protect_only;
    };

    void BurstEnd(AmdGpu::Liverpool& liverpool, Vulkan::Rasterizer* rasterizer, bool submit_done);
    bool Start(AmdGpu::Liverpool& liverpool, Vulkan::Rasterizer* rasterizer, std::string& error);
    void WriteInitialMemory();
    /// A boundary: writes the mapping changes, the guest memory written since the previous
    /// boundary, and the flips armed meanwhile.
    void Flush();
    /// Applies one mapping change under track_mutex; adds what must be copied whole to `whole`.
    void ApplyMappingChange(const PendingMapping& change, IntervalSet& whole);
    void Emit(RecordType type, std::vector<u8> payload);
    template <typename T>
    void EmitRecord(RecordType type, const T& record);
    /// Stops on a pending cancel; true when it did.
    bool CheckCancel();
    std::string StatsText();
    void Stop(bool ok, std::string reason);

    std::atomic<State> state{State::Idle};
    std::atomic<bool> cancel_requested{};
    /// Probe step pending at the next frame end (ProbeStep).
    std::atomic<u32> probe{};
    void RunProbe(u32 step);
    void ProtectTracked();
    void ReleaseTracking();

    // Description and results, under mutex.
    std::mutex mutex;
    std::string name;
    std::filesystem::path path;
    std::string reason;
    u32 frames_requested{};
    u32 deferrals{};
    u64 writeback_ms{};
    u64 snapshot_ms{};
    u64 vma_count{};
    u64 data_pages{};
    u64 zero_pages{};
    u64 raw_bytes{};
    u64 stored_bytes{};

    // Event statistics, written by the command processor thread.
    std::array<std::atomic<u64>, 32> event_counts{};
    std::atomic<u64> events{};
    std::atomic<u64> frames_seen{};
    std::atomic<u64> flushes{};
    std::atomic<u64> flush_ns{};
    std::atomic<u64> delta_pages{};
    std::atomic<u64> delta_zero_pages{};
    std::atomic<u64> whole_bytes{};
    std::atomic<u64> mapping_changes{};
    std::atomic<u64> excluded_stacks{};
    /// Recorded writable bytes the backend would not write-protect (executable or unmapped).
    std::atomic<u64> untracked_bytes{};
    /// steady_clock nanoseconds at BeginStream, and the capture's length once it stopped.
    std::atomic<u64> stream_begin_ns{};
    std::atomic<u64> stream_ns{};
    u64 fault_base{};

    // Command processor thread.
    std::unique_ptr<TraceWriter> writer;
    AmdGpu::Liverpool* liverpool{};
    Vulkan::Rasterizer* rasterizer{};
    bool tracking{};
    bool end_pending{};
    bool frame_dump{};
    u64 seen_faults{};
    std::vector<u64> dirty;

    // What the recorder covers, under track_mutex (command processor thread, NoteGuestStack).
    std::mutex track_mutex;
    IntervalSet recorded; ///< Mapped areas the trace holds.
    IntervalSet tracked;  ///< Recorded areas the recorder write-protects.
    IntervalSet excluded; ///< Guest stacks: never write-protected.

    // Notes from other threads, under pending_mutex.
    std::mutex pending_mutex;
    std::vector<PendingMapping> pending_mappings;
    std::vector<EopFlipRecord> pending_flips;
    /// Contents of submissions made from host copies, until their first resume is recorded.
    std::unordered_map<u64, std::vector<u32>> submit_contents;
};

} // namespace VideoCore::Replay
