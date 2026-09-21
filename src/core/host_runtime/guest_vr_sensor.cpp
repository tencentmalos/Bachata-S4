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
    if (!snapshot.enabled || !std::isfinite(x) || !std::isfinite(y) || !std::isfinite(z) ||
        timestamp_ns == 0 || timestamp_ns <= snapshot.timestamp_ns)
        return;
    const auto previous = snapshot.timestamp_ns;
    snapshot.timestamp_ns = timestamp_ns;
    snapshot.angular_velocity_x = x;
    snapshot.angular_velocity_y = y;
    snapshot.angular_velocity_z = z;
    // First delivery and a delivery gap establish a new time origin. Applying
    // a rate measured after an Activity pause to the missing interval is wrong.
    if (previous == 0 || timestamp_ns - previous > 100'000'000ull)
        return;
    const double dt = static_cast<double>(timestamp_ns - previous) * 1e-9;
    const double speed = std::hypot(double(x), double(y), double(z));
    if (speed == 0.0)
        return;
    const double half_angle = speed * dt * 0.5;
    const double scale = std::sin(half_angle) / speed;
    const double dx = x * scale, dy = y * scale, dz = z * scale;
    const double dw = std::cos(half_angle);
    const double qx = snapshot.orientation_x, qy = snapshot.orientation_y;
    const double qz = snapshot.orientation_z, qw = snapshot.orientation_w;
    // Body-space angular velocity: compose on the right with the exact
    // axis-angle increment, then normalize to bound accumulated float error.
    const double nx = qw * dx + qx * dw + qy * dz - qz * dy;
    const double ny = qw * dy - qx * dz + qy * dw + qz * dx;
    const double nz = qw * dz + qx * dy - qy * dx + qz * dw;
    const double nw = qw * dw - qx * dx - qy * dy - qz * dz;
    const double norm = std::sqrt(nx * nx + ny * ny + nz * nz + nw * nw);
    snapshot.orientation_x = static_cast<float>(nx / norm);
    snapshot.orientation_y = static_cast<float>(ny / norm);
    snapshot.orientation_z = static_cast<float>(nz / norm);
    snapshot.orientation_w = static_cast<float>(nw / norm);
}

void GuestVrSensor::UpdateMoveInput(std::uint64_t buttons, float left_x, float left_y,
                                    float right_x, float right_y, float left_trigger,
                                    float right_trigger, std::uint64_t timestamp_us) {
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
    move_input.timestamp_us = timestamp_us;
}

void GuestVrSensor::ResetOrientation() {
    std::scoped_lock lock{mutex};
    snapshot.orientation_x = 0.0f;
    snapshot.orientation_y = 0.0f;
    snapshot.orientation_z = 0.0f;
    snapshot.orientation_w = 1.0f;
    snapshot.angular_velocity_x = 0.0f;
    snapshot.angular_velocity_y = 0.0f;
    snapshot.angular_velocity_z = 0.0f;
    snapshot.timestamp_ns = 0;
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
