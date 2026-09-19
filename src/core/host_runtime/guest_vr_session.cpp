// SPDX-License-Identifier: GPL-2.0-or-later
#include "core/host_runtime/guest_vr_session.h"

namespace Core::HostRuntime {

GuestVrSession::GuestVrSession(OutputMode output_) : output{output_} {}

void GuestVrSession::Reset() {
    std::scoped_lock lock{mutex};
    ++generation; state = State::Idle; phase = FramePhase::Idle;
    next_sequence = 1; last_submitted_sequence = 0; submitted_frames = 0;
    active_frame = {}; diagnostic_pose = false;
}

GuestVrSession::Error GuestVrSession::InitializeHmd(bool diagnostic_pose_) {
    std::scoped_lock lock{mutex};
    if (state != State::Idle) return Error::InvalidTransition;
    diagnostic_pose = diagnostic_pose_; state = State::HmdInitialized;
    return Error::None;
}
GuestVrSession::Error GuestVrSession::OpenHmd() {
    std::scoped_lock lock{mutex};
    if (state != State::HmdInitialized) return Error::InvalidTransition;
    state = State::HmdOpen; return Error::None;
}
GuestVrSession::Error GuestVrSession::InitializeTracker() {
    std::scoped_lock lock{mutex};
    if (state != State::HmdOpen) return Error::InvalidTransition;
    state = State::TrackerInitialized; return Error::None;
}
GuestVrSession::Error GuestVrSession::InitializeReprojection() {
    std::scoped_lock lock{mutex};
    if (state != State::TrackerInitialized && state != State::HmdOpen)
        return Error::InvalidTransition;
    state = State::ReprojectionInitialized; return Error::None;
}
GuestVrSession::Error GuestVrSession::StartReprojection() {
    std::scoped_lock lock{mutex};
    if (state == State::Running) return Error::AlreadyRunning;
    if (state != State::ReprojectionInitialized) return Error::InvalidTransition;
    phase = FramePhase::Idle; state = State::Running; return Error::None;
}
GuestVrSession::Error GuestVrSession::StopReprojection() {
    std::scoped_lock lock{mutex};
    if (state != State::Running && state != State::ReprojectionInitialized)
        return Error::NotRunning;
    state = State::Stopping; phase = FramePhase::Idle; active_frame = {};
    state = State::TrackerInitialized; return Error::None;
}
GuestVrSession::Error GuestVrSession::CloseHmd() {
    std::scoped_lock lock{mutex};
    if (state == State::Running || state == State::Stopping) return Error::InvalidTransition;
    if (state != State::HmdOpen && state != State::TrackerInitialized &&
        state != State::ReprojectionInitialized) return Error::InvalidTransition;
    state = State::HmdInitialized; phase = FramePhase::Idle; return Error::None;
}
GuestVrSession::Error GuestVrSession::TerminateHmd() {
    std::scoped_lock lock{mutex};
    if (state != State::HmdInitialized) return Error::InvalidTransition;
    ++generation; state = State::Idle; phase = FramePhase::Idle;
    active_frame = {}; diagnostic_pose = false; return Error::None;
}

GuestVrSession::Error GuestVrSession::AcquireFrame(uint64_t predicted_time_us,
                                                   FrameToken* token) {
    if (!token) return Error::InvalidFramePhase;
    std::scoped_lock lock{mutex};
    if (state != State::Running) return Error::NotRunning;
    if (phase != FramePhase::Idle) return Error::InvalidFramePhase;
    active_frame = {.generation = generation, .sequence = next_sequence++,
                    .predicted_time_us = predicted_time_us};
    phase = FramePhase::Acquired; *token = active_frame; return Error::None;
}
GuestVrSession::Error GuestVrSession::CheckFrame(const FrameToken& token,
                                                 FramePhase expected) const {
    if (token.generation != generation || token.sequence != active_frame.sequence)
        return Error::StaleFrame;
    return phase == expected ? Error::None : Error::InvalidFramePhase;
}
GuestVrSession::Error GuestVrSession::MarkTrackerSubmitted(const FrameToken& token) {
    std::scoped_lock lock{mutex};
    if (auto e = CheckFrame(token, FramePhase::Acquired); e != Error::None) return e;
    phase = FramePhase::TrackerSubmitted; return Error::None;
}
GuestVrSession::Error GuestVrSession::MarkTrackerProcessed(const FrameToken& token) {
    std::scoped_lock lock{mutex};
    if (auto e = CheckFrame(token, FramePhase::TrackerSubmitted); e != Error::None) return e;
    phase = FramePhase::TrackerProcessed; return Error::None;
}
GuestVrSession::Error GuestVrSession::SubmitFrame(const FrameToken& token) {
    std::scoped_lock lock{mutex};
    if (auto e = CheckFrame(token, FramePhase::TrackerProcessed); e != Error::None) return e;
    phase = FramePhase::Submitted; last_submitted_sequence = token.sequence;
    ++submitted_frames; return Error::None;
}
GuestVrSession::Error GuestVrSession::CompleteFrame(const FrameToken& token) {
    std::scoped_lock lock{mutex};
    if (auto e = CheckFrame(token, FramePhase::Submitted); e != Error::None) return e;
    phase = FramePhase::Idle; active_frame = {}; return Error::None;
}
GuestVrSession::Error GuestVrSession::SubmitActiveFrame() {
    std::scoped_lock lock{mutex};
    if (auto e = CheckFrame(active_frame, FramePhase::TrackerProcessed); e != Error::None) return e;
    phase = FramePhase::Submitted; last_submitted_sequence = active_frame.sequence;
    ++submitted_frames; return Error::None;
}
GuestVrSession::Error GuestVrSession::CompleteActiveFrame() {
    std::scoped_lock lock{mutex};
    if (auto e = CheckFrame(active_frame, FramePhase::Submitted); e != Error::None) return e;
    phase = FramePhase::Idle; active_frame = {}; return Error::None;
}
void GuestVrSession::SetOutputMode(OutputMode output_) {
    std::scoped_lock lock{mutex}; output = output_;
}
GuestVrSession::Snapshot GuestVrSession::GetSnapshot() const {
    std::scoped_lock lock{mutex};
    return {.state = state, .phase = phase, .output = output, .generation = generation,
            .next_sequence = next_sequence, .last_submitted_sequence = last_submitted_sequence,
            .submitted_frames = submitted_frames, .diagnostic_pose = diagnostic_pose};
}
const char* GuestVrSession::ErrorName(Error e) {
    switch (e) {
    case Error::None: return "none"; case Error::InvalidTransition: return "invalid_transition";
    case Error::InvalidFramePhase: return "invalid_frame_phase"; case Error::StaleFrame: return "stale_frame";
    case Error::AlreadyRunning: return "already_running"; case Error::NotRunning: return "not_running";
    } return "unknown";
}
const char* GuestVrSession::StateName(State s) {
    switch (s) {
    case State::Idle: return "idle"; case State::HmdInitialized: return "hmd_initialized";
    case State::HmdOpen: return "hmd_open"; case State::TrackerInitialized: return "tracker_initialized";
    case State::ReprojectionInitialized: return "reprojection_initialized"; case State::Running: return "running";
    case State::Stopping: return "stopping";
    } return "unknown";
}
const char* GuestVrSession::FramePhaseName(FramePhase p) {
    switch (p) {
    case FramePhase::Idle: return "idle"; case FramePhase::Acquired: return "acquired";
    case FramePhase::TrackerSubmitted: return "tracker_submitted";
    case FramePhase::TrackerProcessed: return "tracker_processed"; case FramePhase::Submitted: return "submitted";
    } return "unknown";
}
} // namespace Core::HostRuntime
