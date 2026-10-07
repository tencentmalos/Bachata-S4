// SPDX-License-Identifier: GPL-2.0-or-later
// Ds4Placement: where a PSVR title sees the DualShock 4 when the headset's controllers stand
// in for the light bar (AstroQuest import plan D2, tier A).
#include <cmath>
#include <cstdio>
#include "core/host_runtime/ds4_placement.h"
using namespace Core::HostRuntime;
using Pose = GuestVrSensor::Pose;
using Vec3 = std::array<float, 3>;
using Quat = std::array<float, 4>;

namespace {
unsigned checks{}, failures{};
void Check(const char* name, bool value) {
    ++checks;
    failures += !value;
    if (!value)
        std::printf("FAIL %s\n", name);
}
bool Near(float a, float b, float eps = 1e-3f) {
    return std::fabs(a - b) <= eps;
}
bool Near(const Vec3& a, const Vec3& b, float eps = 1e-3f) {
    return Near(a[0], b[0], eps) && Near(a[1], b[1], eps) && Near(a[2], b[2], eps);
}
Pose At(Vec3 position, Quat orientation = {0, 0, 0, 1}) {
    Pose pose{};
    pose.position = position;
    pose.orientation = orientation;
    pose.position_valid = pose.orientation_valid = true;
    return pose;
}
Quat Yaw(float radians) {
    return {0, std::sin(radians * 0.5f), 0, std::cos(radians * 0.5f)};
}
Vec3 Forward(const Quat& q) {
    return VrGeometry::Rotate(q, {0, 0, -1});
}
constexpr std::uint64_t Ms = 1'000'000;
} // namespace

int main() {
    const Pose head = At({0, 1.2f, 0});
    const std::array<bool, 2> none{false, false}, both{true, true}, right_only{false, true};

    { // Off places nothing; no head places nothing.
        Ds4Placement p;
        Check("off", !p.Update(Ds4PoseSource::Off, head, {}, both, 1 * Ms).placed);
        Pose no_head{};
        Check("no head", !p.Update(Ds4PoseSource::Right, no_head, {}, both, 2 * Ms).placed);
    }

    { // Never seen: the standard place below and in front of the head, level.
        Ds4Placement p;
        const auto r = p.Update(Ds4PoseSource::Right, head, {}, none, 1 * Ms);
        Check("standard placed", r.placed && !r.seen);
        Check("standard position", Near(r.pose.position, {0, 1.2f - 0.17f, -0.5f}));
        Check("standard orientation", Near(r.pose.orientation[3], 1.f));
    }

    { // Right hand: grip plus the hand-to-middle offset, rotated with the grip.
        Ds4Placement p;
        std::array<Pose, 2> grips{};
        grips[1] = At({0.2f, 1.0f, -0.4f});
        const auto r = p.Update(Ds4PoseSource::Right, head, grips, right_only, 1 * Ms);
        Check("right seen", r.seen);
        Check("right position", Near(r.pose.position, {0.2f - 0.08f, 1.0f, -0.42f}));
        // Turned 90 degrees left: the offset (to the left of the hand) now points backwards.
        Ds4Placement q;
        grips[1] = At({0.2f, 1.0f, -0.4f}, Yaw(1.5707963f));
        const auto t = q.Update(Ds4PoseSource::Right, head, grips, right_only, 1 * Ms);
        Check("right turned position", Near(t.pose.position, {0.2f - 0.02f, 1.0f, -0.4f + 0.08f}));
        // Uncalibrated, a grip that points level reads tilted up by the default pitch.
        const auto f = Forward(r.pose.orientation);
        Check("default pitch up", f[1] > 0.6f && f[1] < 0.66f);
    }

    { // Recalibrate: the current controller direction becomes the head's heading, level.
        Ds4Placement p;
        std::array<Pose, 2> grips{};
        const Quat tilted = VrGeometry::Multiply(Yaw(0.3f), Quat{-0.3f, 0, 0, 0.9539392f});
        grips[1] = At({0.2f, 1.0f, -0.4f}, tilted);
        p.Recalibrate(At({0, 1.2f, 0}, Yaw(-0.5f)));
        const auto r = p.Update(Ds4PoseSource::Right, head, grips, right_only, 1 * Ms);
        const auto f = Forward(r.pose.orientation);
        Check("recalibrated level", Near(f[1], 0.f));
        Check("recalibrated heading", Near(std::atan2(-f[0], -f[2]), -0.5f));
        // Then the controller turning 0.2 rad further turns the pad as much.
        grips[1].orientation = VrGeometry::Multiply(Yaw(0.2f), tilted);
        const auto t = p.Update(Ds4PoseSource::Right, head, grips, right_only, 2 * Ms);
        const auto g = Forward(t.pose.orientation);
        Check("follows after calibration", Near(std::atan2(-g[0], -g[2]), -0.3f, 2e-3f));
    }

    { // Both hands: the middle, ahead of and above the grips; sideways axis left -> right.
        Ds4Placement p;
        std::array<Pose, 2> grips{At({-0.08f, 1.0f, -0.4f}), At({0.08f, 1.0f, -0.4f})};
        p.Recalibrate(head);
        const auto r = p.Update(Ds4PoseSource::Both, head, grips, both, 1 * Ms);
        Check("both seen", r.seen);
        Check("both position", Near(r.pose.position, {0, 1.015f, -0.435f}));
        const auto x = VrGeometry::Rotate(r.pose.orientation, {1, 0, 0});
        Check("both sideways", Near(x, {1, 0, 0}));
        // Too far apart: not one gamepad, so not seen.
        Ds4Placement q;
        grips = {At({-0.3f, 1.0f, -0.4f}), At({0.3f, 1.0f, -0.4f})};
        Check("both too wide", !q.Update(Ds4PoseSource::Both, head, grips, both, 1 * Ms).seen);
        // Only one hand: not seen either.
        Ds4Placement s;
        grips = {At({-0.08f, 1.0f, -0.4f}), At({0.08f, 1.0f, -0.4f})};
        Check("both one hand", !s.Update(Ds4PoseSource::Both, head, grips, right_only, 1 * Ms).seen);
        // A roll of the hands rolls the pad.
        Ds4Placement u;
        grips = {At({-0.08f, 0.96f, -0.4f}), At({0.08f, 1.04f, -0.4f})};
        const auto rolled = u.Update(Ds4PoseSource::Both, head, grips, both, 1 * Ms);
        const auto rx = VrGeometry::Rotate(rolled.pose.orientation, {1, 0, 0});
        Check("both roll", rx[1] > 0.4f);
    }

    { // Lost from sight: it stays where it was held relative to the head, and follows the
      // head's position (not its rotation) when the player moves.
        Ds4Placement p;
        std::array<Pose, 2> grips{};
        grips[1] = At({0.3f, 1.0f, -0.3f});
        auto r = p.Update(Ds4PoseSource::Right, head, grips, right_only, 1 * Ms);
        const Vec3 held = r.pose.position;
        // Still within the 400 ms window: still seen.
        r = p.Update(Ds4PoseSource::Right, head, grips, none, 300 * Ms);
        Check("seen window", r.seen);
        // Long after: unseen, but at the same offset from the (unchanged) head.
        for (std::uint64_t t = 500; t <= 3000; t += 100)
            r = p.Update(Ds4PoseSource::Right, head, grips, none, t * Ms);
        Check("unseen", !r.seen);
        Check("kept place", Near(r.pose.position, held, 2e-3f));
        // The player moves 0.5 m to the right; the pad follows over a second or so.
        const Pose moved = At({0.5f, 1.2f, 0}, Yaw(1.0f));
        for (std::uint64_t t = 3100; t <= 6000; t += 100)
            r = p.Update(Ds4PoseSource::Right, moved, grips, none, t * Ms);
        Check("follows head position",
              Near(r.pose.position, {held[0] + 0.5f, held[1], held[2]}, 5e-3f));
        // The player's own place beats the last seen one.
        p.SetOwnOffset(Vec3{0, -0.3f, -0.45f});
        for (std::uint64_t t = 6100; t <= 9000; t += 100)
            r = p.Update(Ds4PoseSource::Right, moved, grips, none, t * Ms);
        Check("own place", Near(r.pose.position, {0.5f, 0.9f, -0.45f}, 5e-3f));
    }

    { // Reappearing glides rather than jumping.
        Ds4Placement p;
        std::array<Pose, 2> grips{};
        auto r = p.Update(Ds4PoseSource::Right, head, grips, none, 1 * Ms);
        grips[1] = At({0.5f, 1.0f, -0.3f});
        r = p.Update(Ds4PoseSource::Right, head, grips, right_only, 11 * Ms);
        Check("glide partial", r.pose.position[0] > 0.f && r.pose.position[0] < 0.42f);
        for (std::uint64_t t = 21; t <= 400; t += 10)
            r = p.Update(Ds4PoseSource::Right, head, grips, right_only, t * Ms);
        Check("glide arrives", Near(r.pose.position, {0.42f, 1.0f, -0.32f}, 2e-3f));
    }

    { // Velocity of the pad point includes the grip's spin: v + w x r.
        Ds4Placement p;
        std::array<Pose, 2> grips{};
        grips[1] = At({0, 1, -0.4f});
        grips[1].angular_velocity = {0, 1, 0};
        const auto r = p.Update(Ds4PoseSource::Right, head, grips, right_only, 1 * Ms);
        // r = (-0.08, 0, -0.02); w x r = (1*-0.02 - 0, 0, 0 - 1*-0.08) = (-0.02, 0, 0.08)
        Check("velocity", Near(r.pose.linear_velocity, {-0.02f, 0, 0.08f}));
    }

    { // Hands: the palms around a gamepad. The middle sits ahead of and above them; no
      // controller pitch correction is needed.
        Ds4Placement p;
        std::array<Pose, 2> palms{At({-0.08f, 1.0f, -0.4f}), At({0.08f, 1.0f, -0.4f})};
        const auto r = p.Update(Ds4PoseSource::Hands, head, {}, none, 1 * Ms, palms);
        Check("hands seen", r.seen);
        Check("hands position", Near(r.pose.position, {0, 1.015f, -0.435f}));
        Check("hands level", Near(Forward(r.pose.orientation), {0, 0, -1}));
        // The backs of the hands tipped 30 degrees towards the player: the gamepad points up.
        Ds4Placement q;
        const Quat back{std::sin(0.2617994f), 0, 0, std::cos(0.2617994f)};
        palms = {At({-0.08f, 1.0f, -0.4f}, back), At({0.08f, 1.0f, -0.4f}, back)};
        const auto t = q.Update(Ds4PoseSource::Hands, head, {}, none, 1 * Ms, palms);
        Check("hands pitch", Near(Forward(t.pose.orientation)[1], 0.5f));
        // One hand above the other is not a gamepad held level.
        Ds4Placement u;
        palms = {At({-0.03f, 0.9f, -0.4f}), At({0.03f, 1.05f, -0.4f})};
        Check("hands not level", !u.Update(Ds4PoseSource::Hands, head, {}, none, 1 * Ms, palms).seen);
        // Hands ignore the headset's controllers, and controllers ignore the hands.
        Ds4Placement v;
        std::array<Pose, 2> grips{At({-0.08f, 1.0f, -0.4f}), At({0.08f, 1.0f, -0.4f})};
        Check("hands ignore grips", !v.Update(Ds4PoseSource::Hands, head, grips, both, 1 * Ms).seen);
        Ds4Placement w;
        palms = {At({-0.08f, 1.0f, -0.4f}), At({0.08f, 1.0f, -0.4f})};
        Check("grips ignore hands", !w.Update(Ds4PoseSource::Both, head, {}, none, 1 * Ms, palms).seen);
        // Velocity from successive palm positions: moving 1 cm in 10 ms is 1 m/s; the blend
        // reaches it over a few samples.
        Ds4Placement m;
        Ds4Placement::Result mv{};
        for (int i = 0; i <= 10; ++i) {
            const float x = 0.01f * float(i);
            palms = {At({-0.08f + x, 1.0f, -0.4f}), At({0.08f + x, 1.0f, -0.4f})};
            mv = m.Update(Ds4PoseSource::Hands, head, {}, none, std::uint64_t(1 + 10 * i) * Ms, palms);
        }
        Check("hands velocity", Near(mv.pose.linear_velocity[0], 1.0f, 0.02f) &&
                                    Near(mv.pose.linear_velocity[1], 0.f));
    }

    { // The player's own place: moved in steps from where the gamepad is assumed to be, kept
      // within reach, switched off and on, and kept across a new session.
        Ds4Placement p;
        auto own = p.MoveOwnOffset({0, 0.02f, 0}); // from the standard place
        Check("own from standard", Near(own, {0, -0.15f, -0.5f}));
        own = p.MoveOwnOffset({0, 0, 1.0f});
        Check("own within reach", Near(own[2], -0.15f));
        Check("own used", p.OwnPlaceUsed());
        auto r = p.Update(Ds4PoseSource::Hands, head, {}, none, 1 * Ms);
        Check("own place applies", Near(r.pose.position, {0, 1.05f, -0.15f}));
        Check("switch", p.SwitchOwnPlace() && !p.OwnPlaceUsed());
        for (std::uint64_t t = 100; t <= 3000; t += 100)
            r = p.Update(Ds4PoseSource::Hands, head, {}, none, t * Ms);
        Check("switched to standard", Near(r.pose.position, {0, 1.03f, -0.5f}, 2e-3f));
        p.SwitchOwnPlace();
        p.Reset();
        Check("own kept over reset", p.OwnPlaceUsed() && p.OwnOffset() && Near(*p.OwnOffset(), own));
        Ds4Placement q;
        Check("no own place to switch to", !q.SwitchOwnPlace());
    }

    std::printf("ds4_placement_tests: %u checks, %u failures\n", checks, failures);
    return failures ? 1 : 0;
}
