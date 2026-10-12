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
    // Fused Android poses: turning on a desk is yaw for every screen rotation,
    // even after starting inclined and recalibrating flat. Missing callbacks
    // must not permanently lose angles as integration of sampled rates does.
    {
        using namespace Core::HostRuntime;
        const auto mul = VrGeometry::Multiply;
        const float h = std::sqrt(.5f);
        const std::array<float, 4> identity{0, 0, 0, 1};
        const std::array<float, 4> yaw90{0, 0, h, h}; // Android world +Z up
        uint64_t t = 10'000'000'000;
        for (unsigned rotation = 0; rotation < 4; ++rotation) {
            sensor.SetSbsEnabled(true);
            // 45-degree incline plus arbitrary global heading at startup.
            const auto inclined = mul(yaw90, {std::sin(.39269908f), 0, 0, std::cos(.39269908f)});
            sensor.UpdateRotationVector(inclined, rotation, ++t);
            CHECK(Near(sensor.Read().orientation_w, 1));
            sensor.UpdateRotationVector(identity, rotation, t += 20'000'000);
            CHECK(std::abs(sensor.Read().orientation_x) + std::abs(sensor.Read().orientation_z) >
                  .1f);
            sensor.ResetOrientation(); // same path as guest HMD recalibration
            sensor.UpdateRotationVector(identity, rotation, ++t);
            for (float sign : {1.f, -1.f}) {
                sensor.UpdateRotationVector({0, 0, sign * h, h}, rotation, t += 1'000'000'000);
                const auto p = sensor.Read();
                CHECK(Near(p.orientation_x, 0) && Near(p.orientation_z, 0));
                CHECK(Near(p.orientation_y, sign * h) && Near(p.orientation_w, h));
                CHECK(p.angular_velocity_y == 0); // gap invalidates velocity only
            }
            sensor.UpdateRotationVector(identity, rotation, t += 20'000'000);
            CHECK(Near(sensor.Read().orientation_w, 1));
            sensor.UpdateRotationVector({0, 0, 0, -1}, rotation, t += 20'000'000);
            CHECK(Near(sensor.Read().angular_velocity_y, 0));    // antipodal quaternion
            const auto stamp = sensor.Read().timestamp_ns;
            sensor.UpdateRotationVector(yaw90, rotation, stamp); // duplicate/old/bad samples
            sensor.UpdateRotationVector(yaw90, rotation, stamp - 1);
            sensor.UpdateRotationVector({0, 0, 0, 0}, rotation, stamp + 1);
            sensor.UpdateRotationVector({NAN, 0, 0, 1}, rotation, stamp + 1);
            sensor.UpdateRotationVector(identity, 4, stamp + 1);
            CHECK(sensor.Read().timestamp_ns == stamp);
            sensor.UpdateGyro(50, 50, 50, t += 20'000'000); // cannot mix publishers
            CHECK(sensor.Read().timestamp_ns == stamp);
            // A display change starts a fresh neutral orientation.
            sensor.UpdateRotationVector(yaw90, (rotation + 1) % 4, ++t);
            CHECK(Near(sensor.Read().orientation_w, 1));
        }
        // Upright devices and clamshells use the same world-up yaw convention.
        for (unsigned rotation = 0; rotation < 4; ++rotation) {
            const float angle = rotation * std::numbers::pi_v<float> / 2;
            const auto upright =
                mul(std::array<float, 4>{h, 0, 0, h},
                    std::array<float, 4>{0, 0, -std::sin(angle / 2), std::cos(angle / 2)});
            sensor.SetSbsEnabled(true);
            sensor.UpdateRotationVector(upright, rotation, ++t);
            sensor.UpdateRotationVector(mul(yaw90, upright), rotation, t += 20'000'000);
            const auto p = sensor.Read();
            CHECK(Near(p.orientation_x, 0) && Near(p.orientation_z, 0));
            CHECK(Near(p.orientation_y, h) && Near(p.orientation_w, h));
            CHECK(std::abs(p.angular_velocity_y - std::numbers::pi_v<float> / .04f) < .001f);
        }
        sensor.SetSbsEnabled(false);
        sensor.UpdateRotationVector(yaw90, 0, ++t);
        CHECK(!sensor.Read().enabled && sensor.Read().timestamp_ns == 0);
        sensor.SetSbsEnabled(true); // following integration tests use the fallback
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
        CHECK(Near(front.position[0], 2) && Near(front.position[1], 1.7f) && Near(front.position[2], -6.f));
        head.orientation = {0, std::sqrt(.5f), 0, std::sqrt(.5f)};
        const auto left = Core::HostRuntime::VrGeometry::CinemaPlacement(head);
        CHECK(Near(left.position[0], -1.f) && Near(left.position[1], 1.7f) && Near(left.position[2], -3));
    }
    CHECK(Near(Core::HostRuntime::VrGeometry::CinemaHeight(16.f/9.f), 2.475f));
    CHECK(Near(Core::HostRuntime::VrGeometry::CinemaDistance, 3.f));
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

    // Head linear velocity from positions when the runtime reports none: 1 m/s along X at
    // 90 Hz converges to 1 m/s; a reported velocity is kept; a recentre is not motion.
    {
        const auto vgen = sensor.BeginOpenXr();
        Core::HostRuntime::GuestVrSensor::HardwareFrame f{};
        f.generation = vgen;
        f.running = f.focused = true;
        f.head.orientation_valid = f.head.position_valid = true;
        const std::uint64_t start = now();
        const std::uint64_t step = 11'111'111;
        for (unsigned i = 0; i < 20; ++i) {
            f.received_ns = start + i;
            f.predicted_ns = start + i * step;
            f.head.position = {float(i) * step * 1e-9f, 1.6f, 0};
            CHECK(sensor.PublishOpenXr(f));
        }
        auto v = sensor.Read().hardware.head.linear_velocity;
        CHECK(std::abs(v[0] - 1.f) < 1e-3f && std::abs(v[1]) < 1e-4f && std::abs(v[2]) < 1e-4f);
        f.head.linear_velocity = {0, 0, -2};
        f.head.linear_velocity_valid = true;
        f.received_ns = start + 20;
        f.predicted_ns = start + 20 * step;
        f.head.position = {20 * step * 1e-9f, 1.6f, 0};
        CHECK(sensor.PublishOpenXr(f));
        v = sensor.Read().hardware.head.linear_velocity;
        CHECK(v[0] == 0 && v[2] == -2);
        // The new origin moves the head 3 m; no velocity comes of it.
        const auto before = sensor.RecenterCount();
        sensor.NotifyRecenter();
        CHECK(sensor.RecenterCount() == before + 1);
        f.head.linear_velocity_valid = false;
        f.head.linear_velocity = {};
        f.received_ns = start + 21;
        f.predicted_ns = start + 21 * step;
        f.head.position = {3, 1.6f, 0};
        CHECK(sensor.PublishOpenXr(f));
        f.received_ns = start + 22;
        f.predicted_ns = start + 22 * step;
        CHECK(sensor.PublishOpenXr(f));
        v = sensor.Read().hardware.head.linear_velocity;
        CHECK(std::abs(v[0]) < 1e-4f);
        // A gap of more than 100 ms resets it.
        f.received_ns = start + 23;
        f.predicted_ns = start + 22 * step + 150'000'000;
        f.head.position = {3.5f, 1.6f, 0};
        CHECK(sensor.PublishOpenXr(f));
        CHECK(sensor.Read().hardware.head.linear_velocity[0] == 0);
        sensor.EndOpenXr(vgen);
    }
    // XREAL head and the handheld IMU have separate clocks, neutral poses and ownership.
    {
        using namespace Core::HostRuntime;
        sensor.SetSbsEnabled(true);
        sensor.SelectExternalHead(true);
        CHECK(sensor.HeadsetReady());
        CHECK(sensor.Read().hardware.head.position_emulated);
        CHECK(sensor.Read().hardware.head.orientation==std::array<float,4>{0,0,0,1});
        auto t=std::chrono::duration_cast<std::chrono::nanoseconds>(
            std::chrono::steady_clock::now().time_since_epoch()).count()-300'000'000;
        const float h=std::sqrt(.5f);
        GuestVrSensor::Pose head;
        head.position={1,2,3}; head.orientation={0,std::sin(.2f),0,std::cos(.2f)};
        head.orientation_valid=head.position_valid=true;
        sensor.UpdateRotationVector({0,0,0,1},0,t);
        sensor.UpdateExternalHead(head,t);
        auto a=sensor.Read();
        CHECK(a.external_head && !a.openxr && a.HardwareTracking());
        CHECK(!a.hardware.head.position_emulated);
        CHECK(a.hardware.head.position==std::array<float,3>{0,0,0});
        CHECK(Near(a.hardware.head.orientation[3],1));
        CHECK(a.hardware.hands[1].active && !a.hardware.hands[0].active);
        CHECK(a.hardware.hands[1].grip.position_emulated);
        sensor.UpdateRotationVector({0,0,h,h},0,t+=10'000'000);
        auto b=sensor.Read();
        CHECK(b.hardware.head.orientation==a.hardware.head.orientation);
        CHECK(Near(b.hardware.hands[1].grip.orientation[1],h));
        head.orientation=VrGeometry::Multiply(head.orientation,{std::sin(.1f),0,0,std::cos(.1f)});
        head.position[0]+=.02f;
        sensor.UpdateExternalHead(head,t+=10'000'000);
        auto c=sensor.Read();
        CHECK(c.hardware.hands[1].grip.orientation==b.hardware.hands[1].grip.orientation);
        CHECK(c.hardware.hands[1].grip.position==b.hardware.hands[1].grip.position);
        CHECK(std::abs(c.hardware.head.orientation[0])>.09f);
        CHECK(c.hardware.head.position[0]>.01f);
        const auto ipd=VrGeometry::HeadLocalEye(c.hardware.head,c.hardware.eyes[1]);
        CHECK(Near(ipd[0],.0315f) && Near(ipd[1],0) && Near(ipd[2],0));
        sensor.ResetOrientation(); // Guest HMD calibration must not rotate the handheld.
        sensor.UpdateExternalHead(head,t+=10'000'000);
        auto d=sensor.Read();
        CHECK(d.hardware.hands[1].grip.orientation==c.hardware.hands[1].grip.orientation);
        CHECK(d.hardware.head.position==std::array<float,3>{0,0,0});
        sensor.ResetDeviceOrientation(); // Android focus/sensor restart must not recenter the head.
        sensor.UpdateRotationVector({0,0,h,h},0,t+=10'000'000);
        CHECK(sensor.Read().hardware.head.orientation==d.hardware.head.orientation);
        CHECK(Near(sensor.Read().hardware.hands[1].grip.orientation[3],1));
        const auto before=sensor.Read().hardware.head.position;
        head.position[0]+=.86f;
        sensor.UpdateExternalHead(head,t+=50'000'000);
        CHECK(sensor.Read().external_position_rebases==1);
        for(unsigned i=0;i<3;++i) CHECK(Near(sensor.Read().hardware.head.position[i],before[i]));
        sensor.UpdateExternalHead({},t+=10'000'000);
        CHECK(sensor.Read().hardware.head.orientation_valid);
        CHECK(sensor.Read().hardware.head.position_emulated);
        CHECK(sensor.Read().hardware.head.orientation==std::array<float,4>{0,0,0,1});
        CHECK(sensor.Read().hardware.head.position==std::array<float,3>{0,0,0});
        CHECK(sensor.Read().hardware.hands[1].active);
        CHECK(sensor.HeadsetReady());
        sensor.SelectExternalHead(false); sensor.SelectExternalHead(true);
        sensor.UpdateExternalHead(head,t-1'000'000'000);
        CHECK(sensor.Read().hardware.head.position_emulated && sensor.HeadsetReady());
        sensor.UpdateExternalHead(head, now());
        CHECK(!sensor.Read().hardware.head.position_emulated);
        const auto generation=sensor.BeginOpenXr();
        sensor.UpdateExternalHead(head,t+=10'000'000);
        CHECK(sensor.Read().openxr && !sensor.Read().external_head);
        sensor.EndOpenXr(generation);
        // A tilted starting headset is level in-game; world-up motion remains yaw.
        sensor.SelectExternalHead(false);
        sensor.SelectExternalHead(true);
        sensor.SetSbsEnabled(true);
        sensor.RecenterExternalRig();
        t=now()-100'000'000;
        const auto tilted=VrGeometry::Multiply(
            std::array<float,4>{0,std::sin(.3f),0,std::cos(.3f)},
            std::array<float,4>{std::sin(.25f),0,0,std::cos(.25f)});
        head.orientation=tilted;
        sensor.UpdateExternalHead(head,t);
        CHECK(Near(sensor.Read().hardware.head.orientation[3],1));
        for(unsigned i=0;i<3;++i) CHECK(Near(sensor.Read().hardware.head.orientation[i],0));
        head.orientation=VrGeometry::Multiply({0,std::sin(.1f),0,std::cos(.1f)},tilted);
        sensor.UpdateExternalHead(head,t+=10'000'000);
        const auto turned=sensor.Read().hardware.head.orientation;
        CHECK(Near(turned[0],0) && Near(turned[2],0));
        CHECK(Near(turned[1],std::sin(.1f)) && Near(turned[3],std::cos(.1f)));
        sensor.UpdateRotationVector({0,0,0,1},0,t);
        sensor.UpdateRotationVector({0,0,h,h},0,t+=10'000'000);
        sensor.RecenterExternalRig();
        sensor.UpdateRotationVector({0,0,h,h},0,t+=10'000'000);
        sensor.UpdateExternalHead(head,t);
        CHECK(Near(sensor.Read().hardware.hands[1].grip.orientation[3],1));
        CHECK(Near(sensor.Read().hardware.head.orientation[3],1));
        sensor.SelectExternalHead(false); sensor.SetSbsEnabled(false);
    }
    std::printf("guest_vr_sensor_tests: %u checks / %u failures\n", checks, failures);
    return failures ? 1 : 0;
}
