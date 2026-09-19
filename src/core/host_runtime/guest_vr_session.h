// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once

#include <cstdint>
#include <mutex>

namespace Core::HostRuntime {

// Per-guest-session PSVR lifecycle. It contains no Android or OpenXR handle:
// presenters consume these frame boundaries, while HLE reports guest state.
class GuestVrSession final {
public:
    enum class OutputMode : uint8_t { Flat, Sbs, OpenXr };
    enum class State : uint8_t { Idle, HmdInitialized, HmdOpen, TrackerInitialized,
                                 ReprojectionInitialized, Running, Stopping };
    enum class FramePhase : uint8_t { Idle, Acquired, TrackerSubmitted,
                                      TrackerProcessed, Submitted };
    enum class Error : uint8_t { None, InvalidTransition, InvalidFramePhase,
                                 StaleFrame, AlreadyRunning, NotRunning };

    struct FrameToken final {
        uint64_t generation{};
        uint64_t sequence{};
        uint64_t predicted_time_us{};
    };
    struct Snapshot final {
        State state{State::Idle};
        FramePhase phase{FramePhase::Idle};
        OutputMode output{OutputMode::Flat};
        uint64_t generation{};
        uint64_t next_sequence{};
        uint64_t last_submitted_sequence{};
        uint64_t submitted_frames{};
        bool diagnostic_pose{};
    };

    explicit GuestVrSession(OutputMode output = OutputMode::Flat);
    void Reset();
    Error InitializeHmd(bool diagnostic_pose);
    Error OpenHmd();
    Error InitializeTracker();
    Error InitializeReprojection();
    Error StartReprojection();
    Error StopReprojection();
    Error CloseHmd();
    Error TerminateHmd();

    Error AcquireFrame(uint64_t predicted_time_us, FrameToken* token);
    Error MarkTrackerSubmitted(const FrameToken& token);
    Error MarkTrackerProcessed(const FrameToken& token);
    Error SubmitFrame(const FrameToken& token);
    Error CompleteFrame(const FrameToken& token);
    Error SubmitActiveFrame();
    Error CompleteActiveFrame();
    void SetOutputMode(OutputMode output);
    Snapshot GetSnapshot() const;

    static const char* ErrorName(Error error);
    static const char* StateName(State state);
    static const char* FramePhaseName(FramePhase phase);

private:
    Error CheckFrame(const FrameToken& token, FramePhase expected) const;
    mutable std::mutex mutex;
    State state{State::Idle};
    FramePhase phase{FramePhase::Idle};
    OutputMode output{OutputMode::Flat};
    uint64_t generation{1};
    uint64_t next_sequence{1};
    uint64_t last_submitted_sequence{};
    uint64_t submitted_frames{};
    FrameToken active_frame{};
    bool diagnostic_pose{};
};

} // namespace Core::HostRuntime
