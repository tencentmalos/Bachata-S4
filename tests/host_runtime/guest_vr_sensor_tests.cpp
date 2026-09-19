// SPDX-License-Identifier: GPL-2.0-or-later

#include <cmath>
#include <cstdio>

#include "core/host_runtime/guest_vr_sensor.h"

int main() {
    auto& sensor = Core::HostRuntime::GuestVrSensor::Instance();
    sensor.SetSbsEnabled(false);
    sensor.UpdateGyro(0.0f, 0.0f, 1.0f, 1'000'000'000ull);
    if (sensor.Read().enabled)
        return 1;
    sensor.SetSbsEnabled(true);
    sensor.UpdateGyro(0.0f, 0.0f, 1.0f, 1'000'000'000ull);
    for (std::uint64_t timestamp = 1'100'000'000ull; timestamp <= 2'000'000'000ull;
         timestamp += 100'000'000ull) {
        sensor.UpdateGyro(0.0f, 0.0f, 1.0f, timestamp);
    }
    const auto pose = sensor.Read();
    const float expected = std::sin(0.5f);
    const float error = std::fabs(pose.orientation_z - expected);
    sensor.UpdateMoveInput(0xE000, -1.0f, 0.25f, 0.5f, -0.75f, 1.0f, 0.5f, 2'000'000'000ull);
    const auto move = sensor.ReadMoveInput();
    sensor.SetSbsEnabled(false);
    if (!pose.enabled || error > 0.03f || pose.orientation_w < 0.85f)
        return 2;
    if (move.buttons != 0xE000 || move.left_x != -1.0f || move.right_y != -0.75f ||
        move.left_trigger != 1.0f)
        return 3;
    std::printf("GUEST_VR_SENSOR z=%.6f w=%.6f error=%.6f\n", pose.orientation_z,
                pose.orientation_w, error);
    return 0;
}
