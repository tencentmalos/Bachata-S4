// SPDX-FileCopyrightText: Copyright 2026 shadPS4 Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#pragma once

// OrbisPadAdapter — the native consumer of Android controller input.
//
// The Android front end (physical gamepad via GamepadInputManager, or the touch
// overlay) resolves every source into a neutral, already-PS4-shaped snapshot
// (buttons in OrbisPadButtonDataOffset bits, sticks in [-1,1], triggers in
// [0,1]). Before this adapter existed, that snapshot reached
// ManagedSession.submitController() and stopped: no native code consumed it.
//
// This adapter closes that gap. It:
//   * owns per-port PS4 pad state using the REAL Libraries::Pad::OrbisPadData
//     layout (single source of truth — no shortcut POD), applying the exact
//     production conversion pad.cpp/ProcessStates uses (sticks -> 0..255 centred
//     at 128, triggers -> 0..255, button bits passed through),
//   * is session/generation guarded like SessionCore: a Submit tagged with an
//     older token than the live BeginSession is refused, so a stale producer from
//     a previous game cannot write into the current session's pad,
//   * is bounded and lock-guarded; ReadState publishes a consistent snapshot,
//   * carries a latest-wins vibration slot the Android side drains and executes
//     on VibratorManager (the hub never calls Java).
//
// This is deliberately independent of Input::GameController (which pulls SDL). The
// production wiring — scePadReadState reading from this adapter, or this adapter
// driving GameController::Button/Axis once the HLE links into the app — is a later
// stage; the injection shape here (per-port neutral submit) matches that seam so
// the swap is wiring, not a rewrite.

#include <array>
#include <cstdint>
#include <mutex>

#include "core/libraries/pad/pad.h"

namespace Core::HostRuntime {

inline constexpr int kMaxPadPorts = 4;

// Mirrors the Kotlin ControllerSnapshot fields exactly. `buttons` is already in
// Libraries::Pad::OrbisPadButtonDataOffset bit form (the Kotlin resolver produces
// those bits directly), sticks are [-1,1], triggers are [0,1], touch is [0,1].
struct PadSnapshot {
    std::uint64_t buttons = 0;
    float left_x = 0.0f;
    float left_y = 0.0f;
    float right_x = 0.0f;
    float right_y = 0.0f;
    float left_trigger = 0.0f;
    float right_trigger = 0.0f;
    bool touch_down = false;
    float touch_x = 0.0f;
    float touch_y = 0.0f;
};

enum class PadResult {
    Ok = 0,
    WrongSession = 1, // token != the live session token
    BadPort = 2,      // port outside [0, kMaxPadPorts)
    Rejected = 3,     // NaN/Inf in a float field
    NoSession = 4,    // no BeginSession active
};

// One latest-wins vibration request for a port.
struct PadVibration {
    std::uint8_t small_motor = 0;
    std::uint8_t large_motor = 0;
    bool cancel = false; // true when both motors are 0 (an explicit stop)
};

class OrbisPadAdapter {
public:
    OrbisPadAdapter();

    // Starts a new input session; returns the new non-zero token. Clears all port
    // state and queued vibration (a new session is a new generation).
    std::uint64_t BeginSession();

    // Ends `token` if it is the live session; clears state. A stale token is a
    // no-op. After EndSession the ports report disconnected until the next
    // BeginSession.
    void EndSession(std::uint64_t token);

    // The live session token, or 0 when idle.
    std::uint64_t CurrentToken() const;

    // Submits a neutral snapshot for `port`. Validates the session token, the port
    // and the float fields, then converts to OrbisPadData and stores it.
    PadResult Submit(std::uint64_t token, int port, const PadSnapshot& snap);

    // Marks `port` connected/disconnected within the live session.
    PadResult SetConnected(std::uint64_t token, int port, bool connected);

    // Copies the latest converted OrbisPadData for `port`. Returns false for a bad
    // port. A never-written / disconnected port reports connected=false with a
    // neutral (centred) state.
    bool ReadState(int port, Libraries::Pad::OrbisPadData* out) const;

    // Convenience read-backs for telemetry / on-device verification.
    std::uint64_t ReadButtons(int port) const;
    bool Connected(int port) const;

    // Enqueues a latest-wins vibration for `port`. small/large are 0..255. A (0,0)
    // request is a cancel. Session guarded.
    PadResult SetVibration(std::uint64_t token, int port, std::uint8_t small_motor,
                           std::uint8_t large_motor);

    // Drains the pending vibration for `port` (latest wins). Returns false when
    // none is pending. Not session guarded on read so a stop can still deliver a
    // final cancel.
    bool DrainVibration(int port, PadVibration* out);

private:
    struct Port {
        Libraries::Pad::OrbisPadData data{};
        bool has_vibration = false;
        PadVibration vibration{};
    };

    // lock_ must be held.
    void ResetLocked();
    static Libraries::Pad::OrbisPadData NeutralState();

    mutable std::mutex lock_;
    std::uint64_t token_ = 0;
    std::uint64_t next_token_ = 1;
    std::uint64_t sequence_ = 0; // monotonic timestamp source
    std::array<Port, kMaxPadPorts> ports_{};
};

// Process-global adapter shared by the JNI pad bridge and (later) the HLE.
OrbisPadAdapter& GlobalPadAdapter();

} // namespace Core::HostRuntime
