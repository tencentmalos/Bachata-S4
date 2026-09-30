// SPDX-License-Identifier: GPL-2.0-or-later
#include <cmath>
#include <chrono>
#include <cstdio>
#include <limits>
#include <numbers>
#include "core/host_runtime/guest_vr_sensor.h"
#include "core/host_runtime/vr_geometry.h"
static unsigned checks{}, failures{};
#define CHECK(...) do { ++checks; if (!(__VA_ARGS__)) { ++failures; std::printf("FAIL %d: %s\n", __LINE__, #__VA_ARGS__); } } while (0)
static bool Near(float a, float b) { return std::abs(a - b) < 1e-5f; }
int main() {
    auto& sensor = Core::HostRuntime::GuestVrSensor::Instance();
    sensor.SetSbsEnabled(false);
    sensor.UpdateGyro(1, 2, 3, 1'000'000'000);
    CHECK(!sensor.Read().enabled && sensor.Read().timestamp_ns == 0);
    for (unsigned axis = 0; axis < 3; ++axis) {
        sensor.SetSbsEnabled(true);
        float rate[3]{}; rate[axis] = std::numbers::pi_v<float> / 2;
        sensor.UpdateGyro(rate[0], rate[1], rate[2], 1'000'000'000);
        CHECK(sensor.Read().orientation_w == 1);
        for (uint64_t t = 1'010'000'000; t <= 2'000'000'000; t += 10'000'000)
            sensor.UpdateGyro(rate[0], rate[1], rate[2], t);
        const auto pose = sensor.Read();
        const float xyz[]{pose.orientation_x, pose.orientation_y, pose.orientation_z};
        for (unsigned i = 0; i < 3; ++i) CHECK(Near(xyz[i], i == axis ? std::sqrt(0.5f) : 0));
        CHECK(Near(pose.orientation_w, std::sqrt(0.5f)));
        const auto eye = pose.EyeOffset(0.0315f);
        if (axis == 0) CHECK(Near(eye[0], 0.0315f) && Near(eye[1], 0) && Near(eye[2], 0));
        if (axis == 1) CHECK(Near(eye[0], 0) && Near(eye[1], 0) && Near(eye[2], -0.0315f));
        if (axis == 2) CHECK(Near(eye[0], 0) && Near(eye[1], 0.0315f) && Near(eye[2], 0));
        sensor.UpdateGyro(1, 2, 3, 2'000'000'000); // duplicate
        sensor.UpdateGyro(1, 2, 3, 1'000'000'000); // old
        sensor.UpdateGyro(std::numeric_limits<float>::quiet_NaN(), 0, 0, 3'000'000'000);
        sensor.UpdateGyro(0, std::numeric_limits<float>::infinity(), 0, 3'000'000'000);
        CHECK(sensor.Read().timestamp_ns == pose.timestamp_ns);
        CHECK(sensor.Read().orientation_w == pose.orientation_w);
        sensor.UpdateGyro(1, 2, 3, 3'000'000'000); // pause gap: reanchor, no jump
        CHECK(sensor.Read().orientation_w == pose.orientation_w);
        CHECK(sensor.Read().timestamp_ns == 3'000'000'000);
    }
    // The hardware eyes are in LOCAL. Translation and head rotation must be
    // removed exactly once before reporting guest head-local IPD offsets.
    for (unsigned axis = 0; axis < 3; ++axis) {
        Core::HostRuntime::GuestVrSensor::Pose head{};
        head.position = {2.f, 1.5f, -3.f};
        head.orientation = {0, 0, 0, std::sqrt(.5f)};
        head.orientation[axis] = std::sqrt(.5f);
        for (float ipd : {-0.032f, 0.034f}) {
            auto eye = head;
            // Independent 90-degree rotations of the X-axis offset.
            eye.position[axis == 0 ? 0 : axis == 1 ? 2 : 1] += axis == 1 ? -ipd : ipd;
            const auto local = Core::HostRuntime::VrGeometry::HeadLocalEye(head, eye);
            CHECK(Near(local[0], ipd) && Near(local[1], 0) && Near(local[2], 0));
        }
    }
    {
        Core::HostRuntime::GuestVrSensor::Pose head;
        head.position = {2, 1.6f, -3};
        const auto front = Core::HostRuntime::VrGeometry::CinemaPlacement(head);
        CHECK(Near(front.position[0], 2) && Near(front.position[1], 1.6f) && Near(front.position[2], -5.5f));
        head.orientation = {0, std::sqrt(.5f), 0, std::sqrt(.5f)};
        const auto left = Core::HostRuntime::VrGeometry::CinemaPlacement(head);
        CHECK(Near(left.position[0], -.5f) && Near(left.position[1], 1.6f) && Near(left.position[2], -3));
    }
    CHECK(Near(Core::HostRuntime::VrGeometry::CinemaHeight(16.f/9.f), 1.8f));
    CHECK(Near(Core::HostRuntime::VrGeometry::CinemaDistance, 2.5f));
    {
        using namespace Core::HostRuntime;
        GuestVrSensor::Pose calibration, render;
        calibration.orientation = {0, std::sqrt(.5f), 0, std::sqrt(.5f)};
        calibration.position = {10, 4, -3};
        calibration.orientation_valid = calibration.position_valid = true;
        auto eye = calibration;
        eye.position[2] += .032f; // runtime left, head rotated 90 degrees
        render.position = {1, 2, 3};
        render.orientation_valid = render.position_valid = true;
        const auto output = VrGeometry::RenderEye(render, calibration, eye);
        CHECK(Near(output.position[0], .968f) && Near(output.position[1], 2) && Near(output.position[2], 3));
        CHECK(Near(output.orientation[1], 0) && Near(output.orientation[3], 1));
        // Eye cant is an extrinsic; do not discard it when replacing head pose.
        const std::array<float, 4> cant{0, std::sin(.1f), 0, std::cos(.1f)};
        eye.orientation = VrGeometry::Multiply(calibration.orientation, cant);
        const auto canted = VrGeometry::RenderEye(render, calibration, eye);
        CHECK(Near(canted.orientation[1], cant[1]) && Near(canted.orientation[3], cant[3]));
        render.orientation_valid = false;
        CHECK(!VrGeometry::RenderEye(render, calibration, eye).orientation_valid);
        const std::array<float,4> fov{-std::atan(.8f), std::atan(1.1f), -std::atan(.9f), std::atan(1.2f)};
        const float atlas[4]{.2f,-.4f,.3f,.6f};
        const auto uv = VrGeometry::TangentUv(atlas, fov);
        CHECK(VrGeometry::ValidFov(fov));
        CHECK(Near(uv[0],.38f) && Near(uv[1],-.84f) && Near(uv[2],.14f) && Near(uv[3],.96f));
        CHECK(!VrGeometry::ValidFov({0,0,0,0}));
        CHECK(!VrGeometry::ValidFov({-2,1,-1,1}));
        CHECK(!VrGeometry::ValidFov({-1,1,1,-1}));
    }
    sensor.ResetOrientation();
    CHECK(sensor.Read().orientation_w == 1 && sensor.Read().orientation_z == 0);
    CHECK(sensor.Read().timestamp_ns == 0 && sensor.Read().angular_velocity_y == 0);
    for (uint64_t i = 1; i < 20'000; ++i) sensor.UpdateGyro(3, -2, 5, i * 5'000'000);
    const auto p = sensor.Read();
    CHECK(Near(p.orientation_x*p.orientation_x + p.orientation_y*p.orientation_y +
               p.orientation_z*p.orientation_z + p.orientation_w*p.orientation_w, 1));
    sensor.UpdateMoveInput(0xE000, -2, 0.25f, 0.5f, -0.75f, 2, 0.5f, 2'000'000'000);
    const auto move = sensor.ReadMoveInput();
    CHECK(move.timestamp_us == 2'000'000'000);
    CHECK(move.buttons == 0xE000 && move.left_x == -1 && move.right_y == -0.75f && move.left_trigger == 1);
    sensor.SetSbsEnabled(false);
    CHECK(!sensor.Read().enabled && sensor.Read().orientation_w == 1 && sensor.ReadMoveInput().buttons == 0);
    // Runtime generations reject late publishers; inactive/stale hands cannot stick.
    using Sensor = Core::HostRuntime::GuestVrSensor;
    const auto now = [] { return uint64_t(std::chrono::duration_cast<std::chrono::nanoseconds>(
        std::chrono::steady_clock::now().time_since_epoch()).count()); };
    auto gen = sensor.BeginOpenXr();
    Sensor::HardwareFrame frame{};
    frame.generation = gen; frame.received_ns = now(); frame.predicted_ns = frame.received_ns + 10'000'000;
    frame.running = frame.focused = true;
    frame.head.orientation_valid = frame.head.position_valid = true;
    frame.eyes[0] = frame.eyes[1] = frame.head;
    frame.eyes[0].position[0] = -.032f; frame.eyes[1].position[0] = .032f;
    frame.hands[0].active = frame.hands[1].active = true;
    frame.hands[0].trigger = .25f; frame.hands[1].trigger = .75f;
    frame.hands[1].grip = frame.head;
    frame.hands[1].grip.position[0] = .4f;
    CHECK(sensor.PublishOpenXr(frame));
    sensor.SetSbsEnabled(false);
    sensor.UpdateGyro(99, 99, 99, now());
    CHECK(sensor.Read().openxr && sensor.Read().hardware.hands[0].trigger == .25f);
    CHECK(sensor.Read().hardware.hands[1].grip.position[0] == .4f);
    sensor.RecordHmdQuery(sensor.Read().hardware);
    frame.eyes[0].position[0] = -.1f;
    CHECK(sensor.PublishOpenXr(frame));
    CHECK(sensor.RenderEyes()[0].position[0] == -.032f); // no display-time pose substitution
    CHECK(sensor.RequestHaptic(1, .4f));
    CHECK(sensor.TakeHaptics(gen)[1] == .4f && sensor.TakeHaptics(gen)[1] == -1);
    frame.focused = false;
    CHECK(sensor.PublishOpenXr(frame));
    CHECK(!sensor.Read().hardware.hands[1].active && !sensor.RequestHaptic(1, 1));
    frame.focused = true;
    frame.hands[0].trigger = std::numeric_limits<float>::quiet_NaN();
    frame.head.orientation[0] = std::numeric_limits<float>::infinity();
    CHECK(sensor.PublishOpenXr(frame));
    CHECK(sensor.Read().hardware.hands[0].trigger == 0);
    CHECK(!sensor.Read().hardware.head.orientation_valid && sensor.Read().orientation_w == 1);
    const auto new_gen = sensor.BeginOpenXr();
    CHECK(!sensor.PublishOpenXr(frame));
    sensor.EndOpenXr(gen); CHECK(sensor.Read().openxr);
    frame.generation = new_gen; frame.received_ns = now() - 600'000'000;
    CHECK(sensor.PublishOpenXr(frame));
    CHECK(!sensor.Read().hardware.hands[1].active && !sensor.RequestHaptic(1, 1));
    sensor.EndOpenXr(new_gen); CHECK(!sensor.Read().enabled);
    std::printf("guest_vr_sensor_tests: %u checks / %u failures\n", checks, failures);
    return failures ? 1 : 0;
}
