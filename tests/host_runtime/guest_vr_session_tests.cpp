// SPDX-License-Identifier: GPL-2.0-or-later
#include <cstdio>
#include "core/host_runtime/guest_vr_session.h"
using Core::HostRuntime::GuestVrSession;
int main() {
    unsigned checks = 0, failures = 0;
#define CHECK(x) do { ++checks; if (!(x)) { ++failures; std::printf("FAIL %d: %s\n", __LINE__, #x); } } while (false)
    GuestVrSession s{GuestVrSession::OutputMode::Sbs};
    CHECK(s.InitializeHmd(true) == GuestVrSession::Error::None);
    CHECK(s.OpenHmd() == GuestVrSession::Error::None);
    CHECK(s.InitializeTracker() == GuestVrSession::Error::None);
    CHECK(s.InitializeReprojection() == GuestVrSession::Error::None);
    CHECK(s.StartReprojection() == GuestVrSession::Error::None);
    GuestVrSession::FrameToken frame{};
    CHECK(s.AcquireFrame(1234, &frame) == GuestVrSession::Error::None);
    CHECK(s.MarkTrackerSubmitted(frame) == GuestVrSession::Error::None);
    CHECK(s.MarkTrackerProcessed(frame) == GuestVrSession::Error::None);
    CHECK(s.SubmitFrame(frame) == GuestVrSession::Error::None);
    CHECK(s.CompleteFrame(frame) == GuestVrSession::Error::None);
    CHECK(s.GetSnapshot().submitted_frames == 1);
    GuestVrSession::FrameToken second{};
    CHECK(s.AcquireFrame(5678, &second) == GuestVrSession::Error::None);
    CHECK(s.StopReprojection() == GuestVrSession::Error::None);
    CHECK(s.CompleteFrame(second) == GuestVrSession::Error::StaleFrame);
    CHECK(s.CloseHmd() == GuestVrSession::Error::None);
    CHECK(s.TerminateHmd() == GuestVrSession::Error::None);
    CHECK(s.GetSnapshot().generation == 2);
    std::printf("GUEST_VR_SESSION checks=%u failures=%u\n", checks, failures);
    return failures ? 1 : 0;
}
