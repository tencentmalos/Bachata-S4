// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once
#include <algorithm>
#include <cmath>
#include "core/host_runtime/guest_vr_sensor.h"
#include "spatial/xr/ControllerSnapshot.h"

namespace Core::HostRuntime {
// Match PS pad positions (A/Cross, B/Circle, X/Square, Y/Triangle). This does
// not pass through Android's user-selectable Xbox/Nintendo label conversion.
inline void MapXrHand(const spatial::xr::HandSnapshot& c, unsigned hand, GuestVrSensor::Hand& h) {
    const auto clamp = [](float v, float lo, float hi) {
        return std::isfinite(v) ? std::clamp(v, lo, hi) : 0.f;
    };
    h.stick_x = clamp(c.thumbstick.x, -1, 1);
    h.stick_y = -clamp(c.thumbstick.y, -1, 1);
    h.trigger = clamp(c.trigger, 0, 1);
    h.squeeze = clamp(c.squeeze, 0, 1);
    using B = spatial::xr::ControllerButton;
    const auto pressed = [&](B b) { return (c.buttons & spatial::xr::ButtonBit(b)) != 0; };
    h.buttons = 0;
    if (pressed(B::A))
        h.buttons |= 0x4000;
    if (pressed(B::B))
        h.buttons |= 0x2000;
    if (pressed(B::X))
        h.buttons |= 0x8000;
    if (pressed(B::Y))
        h.buttons |= 0x1000;
    if (pressed(B::Menu))
        h.buttons |= 0x8;
    if (pressed(B::ThumbstickClick))
        h.buttons |= h.squeeze > .5f ? (hand ? 0x8 : 0x100000) : (hand ? 0x4 : 0x2);
    if (h.squeeze > .5f) {
        if (h.trigger > .5f)
            h.buttons |= hand ? 0x800 : 0x400;
        if (h.stick_y < -.5f)
            h.buttons |= 0x10;
        if (h.stick_y > .5f)
            h.buttons |= 0x40;
        if (h.stick_x < -.5f)
            h.buttons |= 0x80;
        if (h.stick_x > .5f)
            h.buttons |= 0x20;
    }
}
struct XrPadMapping {
    uint64_t buttons{};
    std::array<float, 6> axes{};
    bool connected{};
};
inline XrPadMapping MapXrPad(const std::array<GuestVrSensor::Hand, 2>& hands) {
    XrPadMapping pad;
    for (unsigned i = 0; i < 2; ++i) {
        const auto& h = hands[i];
        if (!h.active)
            continue;
        pad.connected = true;
        // As in Citron, right-hand D-pad takes precedence when both hands chord.
        if (i && (h.buttons & 0xf0))
            pad.buttons &= ~uint64_t(0xf0);
        pad.buttons |= h.buttons;
        if (h.squeeze <= .5f) {
            pad.axes[i * 2] = h.stick_x;
            pad.axes[i * 2 + 1] = h.stick_y;
            pad.axes[4 + i] = h.trigger;
        }
    }
    return pad;
}
} // namespace Core::HostRuntime
