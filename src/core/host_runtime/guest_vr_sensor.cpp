// SPDX-FileCopyrightText: Copyright 2026 shadPS4 Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#include "core/host_runtime/guest_vr_sensor.h"

#include <algorithm>
#include <cmath>

namespace Core::HostRuntime {

GuestVrSensor& GuestVrSensor::Instance() {
    static GuestVrSensor sensor;
    return sensor;
}

void GuestVrSensor::SetSbsEnabled(bool enabled) {
    std::scoped_lock lock{mutex};
    snapshot.enabled = enabled;
    if (!enabled) {
        snapshot = {};
        snapshot.orientation_w = 1.0f;
        move_input = {};
    } else {
        snapshot.orientation_x = 0.0f;
        snapshot.orientation_y = 0.0f;
        snapshot.orientation_z = 0.0f;
        snapshot.orientation_w = 1.0f;
        snapshot.angular_velocity_x = 0.0f;
        snapshot.angular_velocity_y = 0.0f;
        snapshot.angular_velocity_z = 0.0f;
        snapshot.timestamp_ns = 0;
        move_input = {};
    }
}

void GuestVrSensor::UpdateGyro(float x, float y, float z, std::uint64_t timestamp_ns) {
    std::scoped_lock lock{mutex};
    if (!snapshot.enabled)
        return;

    float dt = 0.0f;
    if (snapshot.timestamp_ns != 0 && timestamp_ns > snapshot.timestamp_ns) {
        // Android sensor delivery can pause while the Activity changes focus;
        // never turn such a gap into a multi-second pose jump.
        dt = std::min(0.1f, static_cast<float>(timestamp_ns - snapshot.timestamp_ns) * 1e-9f);
    }
    snapshot.angular_velocity_x = x;
    snapshot.angular_velocity_y = y;
    snapshot.angular_velocity_z = z;
    snapshot.timestamp_ns = timestamp_ns;
    if (dt <= 0.0f)
        return;

    const float qx = snapshot.orientation_x;
    const float qy = snapshot.orientation_y;
    const float qz = snapshot.orientation_z;
    const float qw = snapshot.orientation_w;
    const float hx = x * dt * 0.5f;
    const float hy = y * dt * 0.5f;
    const float hz = z * dt * 0.5f;
    snapshot.orientation_x = qx + (qw * hx + qy * hz - qz * hy);
    snapshot.orientation_y = qy + (qw * hy + qz * hx - qx * hz);
    snapshot.orientation_z = qz + (qw * hz + qx * hy - qy * hx);
    snapshot.orientation_w = qw + (-qx * hx - qy * hy - qz * hz);
    const float norm = std::sqrt(snapshot.orientation_x * snapshot.orientation_x +
                                 snapshot.orientation_y * snapshot.orientation_y +
                                 snapshot.orientation_z * snapshot.orientation_z +
                                 snapshot.orientation_w * snapshot.orientation_w);
    if (norm > 1e-6f) {
        snapshot.orientation_x /= norm;
        snapshot.orientation_y /= norm;
        snapshot.orientation_z /= norm;
        snapshot.orientation_w /= norm;
    }
}

void GuestVrSensor::UpdateMoveInput(std::uint64_t buttons, float left_x, float left_y,
                                    float right_x, float right_y, float left_trigger,
                                    float right_trigger, std::uint64_t timestamp_ns) {
    std::scoped_lock lock{mutex};
    if (!snapshot.enabled)
        return;
    move_input.buttons = buttons;
    move_input.left_x = std::clamp(left_x, -1.0f, 1.0f);
    move_input.left_y = std::clamp(left_y, -1.0f, 1.0f);
    move_input.right_x = std::clamp(right_x, -1.0f, 1.0f);
    move_input.right_y = std::clamp(right_y, -1.0f, 1.0f);
    move_input.left_trigger = std::clamp(left_trigger, 0.0f, 1.0f);
    move_input.right_trigger = std::clamp(right_trigger, 0.0f, 1.0f);
    move_input.timestamp_ns = timestamp_ns;
}

void GuestVrSensor::ResetOrientation() {
    std::scoped_lock lock{mutex};
    snapshot.orientation_x = 0.0f;
    snapshot.orientation_y = 0.0f;
    snapshot.orientation_z = 0.0f;
    snapshot.orientation_w = 1.0f;
}

GuestVrSensor::Snapshot GuestVrSensor::Read() const {
    std::scoped_lock lock{mutex};
    return snapshot;
}

GuestVrSensor::MoveInputSnapshot GuestVrSensor::ReadMoveInput() const {
    std::scoped_lock lock{mutex};
    return move_input;
}

} // namespace Core::HostRuntime
