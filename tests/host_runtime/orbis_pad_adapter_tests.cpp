// SPDX-FileCopyrightText: Copyright 2026 shadPS4 Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

// Host contract test for OrbisPadAdapter — the native consumer of Android
// controller input. Runs on the build host and cross-compiles under the NDK. It
// exercises the spec's control-correctness, session/generation, port isolation,
// and feedback dimensions against the real Libraries::Pad::OrbisPadData layout.

#include <cmath>
#include <cstdint>
#include <cstdio>
#include <limits>

#include "core/host_runtime/orbis_pad_adapter.h"
#include "core/libraries/pad/pad.h"

using Core::HostRuntime::OrbisPadAdapter;
using Core::HostRuntime::PadResult;
using Core::HostRuntime::PadSnapshot;
using Core::HostRuntime::PadVibration;
using Libraries::Pad::OrbisPadButtonDataOffset;
using Libraries::Pad::OrbisPadData;

namespace {

int g_checks = 0;
int g_failures = 0;

void Check(bool ok, const char* what) {
    ++g_checks;
    if (!ok) {
        ++g_failures;
        std::fprintf(stderr, "  FAIL: %s\n", what);
    }
}

std::uint64_t ButtonsOf(const OrbisPadData& d) {
    return static_cast<std::uint64_t>(d.buttons);
}

void TestConversionAndButtons() {
    std::printf("control correctness: buttons, stick/trigger conversion, rejection\n");
    OrbisPadAdapter pad;
    const auto s = pad.BeginSession();
    Check(s != 0, "session token nonzero");

    // Cross bit passes through unchanged; neutral sticks centre at 128.
    PadSnapshot snap;
    snap.buttons = static_cast<std::uint64_t>(OrbisPadButtonDataOffset::Cross);
    Check(pad.Submit(s, 0, snap) == PadResult::Ok, "submit ok");

    OrbisPadData d{};
    Check(pad.ReadState(0, &d), "read ok");
    Check(ButtonsOf(d) == static_cast<std::uint64_t>(OrbisPadButtonDataOffset::Cross),
          "cross bit preserved");
    Check(d.leftStick.x == 128 && d.leftStick.y == 128, "neutral left stick = 128");
    Check(d.rightStick.x == 128 && d.rightStick.y == 128, "neutral right stick = 128");
    Check(d.connected, "port connected after submit");

    // Full right / up. left_x = +1 -> 255, left_y = -1 -> 1 (centre 128 - 127).
    snap = PadSnapshot{};
    snap.left_x = 1.0f;
    snap.left_y = -1.0f;
    snap.right_x = -1.0f;
    snap.right_y = 1.0f;
    Check(pad.Submit(s, 0, snap) == PadResult::Ok, "submit deflection ok");
    Check(pad.ReadState(0, &d), "read deflection");
    Check(d.leftStick.x == 255, "left_x +1 -> 255");
    Check(d.leftStick.x >= 1 && d.leftStick.x == 255, "left_x max");
    Check(d.leftStick.y == 1, "left_y -1 -> 1");
    Check(d.rightStick.x == 1, "right_x -1 -> 1");
    Check(d.rightStick.y == 255, "right_y +1 -> 255");

    // Triggers 0..1 -> 0..255.
    snap = PadSnapshot{};
    snap.left_trigger = 1.0f;
    snap.right_trigger = 0.5f;
    Check(pad.Submit(s, 0, snap) == PadResult::Ok, "submit triggers ok");
    Check(pad.ReadState(0, &d), "read triggers");
    Check(d.analogButtons.l2 == 255, "left trigger 1.0 -> 255");
    Check(d.analogButtons.r2 >= 127 && d.analogButtons.r2 <= 128, "right trigger 0.5 ~ 128");

    // NaN / Inf rejected; last good state unchanged.
    snap = PadSnapshot{};
    snap.left_x = std::numeric_limits<float>::quiet_NaN();
    Check(pad.Submit(s, 0, snap) == PadResult::Rejected, "NaN rejected");
    snap.left_x = std::numeric_limits<float>::infinity();
    Check(pad.Submit(s, 0, snap) == PadResult::Rejected, "Inf rejected");
    Check(pad.ReadState(0, &d) && d.analogButtons.l2 == 255, "state unchanged after reject");

    // Bad port.
    Check(pad.Submit(s, 4, PadSnapshot{}) == PadResult::BadPort, "port 4 rejected");
    Check(pad.Submit(s, -1, PadSnapshot{}) == PadResult::BadPort, "port -1 rejected");
}

void TestSessionGuard() {
    std::printf("session/generation: wrong token, no session, re-begin clears state\n");
    OrbisPadAdapter pad;

    // No session yet.
    Check(pad.Submit(1, 0, PadSnapshot{}) == PadResult::NoSession, "submit before begin refused");

    const auto s1 = pad.BeginSession();
    PadSnapshot snap;
    snap.buttons = static_cast<std::uint64_t>(OrbisPadButtonDataOffset::Circle);
    Check(pad.Submit(s1, 0, snap) == PadResult::Ok, "s1 submit ok");

    // Wrong token refused.
    Check(pad.Submit(s1 + 999, 0, snap) == PadResult::WrongSession, "wrong token refused");

    // A new session is a new generation: state cleared, old token stale.
    const auto s2 = pad.BeginSession();
    Check(s2 != s1, "new session token differs");
    Check(pad.Submit(s1, 0, snap) == PadResult::WrongSession, "old token stale after re-begin");
    OrbisPadData d{};
    Check(pad.ReadState(0, &d), "read after re-begin");
    Check(ButtonsOf(d) == 0, "state cleared on new session");
    Check(!d.connected, "port neutral (disconnected) after new session");

    // EndSession with a stale token is a no-op; with the live token clears.
    pad.EndSession(s1);
    Check(pad.CurrentToken() == s2, "stale EndSession ignored");
    pad.EndSession(s2);
    Check(pad.CurrentToken() == 0, "live EndSession clears");
    Check(pad.Submit(s2, 0, snap) == PadResult::NoSession, "submit after end refused");
}

void TestPortIsolationAndConnect() {
    std::printf("multi-port isolation + connect/disconnect\n");
    OrbisPadAdapter pad;
    const auto s = pad.BeginSession();

    PadSnapshot a;
    a.buttons = static_cast<std::uint64_t>(OrbisPadButtonDataOffset::Triangle);
    PadSnapshot b;
    b.buttons = static_cast<std::uint64_t>(OrbisPadButtonDataOffset::Square);
    Check(pad.Submit(s, 0, a) == PadResult::Ok, "port0 submit");
    Check(pad.Submit(s, 1, b) == PadResult::Ok, "port1 submit");

    Check(pad.ReadButtons(0) == static_cast<std::uint64_t>(OrbisPadButtonDataOffset::Triangle),
          "port0 triangle");
    Check(pad.ReadButtons(1) == static_cast<std::uint64_t>(OrbisPadButtonDataOffset::Square),
          "port1 square (isolated)");
    Check(pad.ReadButtons(2) == 0, "port2 untouched");

    // Disconnect port0 neutralizes it but preserves connectedCount for reconnect.
    OrbisPadData d{};
    Check(pad.ReadState(0, &d), "read port0");
    const std::uint8_t count_before = d.connectedCount;
    Check(pad.SetConnected(s, 0, false) == PadResult::Ok, "disconnect port0");
    Check(!pad.Connected(0), "port0 disconnected");
    Check(pad.ReadButtons(0) == 0, "port0 neutralized on disconnect");
    Check(pad.Connected(1), "port1 unaffected");

    // Reconnect bumps connectedCount.
    Check(pad.SetConnected(s, 0, true) == PadResult::Ok, "reconnect port0");
    Check(pad.ReadState(0, &d), "read port0 reconnected");
    Check(d.connectedCount > count_before, "connectedCount bumped on reconnect");
}

void TestVibration() {
    std::printf("feedback: enqueue latest-wins, cancel, drain, session guard\n");
    OrbisPadAdapter pad;
    const auto s = pad.BeginSession();

    Check(pad.SetVibration(s, 0, 200, 100) == PadResult::Ok, "vibration enqueued");
    // Latest wins: a second enqueue overwrites the first pending command.
    Check(pad.SetVibration(s, 0, 50, 25) == PadResult::Ok, "vibration overwrite");
    PadVibration v{};
    Check(pad.DrainVibration(0, &v), "drain ok");
    Check(v.small_motor == 50 && v.large_motor == 25, "latest vibration wins");
    Check(!v.cancel, "not a cancel");
    Check(!pad.DrainVibration(0, &v), "nothing left after drain");

    // A (0,0) request is a cancel.
    Check(pad.SetVibration(s, 0, 0, 0) == PadResult::Ok, "cancel enqueued");
    Check(pad.DrainVibration(0, &v), "drain cancel");
    Check(v.cancel && v.small_motor == 0 && v.large_motor == 0, "cancel zeroes motors");

    // Wrong session refused.
    Check(pad.SetVibration(s + 7, 0, 100, 100) == PadResult::WrongSession,
          "wrong-session vibration refused");

    // New session clears queued feedback.
    Check(pad.SetVibration(s, 1, 100, 100) == PadResult::Ok, "queue on port1");
    pad.BeginSession();
    Check(!pad.DrainVibration(1, &v), "feedback cleared on new session");
}

} // namespace

int main() {
    std::printf("orbis_pad_adapter_tests\n");
    TestConversionAndButtons();
    TestSessionGuard();
    TestPortIsolationAndConnect();
    TestVibration();
    std::printf("\n%d checks, %d failures\n", g_checks, g_failures);
    return g_failures == 0 ? 0 : 1;
}
