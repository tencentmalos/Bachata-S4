// SPDX-FileCopyrightText: Copyright 2026 shadPS4 Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#pragma once

#include <algorithm>
#include <array>
#include <atomic>
#include <cmath>
#include <cstdint>
#include <optional>
#include "core/host_runtime/guest_vr_sensor.h"
#include "core/host_runtime/vr_geometry.h"

namespace Core::HostRuntime {

// Where a PSVR title sees the player's DualShock 4. On a PS4 the camera tracks the light
// bar; here the headset's own controllers stand in for it (AstroQuest import plan D2, tier
// A), or the player's tracked hands around a real gamepad (tier B, H5). Sources:
//   Right: the right controller is the gamepad, held in that hand.
//   Both:  the two controllers held side by side like the two grips of a gamepad.
//   Hands: hand tracking finds the two palms closed around a gamepad's grips.
// When nothing places the gamepad (controllers out of sight, put down, hands lost) it stays
// where the player keeps it: their own place if they set one, else the offset from the head
// where it was last seen, else the standard place in front of and below the head
// (AQ:core/vr/vr_runtime.cpp:878-946).
enum class Ds4PoseSource : std::uint8_t { Off = 0, Right = 1, Both = 2, Hands = 3 };

// Per game, set before the session starts (Android setting input.xr_ds4_pose). Defined with
// the VrTracker library, in the host library the JNI side calls into.
void SetDs4PoseSource(Ds4PoseSource source);
Ds4PoseSource GetDs4PoseSource();
// The PS camera is the origin of tracker space and a seated player is about 1.5 m in front
// of it (AQ:core/vr/vr_runtime.h:126 origin_offset). OpenXR LOCAL has its origin at the
// player's head instead. Titles that check the player's place in front of the camera
// (ASTRO BOT's seat calibration) refuse a head at the camera, so VrTracker results are
// moved by this offset. Both spaces face the camera along -Z, so no rotation is involved.
// The host keeps its own LOCAL poses for presentation. On by default; DebugBus
// `xr_tracking seat on|off`.
void SetTrackerSeatOffset(bool on);
bool TrackerSeatOffset();
// PS held + D-pad / L1 / R1 moves the player's own place for the gamepad (2 cm a press),
// PS + Triangle switches between it and the seen/standard place. Kept in the user directory
// as vr_controller.json across sessions and games.
void MoveDs4OwnPlace(const std::array<float, 3>& by);
void SwitchDs4OwnPlace();
// Right-controller mode: the gamepad middle in the right grip's own axes (metres). The grip
// origin differs between headsets, so it can be tuned while wearing one (DebugBus
// `xr_tracking ds4_offset X Y Z`); it lasts for the process.
void SetDs4RightOffset(const std::array<float, 3>& offset);
std::array<float, 3> Ds4RightOffset();

class Ds4Placement final {
public:
    using Pose = GuestVrSensor::Pose;
    using Vec3 = std::array<float, 3>;
    using Quat = std::array<float, 4>;

    // Where a title expects the gamepad before it was ever seen: below and in front of the
    // head, in tracking space (not turned with the head: hands do not swing when the player
    // looks about).
    static constexpr Vec3 StandardOffset{0.0f, -0.17f, -0.50f};
    // The middle of a gamepad held in the right hand, from that hand's grip: to the left and
    // a little forward. Inferred from a DualShock 4's width (16 cm between the grips).
    static constexpr Vec3 RightGripToPad{-0.08f, 0.0f, -0.02f};
    // The two grips close behind and below the middle of the gamepad (as AQ's palm rule).
    static constexpr float BothForward = 0.035f, BothUp = 0.015f;
    // Hands further apart or closer together than this do not hold one gamepad.
    static constexpr float BothMinSpan = 0.05f, BothMaxSpan = 0.32f;
    // A sample this old no longer counts as seeing the gamepad.
    static constexpr std::uint64_t SeenWindowNs = 400'000'000;
    static constexpr float AnchorFollowS = 0.25f, SeenFollowS = 0.035f, UnseenFollowS = 0.30f;

    struct Result {
        Pose pose{};
        bool seen{};   // placed by a controller right now
        bool placed{}; // any pose at all (false: no head yet)
    };

    // `grips` are the runtime's controller grips and `palms` the tracked palms (index 0 left,
    // 1 right); `head` the headset. Call it whenever the title asks; time only has to grow.
    Result Update(Ds4PoseSource source, const Pose& head, const std::array<Pose, 2>& grips,
                  const std::array<bool, 2>& active, std::uint64_t now_ns,
                  const std::array<Pose, 2>& palms = {}) {
        Result result{};
        if (source == Ds4PoseSource::Off || !head.position_valid)
            return result;
        const float since = last_ns ? std::clamp(float(now_ns - last_ns) * 1e-9f, 0.f, 1.f) : 0.f;
        last_ns = now_ns;
        const auto blend = [since](float follow) { return 1.f - std::exp(-since / follow); };

        // The anchor trails the head position, smoothed so a nod does not move the pad.
        if (!anchor) {
            anchor = head.position;
        } else {
            const float b = blend(AnchorFollowS);
            for (unsigned i = 0; i < 3; ++i)
                (*anchor)[i] += (head.position[i] - (*anchor)[i]) * b;
        }

        if (auto sample = Locate(source, grips, active, palms)) {
            if (source == Ds4PoseSource::Hands) {
                // Palm joints carry no velocity: follow successive positions (AQ blends 0.4).
                const float dt = float(now_ns - seen_ns) * 1e-9f;
                if (seen_ns && dt > 0.f && dt < 0.1f) {
                    for (unsigned i = 0; i < 3; ++i)
                        hands_velocity[i] += ((sample->position[i] - seen_pose.position[i]) / dt -
                                              hands_velocity[i]) * 0.4f;
                } else {
                    hands_velocity = {};
                }
                sample->linear_velocity = hands_velocity;
            }
            seen_pose = *sample;
            seen_ns = now_ns;
            last_orientation = sample->orientation;
            for (unsigned i = 0; i < 3; ++i)
                seen_offset[i] = sample->position[i] - (*anchor)[i];
            seen_offset_valid = true;
        }
        const bool seen = seen_ns && now_ns - seen_ns < SeenWindowNs;
        const Vec3& assumed = own_offset && own_used ? *own_offset
                              : seen_offset_valid ? seen_offset
                                                  : StandardOffset;
        Vec3 goal{};
        for (unsigned i = 0; i < 3; ++i)
            goal[i] = seen ? seen_pose.position[i] : (*anchor)[i] + assumed[i];
        // Glide to the goal: sight of the controllers comes and goes, and a gamepad that
        // teleports is worse than one that lags a little.
        if (!shown) {
            shown = goal;
        } else {
            const float b = blend(seen ? SeenFollowS : UnseenFollowS);
            for (unsigned i = 0; i < 3; ++i)
                (*shown)[i] += (goal[i] - (*shown)[i]) * b;
        }

        result.placed = true;
        result.seen = seen;
        result.pose.position = *shown;
        result.pose.position_valid = true;
        result.pose.orientation = last_orientation;
        result.pose.orientation_valid = true;
        if (seen) {
            result.pose.linear_velocity = seen_pose.linear_velocity;
            result.pose.angular_velocity = seen_pose.angular_velocity;
        }
        return result;
    }

    // The player says where they keep the gamepad (relative to the head anchor); it counts for
    // more than where it was last seen. nullopt goes back to the seen/standard places.
    void SetOwnOffset(std::optional<Vec3> offset, bool used = true) {
        own_offset = offset ? std::optional<Vec3>{WithinReach(*offset)} : std::nullopt;
        own_used = own_offset && used;
    }
    // Moves the player's own place by `by` (starting from where the gamepad is assumed to be
    // now) and uses it. Returns the new place.
    Vec3 MoveOwnOffset(const Vec3& by) {
        const Vec3 base = own_offset && own_used ? *own_offset
                          : seen_offset_valid    ? seen_offset
                                                 : StandardOffset;
        own_offset = WithinReach({base[0] + by[0], base[1] + by[1], base[2] + by[2]});
        own_used = true;
        return *own_offset;
    }
    // Between the player's own place and the seen/standard one. False: there is no own place.
    bool SwitchOwnPlace() {
        if (!own_offset)
            return false;
        own_used = !own_used;
        return true;
    }
    [[nodiscard]] std::optional<Vec3> OwnOffset() const {
        return own_offset;
    }
    [[nodiscard]] bool OwnPlaceUsed() const {
        return own_offset && own_used;
    }
    // Within reach and in front of the player (AQ:core/vr/vr_runtime.cpp WithinReach).
    static Vec3 WithinReach(const Vec3& v) {
        return {std::clamp(v[0], -0.40f, 0.40f), std::clamp(v[1], -0.70f, 0.30f),
                std::clamp(v[2], -0.90f, -0.15f)};
    }

    // The title recalibrated (or the view was reset): whatever the gamepad points at now is
    // straight ahead and level. Keeps the heading of the head, not its pitch or roll.
    void Recalibrate(const Pose& head) {
        recalibrate_pending = true;
        recalibrate_yaw = YawOnly(head.orientation);
    }

    // A new session: nothing seen, nothing calibrated. The player's own place stays, as does
    // a tuned right-controller offset.
    void Reset() {
        const auto own = own_offset;
        const bool used = own_used;
        const auto right = right_offset;
        *this = Ds4Placement{};
        own_offset = own;
        own_used = used;
        right_offset = right;
    }
    void SetRightOffset(const Vec3& offset) {
        right_offset = offset;
    }
    [[nodiscard]] Vec3 RightOffset() const {
        return right_offset;
    }

private:
    std::optional<Pose> Locate(Ds4PoseSource source, const std::array<Pose, 2>& grips,
                               const std::array<bool, 2>& active, const std::array<Pose, 2>& palms) {
        if (source == Ds4PoseSource::Hands) {
            const auto usable = [](const Pose& p) { return p.position_valid && p.orientation_valid; };
            if (!usable(palms[0]) || !usable(palms[1]))
                return std::nullopt;
            return BetweenHands(palms[0], palms[1], true);
        }
        const auto usable = [&](unsigned hand) {
            return active[hand] && grips[hand].position_valid && grips[hand].orientation_valid;
        };
        if (source == Ds4PoseSource::Right) {
            if (!usable(1))
                return std::nullopt;
            const auto& grip = grips[1];
            Pose pose = grip;
            const auto r = VrGeometry::Rotate(grip.orientation, right_offset);
            for (unsigned i = 0; i < 3; ++i)
                pose.position[i] += r[i];
            // v_pad = v_grip + w x r for the same rigid body.
            const auto& w = grip.angular_velocity;
            pose.linear_velocity[0] += w[1] * r[2] - w[2] * r[1];
            pose.linear_velocity[1] += w[2] * r[0] - w[0] * r[2];
            pose.linear_velocity[2] += w[0] * r[1] - w[1] * r[0];
            pose.orientation = Calibrated(grip.orientation, DefaultPitch);
            return pose;
        }
        if (!usable(0) || !usable(1))
            return std::nullopt;
        return BetweenHands(grips[0], grips[1], false);
    }

    // A gamepad held between two hands: `palms` says the poses are palm joints (their +Y
    // points out of the back of the hand), otherwise controller grips (their -Z points along
    // the fist).
    std::optional<Pose> BetweenHands(const Pose& left, const Pose& right, bool palms) {
        const Vec3 across{right.position[0] - left.position[0], right.position[1] - left.position[1],
                          right.position[2] - left.position[2]};
        const float span = std::sqrt(across[0] * across[0] + across[1] * across[1] +
                                     across[2] * across[2]);
        if (span < BothMinSpan || span > BothMaxSpan)
            return std::nullopt;
        // Hands one above the other do not hold a gamepad level enough to be one (AQ: the
        // horizontal part of the span is at least 60% of it).
        if (palms && std::sqrt(across[0] * across[0] + across[2] * across[2]) < 0.6f * span)
            return std::nullopt;
        Pose pose{};
        pose.position_valid = pose.orientation_valid = true;
        for (unsigned i = 0; i < 3; ++i) {
            pose.position[i] = (left.position[i] + right.position[i]) * 0.5f;
            pose.linear_velocity[i] = (left.linear_velocity[i] + right.linear_velocity[i]) * 0.5f;
        }
        // The line from the left to the right hand is the gamepad's sideways axis (heading
        // and roll). Its tilt comes from the two hands together: the backs of the hands give
        // its up, the grips' mean forward its forward.
        const Quat mean = Mean(left.orientation, right.orientation);
        const Vec3 x{across[0] / span, across[1] / span, across[2] / span};
        const auto orthogonal = [&](Vec3 v) -> std::optional<Vec3> {
            const float d = v[0] * x[0] + v[1] * x[1] + v[2] * x[2];
            for (unsigned i = 0; i < 3; ++i)
                v[i] -= d * x[i];
            const float l = std::sqrt(v[0] * v[0] + v[1] * v[1] + v[2] * v[2]);
            if (l < 1e-3f)
                return std::nullopt;
            for (auto& c : v)
                c /= l;
            return v;
        };
        Vec3 y{}, z{};
        if (palms) {
            const auto up = orthogonal(VrGeometry::Rotate(mean, {0, 1, 0}));
            if (!up)
                return std::nullopt;
            y = *up;
            z = {x[1] * y[2] - x[2] * y[1], x[2] * y[0] - x[0] * y[2], x[0] * y[1] - x[1] * y[0]};
        } else {
            const auto f = orthogonal(VrGeometry::Rotate(mean, {0, 0, -1}));
            if (!f)
                return std::nullopt;
            z = {-(*f)[0], -(*f)[1], -(*f)[2]};
            y = {z[1] * x[2] - z[2] * x[1], z[2] * x[0] - z[0] * x[2], z[0] * x[1] - z[1] * x[0]};
        }
        pose.orientation = Calibrated(FromAxes(x, y, z), palms ? 0.f : DefaultPitch);
        // The hands close behind and below the middle of the gamepad: the middle is forward
        // (-Z) and up (+Y) of them.
        const auto ahead = VrGeometry::Rotate(pose.orientation, {0, BothUp, -BothForward});
        for (unsigned i = 0; i < 3; ++i)
            pose.position[i] += ahead[i];
        const auto& wl = left.angular_velocity;
        const auto& wr = right.angular_velocity;
        pose.angular_velocity = {(wl[0] + wr[0]) * 0.5f, (wl[1] + wr[1]) * 0.5f,
                                 (wl[2] + wr[2]) * 0.5f};
        return pose;
    }

    // The raw controller orientation, turned so that the gamepad reads level and straight
    // ahead when the title last recalibrated. Before any recalibration a held controller
    // points forward and down by about this much: rotate it back up. Hands need none.
    static constexpr float DefaultPitch = 0.6981317f; // 40 degrees, inferred
    Quat Calibrated(const Quat& raw, float default_pitch) {
        // The correction is in the controller's own frame (applied on the right), so turning
        // the controller about any world axis turns the gamepad about the same axis.
        if (recalibrate_pending) {
            // raw * correction == recalibrate_yaw  =>  correction = conj(raw) * yaw
            correction = VrGeometry::Multiply(Conjugate(raw), recalibrate_yaw);
            has_correction = true;
            recalibrate_pending = false;
        }
        if (has_correction)
            return Normalize(VrGeometry::Multiply(raw, correction));
        const Quat pitch{std::sin(default_pitch * 0.5f), 0, 0, std::cos(default_pitch * 0.5f)};
        return Normalize(VrGeometry::Multiply(raw, pitch));
    }

    static Quat Conjugate(const Quat& q) {
        return {-q[0], -q[1], -q[2], q[3]};
    }
    static Quat Normalize(Quat q) {
        const float n = std::sqrt(q[0] * q[0] + q[1] * q[1] + q[2] * q[2] + q[3] * q[3]);
        if (n < 1e-6f)
            return {0, 0, 0, 1};
        for (auto& v : q)
            v /= n;
        return q;
    }
    static Quat Mean(const Quat& a, Quat b) {
        if (a[0] * b[0] + a[1] * b[1] + a[2] * b[2] + a[3] * b[3] < 0)
            for (auto& v : b)
                v = -v;
        return Normalize({a[0] + b[0], a[1] + b[1], a[2] + b[2], a[3] + b[3]});
    }
    static Quat YawOnly(const Quat& q) {
        const auto f = VrGeometry::Rotate(q, {0, 0, -1});
        const float yaw = std::atan2(-f[0], -f[2]);
        return {0, std::sin(yaw * 0.5f), 0, std::cos(yaw * 0.5f)};
    }
    // Rotation whose columns are the given orthonormal axes.
    static Quat FromAxes(const Vec3& x, const Vec3& y, const Vec3& z) {
        const float m00 = x[0], m11 = y[1], m22 = z[2];
        const float trace = m00 + m11 + m22;
        Quat q{};
        if (trace > 0) {
            const float s = std::sqrt(trace + 1.f) * 2.f;
            q = {(y[2] - z[1]) / s, (z[0] - x[2]) / s, (x[1] - y[0]) / s, 0.25f * s};
        } else if (m00 > m11 && m00 > m22) {
            const float s = std::sqrt(1.f + m00 - m11 - m22) * 2.f;
            q = {0.25f * s, (y[0] + x[1]) / s, (z[0] + x[2]) / s, (y[2] - z[1]) / s};
        } else if (m11 > m22) {
            const float s = std::sqrt(1.f + m11 - m00 - m22) * 2.f;
            q = {(y[0] + x[1]) / s, 0.25f * s, (z[1] + y[2]) / s, (z[0] - x[2]) / s};
        } else {
            const float s = std::sqrt(1.f + m22 - m00 - m11) * 2.f;
            q = {(z[0] + x[2]) / s, (z[1] + y[2]) / s, 0.25f * s, (x[1] - y[0]) / s};
        }
        return Normalize(q);
    }

    std::optional<Vec3> anchor, shown, own_offset;
    Vec3 seen_offset{}, hands_velocity{};
    Vec3 right_offset{RightGripToPad};
    bool seen_offset_valid{}, own_used{};
    Pose seen_pose{};
    std::uint64_t seen_ns{}, last_ns{};
    Quat last_orientation{0, 0, 0, 1};
    Quat correction{0, 0, 0, 1}, recalibrate_yaw{0, 0, 0, 1};
    bool has_correction{}, recalibrate_pending{};
};

} // namespace Core::HostRuntime
