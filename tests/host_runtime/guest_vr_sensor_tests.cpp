// SPDX-License-Identifier: GPL-2.0-or-later
#include <cmath>
#include <cstdio>
#include <limits>
#include <numbers>
#include "core/host_runtime/guest_vr_sensor.h"
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
    std::printf("guest_vr_sensor_tests: %u checks / %u failures\n", checks, failures);
    return failures ? 1 : 0;
}
