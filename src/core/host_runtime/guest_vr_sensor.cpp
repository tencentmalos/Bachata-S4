// SPDX-FileCopyrightText: Copyright 2026 shadPS4 Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#include "core/host_runtime/guest_vr_sensor.h"

#include <algorithm>
#include <chrono>
#include <cmath>

namespace Core::HostRuntime {
namespace {
using Pose = GuestVrSensor::Pose;
uint64_t SensorNowNs() {
    return std::chrono::duration_cast<std::chrono::nanoseconds>(
               std::chrono::steady_clock::now().time_since_epoch())
        .count();
}
bool Fresh(uint64_t received) {
    const auto now = SensorNowNs();
    return received && now >= received && now - received <= 500'000'000;
}
void Sanitize(Pose& p) {
    double norm{};
    for (auto v : p.orientation)
        norm += double(v) * v;
    if (!std::isfinite(norm) || norm < .25 || norm > 4 || !p.orientation_valid) {
        p.orientation = {0, 0, 0, 1};
        p.orientation_valid = false;
    } else
        for (auto& v : p.orientation)
            v /= std::sqrt(norm);
    if (!p.position_valid ||
        !std::ranges::all_of(p.position, [](float v) { return std::isfinite(v); })) {
        p.position = {};
        p.position_valid = false;
    }
    for (auto* vec : {&p.angular_velocity, &p.linear_velocity})
        for (auto& v : *vec)
            if (!std::isfinite(v))
                v = 0;
}
float FiniteClamp(float v, float low, float high) {
    return std::isfinite(v) ? std::clamp(v, low, high) : 0.f;
}
} // namespace

GuestVrSensor& GuestVrSensor::Instance() {
    static GuestVrSensor sensor;
    return sensor;
}

void GuestVrSensor::SetSbsEnabled(bool enabled) {
    std::scoped_lock lock{mutex};
    if (snapshot.openxr)
        return; // Runtime owns tracking until its generation is retired.
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
    if (!snapshot.enabled || snapshot.openxr || !std::isfinite(x) || !std::isfinite(y) ||
        !std::isfinite(z) || timestamp_ns == 0 || timestamp_ns <= snapshot.timestamp_ns)
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
    if (snapshot.openxr)
        return; // Recenter belongs to the runtime's reference space.
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
    auto result = snapshot;
    const auto now = std::chrono::duration_cast<std::chrono::nanoseconds>(
                         std::chrono::steady_clock::now().time_since_epoch())
                         .count();
    if (result.openxr &&
        (now < result.hardware.received_ns || now - result.hardware.received_ns > 500'000'000)) {
        result.hardware.focused = false;
        result.hardware.head.orientation_valid = result.hardware.head.position_valid = false;
        for (auto& eye : result.hardware.eyes)
            eye.orientation_valid = eye.position_valid = false;
        for (auto& hand : result.hardware.hands)
            hand = {};
    }
    return result;
}

std::uint64_t GuestVrSensor::BeginOpenXr() {
    std::scoped_lock lock{mutex};
    snapshot = {};
    render_eyes = {};
    snapshot.enabled = snapshot.openxr = true;
    snapshot.hardware.generation = ++xr_generation;
    move_input = {};
    haptics = {-1, -1};
    return xr_generation;
}

void GuestVrSensor::EndOpenXr(std::uint64_t generation) {
    std::scoped_lock lock{mutex};
    if (generation != xr_generation)
        return;
    snapshot = {};
    move_input = {};
    haptics = {-1, -1};
}

bool GuestVrSensor::PublishOpenXr(const HardwareFrame& frame) {
    std::scoped_lock lock{mutex};
    if (!snapshot.openxr || frame.generation != xr_generation ||
        frame.received_ns < snapshot.hardware.received_ns)
        return false;
    snapshot.hardware = frame;
    auto& safe = snapshot.hardware;
    Sanitize(safe.head);
    for (auto& eye : safe.eyes)
        Sanitize(eye);
    for (auto& hand : safe.hands) {
        if (!hand.active) {
            hand = {};
            continue;
        }
        Sanitize(hand.grip);
        Sanitize(hand.aim);
        hand.stick_x = FiniteClamp(hand.stick_x, -1, 1);
        hand.stick_y = FiniteClamp(hand.stick_y, -1, 1);
        hand.trigger = FiniteClamp(hand.trigger, 0, 1);
        hand.squeeze = FiniteClamp(hand.squeeze, 0, 1);
    }
    if (!frame.focused || !frame.running) {
        for (auto& hand : snapshot.hardware.hands)
            hand = {};
        haptics = {-1, -1};
    }
    snapshot.orientation_x = safe.head.orientation[0];
    snapshot.orientation_y = safe.head.orientation[1];
    snapshot.orientation_z = safe.head.orientation[2];
    snapshot.orientation_w = safe.head.orientation[3];
    snapshot.timestamp_ns = frame.predicted_ns;
    snapshot.angular_velocity_x = safe.head.angular_velocity[0];
    snapshot.angular_velocity_y = safe.head.angular_velocity[1];
    snapshot.angular_velocity_z = safe.head.angular_velocity[2];
    return true;
}

void GuestVrSensor::RecordHmdQuery(const HardwareFrame& frame) {
    std::scoped_lock lock{mutex};
    if (snapshot.openxr && frame.generation == xr_generation)
        render_eyes = frame.eyes;
}

std::array<GuestVrSensor::Pose, 2> GuestVrSensor::RenderEyes() const {
    std::scoped_lock lock{mutex};
    return snapshot.openxr ? render_eyes : std::array<Pose, 2>{};
}

bool GuestVrSensor::RequestHaptic(unsigned hand, float amplitude) {
    std::scoped_lock lock{mutex};
    if (!snapshot.openxr || !snapshot.hardware.focused || !Fresh(snapshot.hardware.received_ns) ||
        hand >= 2 || !snapshot.hardware.hands[hand].active || !std::isfinite(amplitude))
        return false;
    haptics[hand] = std::clamp(amplitude, 0.f, 1.f);
    return true;
}

std::array<float, 2> GuestVrSensor::TakeHaptics(std::uint64_t generation) {
    std::scoped_lock lock{mutex};
    if (!snapshot.openxr || generation != xr_generation || !Fresh(snapshot.hardware.received_ns))
        return {-1, -1};
    auto result = haptics;
    haptics = {-1, -1};
    return result;
}

GuestVrSensor::MoveInputSnapshot GuestVrSensor::ReadMoveInput() const {
    std::scoped_lock lock{mutex};
    return move_input;
}

} // namespace Core::HostRuntime
