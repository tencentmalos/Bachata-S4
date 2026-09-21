// SPDX-FileCopyrightText: Copyright 2026 shadPS4 Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#pragma once

#include <cstdint>
#include <array>
#include <mutex>

namespace Core::HostRuntime {

// Android SBS integrates display-aligned gyroscope rates from an initially
// forward-facing orientation for the virtual PSVR. This provider owns no guest
// pointers, Vulkan objects, or OpenXR handles.  Desktop sessions leave it
// disabled and keep their existing no-device behavior.
class GuestVrSensor final {
public:
    struct Snapshot final {
        bool enabled{};
        float orientation_x{};
        float orientation_y{};
        float orientation_z{};
        float orientation_w{1.0f};
        float angular_velocity_x{};
        float angular_velocity_y{};
        float angular_velocity_z{};
        std::uint64_t timestamp_ns{};
        [[nodiscard]] std::array<float, 3> EyeOffset(float x) const {
            // Rotate the head-local IPD vector into the same tracking space as
            // device_pose; rotating only eye orientation leaves an invalid rig.
            return {x * (1 - 2 * (orientation_y * orientation_y + orientation_z * orientation_z)),
                    x * 2 * (orientation_x * orientation_y + orientation_w * orientation_z),
                    x * 2 * (orientation_x * orientation_z - orientation_w * orientation_y)};
        }
    };

    // The Android controller adapter publishes the merged PS4 pad state here.
    // Move does not expose a host pointer or an Android object; the HLE reads
    // this immutable snapshot when the guest asks for the next sample.  The
    // face-button mask deliberately remains in PS4's public bit positions so
    // the touch overlay and a physical controller use exactly the same path.
    struct MoveInputSnapshot final {
        std::uint64_t buttons{};
        float left_x{};
        float left_y{};
        float right_x{};
        float right_y{};
        float left_trigger{};
        float right_trigger{};
        // OrbisPadData and OrbisMoveData both use monotonic microseconds.
        // Gyroscope Snapshot above separately uses Android nanoseconds.
        std::uint64_t timestamp_us{};
    };

    static GuestVrSensor& Instance();

    void SetSbsEnabled(bool enabled);
    void UpdateGyro(float x, float y, float z, std::uint64_t timestamp_ns);
    void UpdateMoveInput(std::uint64_t buttons, float left_x, float left_y, float right_x,
                         float right_y, float left_trigger, float right_trigger,
                         std::uint64_t timestamp_us = 0);
    void ResetOrientation();
    [[nodiscard]] Snapshot Read() const;
    [[nodiscard]] MoveInputSnapshot ReadMoveInput() const;

private:
    mutable std::mutex mutex;
    Snapshot snapshot;
    MoveInputSnapshot move_input;
};

} // namespace Core::HostRuntime
