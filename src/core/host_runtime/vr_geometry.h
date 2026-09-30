// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once
#include <array>
#include <cmath>
#include "core/host_runtime/guest_vr_sensor.h"

namespace Core::HostRuntime::VrGeometry {
inline constexpr float CinemaDistance = 2.5f;
inline constexpr float CinemaWidth = 3.2f;
inline float CinemaHeight(float aspect) {
    return CinemaWidth / ((std::isfinite(aspect) && aspect > 0) ? aspect : 16.f / 9.f);
}
// Anchor once at eye height, looking horizontally along the viewer's initial yaw.
// This makes the distance relative to the person, not the runtime's LOCAL origin.
inline GuestVrSensor::Pose CinemaPlacement(const GuestVrSensor::Pose& head) {
    const auto& q = head.orientation;
    const float forward_x = -2 * (q[0] * q[2] + q[3] * q[1]);
    const float forward_z = 2 * (q[0] * q[0] + q[1] * q[1]) - 1;
    const float length = std::hypot(forward_x, forward_z);
    const float yaw = length > 1e-4f ? std::atan2(-forward_x, -forward_z) : 0.f;
    GuestVrSensor::Pose out;
    out.orientation = {0, std::sin(yaw / 2), 0, std::cos(yaw / 2)};
    out.position = {head.position[0] - std::sin(yaw) * CinemaDistance, head.position[1],
                    head.position[2] - std::cos(yaw) * CinemaDistance};
    out.orientation_valid = out.position_valid = true;
    return out;
}
inline std::array<float, 3> HeadLocalEye(const GuestVrSensor::Pose& head,
                                       const GuestVrSensor::Pose& eye) {
    const auto& q = head.orientation;
    const float x = eye.position[0] - head.position[0];
    const float y = eye.position[1] - head.position[1];
    const float z = eye.position[2] - head.position[2];
    const float tx = 2 * (q[2] * y - q[1] * z);
    const float ty = 2 * (q[0] * z - q[2] * x);
    const float tz = 2 * (q[1] * x - q[0] * y);
    return {x + q[3] * tx - q[1] * tz + q[2] * ty,
            y + q[3] * ty - q[2] * tx + q[0] * tz,
            z + q[3] * tz - q[0] * ty + q[1] * tx};
}

inline std::array<float, 4> Multiply(const std::array<float, 4>& a,
                                    const std::array<float, 4>& b) {
    return {a[3]*b[0]+a[0]*b[3]+a[1]*b[2]-a[2]*b[1],
            a[3]*b[1]-a[0]*b[2]+a[1]*b[3]+a[2]*b[0],
            a[3]*b[2]+a[0]*b[1]-a[1]*b[0]+a[2]*b[3],
            a[3]*b[3]-a[0]*b[0]-a[1]*b[1]-a[2]*b[2]};
}

// VrTracker reports the Move's optical sphere, whereas OpenXR grip is the
// palm's holding point. Use a nominal virtual Move geometry, not the runtime's
// aim ray (whose origin and orientation have different meanings). 75 mm is the
// inverse of PSMoveSteamVRBridge's default sphere-to-holding-point displacement.
// This is a model dimension; headset/controller-specific physical calibration
// still needs an in-headset comparison.
inline constexpr float MoveGripToSphereMeters = .075f;
inline GuestVrSensor::Pose MoveSpherePose(const GuestVrSensor::Pose& grip) {
    auto out = grip;
    // Locating an offset point needs orientation even if grip position is valid.
    out.position_valid = grip.position_valid && grip.orientation_valid;
    if (!grip.orientation_valid) return out;
    const auto& q = grip.orientation;
    const auto r = Multiply(Multiply(q, {0, 0, -MoveGripToSphereMeters, 0}),
                            {-q[0], -q[1], -q[2], q[3]});
    for (unsigned i = 0; i < 3; ++i) out.position[i] += r[i];
    // Velocity must refer to the same rigid-body point: v_sphere = v_grip + w x r.
    const auto& w = grip.angular_velocity;
    out.linear_velocity[0] += w[1]*r[2] - w[2]*r[1];
    out.linear_velocity[1] += w[2]*r[0] - w[0]*r[2];
    out.linear_velocity[2] += w[0]*r[1] - w[1]*r[0];
    return out;
}

// Keep the runtime's eye extrinsics (including canted optics), but place them
// at the head pose submitted WITH the guest image. The current display pose
// must never be used as the camera pose of an older image.
inline GuestVrSensor::Pose RenderEye(const GuestVrSensor::Pose& render_head,
                                     const GuestVrSensor::Pose& calibration_head,
                                     const GuestVrSensor::Pose& calibration_eye) {
    GuestVrSensor::Pose out;
    if (!render_head.orientation_valid || !render_head.position_valid ||
        !calibration_head.orientation_valid || !calibration_head.position_valid ||
        !calibration_eye.orientation_valid || !calibration_eye.position_valid)
        return out;
    const auto offset = HeadLocalEye(calibration_head, calibration_eye);
    const auto& q = render_head.orientation;
    const auto rotated = Multiply(Multiply(q, {offset[0], offset[1], offset[2], 0}),
                                  {-q[0], -q[1], -q[2], q[3]});
    for (unsigned j = 0; j < 3; ++j)
        out.position[j] = render_head.position[j] + rotated[j];
    const auto& c = calibration_head.orientation;
    out.orientation = Multiply(q, Multiply({-c[0], -c[1], -c[2], c[3]},
                                           calibration_eye.orientation));
    out.orientation_valid = out.position_valid = true;
    return out;
}

inline bool ValidFov(const std::array<float, 4>& fov) {
    for (float angle : fov)
        if (!std::isfinite(angle) || std::abs(angle) >= 1.5707963f)
            return false;
    return fov[0] < fov[1] && fov[2] < fov[3];
}

inline std::array<float, 3> LocalAngularVelocity(const GuestVrSensor::Pose& pose) {
    if (!pose.orientation_valid) return {};
    const auto& q = pose.orientation;
    const auto& v = pose.angular_velocity;
    const auto local = Multiply(Multiply({-q[0], -q[1], -q[2], q[3]},
                                         {v[0], v[1], v[2], 0}), q);
    return {local[0], local[1], local[2]};
}

// FOV is left/right/down/up. Convert those output rays to the guest's source
// rectangle; a different headset FOV requires different UVs, not a stretched image.
inline std::array<float, 4> TangentUv(const float* transform,
                                     const std::array<float, 4>& fov) {
    const float l = std::tan(fov[0]), r = std::tan(fov[1]);
    const float d = std::tan(fov[2]), u = std::tan(fov[3]);
    return {transform[0] * (r-l), transform[1] * (u-d),
            transform[2] + transform[0] * l, transform[3] + transform[1] * d};
}
} // namespace Core::HostRuntime::VrGeometry
