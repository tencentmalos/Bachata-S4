// SPDX-FileCopyrightText: Copyright 2026 shadPS4 Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#include "core/host_runtime/orbis_pad_adapter.h"

#include <cmath>

namespace Core::HostRuntime {

namespace {

// Production stick conversion (pad.cpp/ProcessStates + controller.h axis_defaults):
// the guest expects u8 axes centred at 128. A neutral stick is 128; full deflection
// maps to 0 / 255. Input is a normalized float in [-1, 1].
std::uint8_t StickToU8(float v) {
    if (v < -1.0f) v = -1.0f;
    if (v > 1.0f) v = 1.0f;
    const float scaled = 128.0f + v * 127.0f;
    const int rounded = static_cast<int>(std::lround(scaled));
    return static_cast<std::uint8_t>(rounded < 0 ? 0 : (rounded > 255 ? 255 : rounded));
}

// Trigger conversion: a normalized float in [0, 1] to a u8 in [0, 255].
std::uint8_t TriggerToU8(float v) {
    if (v < 0.0f) v = 0.0f;
    if (v > 1.0f) v = 1.0f;
    const int rounded = static_cast<int>(std::lround(v * 255.0f));
    return static_cast<std::uint8_t>(rounded < 0 ? 0 : (rounded > 255 ? 255 : rounded));
}

std::uint16_t TouchToU16(float v, std::uint16_t span) {
    if (v < 0.0f) v = 0.0f;
    if (v > 1.0f) v = 1.0f;
    const int rounded = static_cast<int>(std::lround(v * static_cast<float>(span)));
    return static_cast<std::uint16_t>(rounded < 0 ? 0 : (rounded > span ? span : rounded));
}

bool Finite(const PadSnapshot& s) {
    return std::isfinite(s.left_x) && std::isfinite(s.left_y) && std::isfinite(s.right_x) &&
           std::isfinite(s.right_y) && std::isfinite(s.left_trigger) &&
           std::isfinite(s.right_trigger) && std::isfinite(s.touch_x) && std::isfinite(s.touch_y);
}

// PS4 DS4 touchpad reports 1920x943 in the resolution most titles read; the exact
// value is device-info driven in the full HLE. Using the common DS4 span keeps the
// converted coordinates in range until the HLE supplies the queried resolution.
constexpr std::uint16_t kTouchSpanX = 1919;
constexpr std::uint16_t kTouchSpanY = 942;

} // namespace

OrbisPadAdapter::OrbisPadAdapter() {
    for (auto& p : ports_) {
        p.data = NeutralState();
    }
}

Libraries::Pad::OrbisPadData OrbisPadAdapter::NeutralState() {
    Libraries::Pad::OrbisPadData d{};
    d.buttons = Libraries::Pad::OrbisPadButtonDataOffset::None;
    d.leftStick = {128, 128};
    d.rightStick = {128, 128};
    d.analogButtons = {0, 0, {0, 0}};
    d.orientation = {0.0f, 0.0f, 0.0f, 1.0f};
    d.acceleration = {0.0f, 0.0f, 0.0f};
    d.angularVelocity = {0.0f, 0.0f, 0.0f};
    d.connected = false;
    d.connectedCount = 0;
    d.timestamp = 0;
    d.deviceUniqueDataLen = 0;
    return d;
}

void OrbisPadAdapter::ResetLocked() {
    for (auto& p : ports_) {
        p.data = NeutralState();
        p.has_vibration = false;
        p.vibration = {};
    }
}

std::uint64_t OrbisPadAdapter::BeginSession() {
    std::lock_guard lock(lock_);
    token_ = next_token_++;
    ResetLocked();
    return token_;
}

void OrbisPadAdapter::EndSession(std::uint64_t token) {
    std::lock_guard lock(lock_);
    if (token == 0 || token != token_) {
        return; // stale / no session
    }
    token_ = 0;
    ResetLocked();
}

std::uint64_t OrbisPadAdapter::CurrentToken() const {
    std::lock_guard lock(lock_);
    return token_;
}

PadResult OrbisPadAdapter::Submit(std::uint64_t token, int port, const PadSnapshot& snap) {
    std::lock_guard lock(lock_);
    if (token_ == 0) {
        return PadResult::NoSession;
    }
    if (token != token_) {
        return PadResult::WrongSession;
    }
    if (port < 0 || port >= kMaxPadPorts) {
        return PadResult::BadPort;
    }
    if (!Finite(snap)) {
        return PadResult::Rejected;
    }

    Port& p = ports_[static_cast<std::size_t>(port)];
    Libraries::Pad::OrbisPadData& d = p.data;

    d.buttons = static_cast<Libraries::Pad::OrbisPadButtonDataOffset>(snap.buttons);
    d.leftStick.x = StickToU8(snap.left_x);
    d.leftStick.y = StickToU8(snap.left_y);
    d.rightStick.x = StickToU8(snap.right_x);
    d.rightStick.y = StickToU8(snap.right_y);
    d.analogButtons.l2 = TriggerToU8(snap.left_trigger);
    d.analogButtons.r2 = TriggerToU8(snap.right_trigger);

    if (snap.touch_down) {
        d.touchData.touchNum = 1;
        d.touchData.touch[0].x = TouchToU16(snap.touch_x, kTouchSpanX);
        d.touchData.touch[0].y = TouchToU16(snap.touch_y, kTouchSpanY);
        d.touchData.touch[0].id = 1;
    } else {
        d.touchData.touchNum = 0;
        d.touchData.touch[0] = {};
        d.touchData.touch[1] = {};
    }

    d.orientation = {0.0f, 0.0f, 0.0f, 1.0f};
    d.connected = true;
    if (d.connectedCount == 0) {
        d.connectedCount = 1;
    }
    d.timestamp = ++sequence_;
    return PadResult::Ok;
}

PadResult OrbisPadAdapter::SetConnected(std::uint64_t token, int port, bool connected) {
    std::lock_guard lock(lock_);
    if (token_ == 0) {
        return PadResult::NoSession;
    }
    if (token != token_) {
        return PadResult::WrongSession;
    }
    if (port < 0 || port >= kMaxPadPorts) {
        return PadResult::BadPort;
    }
    Port& p = ports_[static_cast<std::size_t>(port)];
    if (connected && !p.data.connected) {
        // A fresh connection bumps connectedCount, matching the guest contract that
        // uses it to detect a reconnect between reads.
        p.data.connectedCount = static_cast<std::uint8_t>(p.data.connectedCount + 1);
    }
    p.data.connected = connected;
    if (!connected) {
        // Neutralize inputs on disconnect but keep connectedCount for reconnect
        // detection.
        const std::uint8_t count = p.data.connectedCount;
        p.data = NeutralState();
        p.data.connectedCount = count;
    }
    p.data.timestamp = ++sequence_;
    return PadResult::Ok;
}

bool OrbisPadAdapter::ReadState(int port, Libraries::Pad::OrbisPadData* out) const {
    if (out == nullptr || port < 0 || port >= kMaxPadPorts) {
        return false;
    }
    std::lock_guard lock(lock_);
    *out = ports_[static_cast<std::size_t>(port)].data;
    return true;
}

std::uint64_t OrbisPadAdapter::ReadButtons(int port) const {
    if (port < 0 || port >= kMaxPadPorts) {
        return 0;
    }
    std::lock_guard lock(lock_);
    return static_cast<std::uint64_t>(ports_[static_cast<std::size_t>(port)].data.buttons);
}

bool OrbisPadAdapter::Connected(int port) const {
    if (port < 0 || port >= kMaxPadPorts) {
        return false;
    }
    std::lock_guard lock(lock_);
    return ports_[static_cast<std::size_t>(port)].data.connected;
}

PadResult OrbisPadAdapter::SetVibration(std::uint64_t token, int port, std::uint8_t small_motor,
                                        std::uint8_t large_motor) {
    std::lock_guard lock(lock_);
    if (token_ == 0) {
        return PadResult::NoSession;
    }
    if (token != token_) {
        return PadResult::WrongSession;
    }
    if (port < 0 || port >= kMaxPadPorts) {
        return PadResult::BadPort;
    }
    Port& p = ports_[static_cast<std::size_t>(port)];
    p.vibration.small_motor = small_motor;
    p.vibration.large_motor = large_motor;
    p.vibration.cancel = (small_motor == 0 && large_motor == 0);
    p.has_vibration = true; // latest wins; overwrites any pending
    return PadResult::Ok;
}

bool OrbisPadAdapter::DrainVibration(int port, PadVibration* out) {
    if (out == nullptr || port < 0 || port >= kMaxPadPorts) {
        return false;
    }
    std::lock_guard lock(lock_);
    Port& p = ports_[static_cast<std::size_t>(port)];
    if (!p.has_vibration) {
        return false;
    }
    *out = p.vibration;
    p.has_vibration = false;
    return true;
}

OrbisPadAdapter& GlobalPadAdapter() {
    static OrbisPadAdapter adapter;
    return adapter;
}

} // namespace Core::HostRuntime
