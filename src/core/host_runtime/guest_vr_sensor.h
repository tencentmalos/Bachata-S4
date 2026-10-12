// SPDX-FileCopyrightText: Copyright 2026 shadPS4 Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#pragma once

#include <array>
#include <atomic>
#include <cstdint>
#include <memory>
#include <mutex>

namespace Core::HostRuntime {

// Android SBS uses fused device orientation relative to a neutral pose, with
// gyroscope integration as a fallback. This provider owns no guest
// pointers, Vulkan objects, or OpenXR handles.  Desktop sessions leave it
// disabled and keep their existing no-device behavior.
class GuestVrSensor final {
public:
    struct Pose {
        std::array<float, 4> orientation{0, 0, 0, 1};
        std::array<float, 3> position{};
        std::array<float, 3> angular_velocity{};
        std::array<float, 3> linear_velocity{};
        bool orientation_valid{}, position_valid{};
        bool position_emulated{}; // A placement model, not an optical position measurement.
        // The runtime reported linear_velocity (XR_SPACE_VELOCITY_LINEAR_VALID_BIT). When it
        // does not, PublishOpenXr derives the head's from successive positions.
        bool linear_velocity_valid{};
    };
    struct Hand {
        Pose grip{}, aim{};
        bool active{};
        std::uint64_t buttons{}; // PS4 face/option bits, independent per hand.
        float stick_x{}, stick_y{}, trigger{}, squeeze{};
    };
    // Diagnostic guest tracking input. Camera-space metres, xyzw quaternion.
    // Its lifetime belongs to the existing Pad DebugBus session/owner lease.
    enum class DebugTarget : unsigned { Head, Ds4 };
    struct DebugPose {
        DebugTarget target{};
        Pose pose{};
        std::uint64_t action_id{}, deadline_ns{};
        std::atomic<std::uint64_t> polls{}, first_poll_ns{};
    };
    struct TrackerSample {
        bool valid{};
        DebugTarget target{};
        int handle{-1}, status{}, position_quality{}, orientation_quality{};
        bool connected{};
        Pose pose{};
        std::uint64_t sampled_ns{}, polls{};
        std::shared_ptr<DebugPose> debug;
    };
    void PublishDebugPose(std::shared_ptr<DebugPose> pose);
    void ClearDebugPose(DebugTarget target);
    std::shared_ptr<DebugPose> ReadDebugPose(DebugTarget target) const;
    // Called only after the checked guest output write succeeds.
    void ObserveTrackerRead(TrackerSample sample);
    std::array<TrackerSample, 2> ReadTrackerSamples() const;
    void ClearTrackerSamples();
    struct HardwareFrame {
        std::uint64_t generation{}, received_ns{}, predicted_ns{};
        bool running{}, focused{}, mounted{};
        Pose head{};
        std::array<Pose, 2> eyes{};
        // Left/right/down/up angles, radians, exactly as located by the runtime.
        std::array<std::array<float, 4>, 2> fov{};
        std::array<Hand, 2> hands{};
        // Tracked palms (hand tracking, hands not holding the headset's controllers), 0 left.
        std::array<Pose, 2> palms{};
        std::uint32_t eye_width{}, eye_height{};
    };
    struct Snapshot final {
        bool enabled{};
        bool openxr{};
        bool external_head{}; // Tracking only; the window still presents SBS, not OpenXR.
        std::uint64_t external_position_rebases{};
        [[nodiscard]] bool HardwareTracking() const { return openxr || external_head; }
        HardwareFrame hardware{};
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

    std::uint64_t BeginOpenXr();
    void EndOpenXr(std::uint64_t generation);
    bool PublishOpenXr(const HardwareFrame& frame);
    void RecordHmdQuery(const HardwareFrame& frame);
    std::array<Pose, 2> RenderEyes() const;
    bool RequestHaptic(unsigned hand, float amplitude);
    std::array<float, 2> TakeHaptics(std::uint64_t generation);

    void SetSbsEnabled(bool enabled);
    void UpdateGyro(float x, float y, float z, std::uint64_t timestamp_ns);
    void UpdateRotationVector(std::array<float, 4> orientation, unsigned display_rotation,
                              std::uint64_t timestamp_ns);
    void UpdateMoveInput(std::uint64_t buttons, float left_x, float left_y, float right_x,
                         float right_y, float left_trigger, float right_trigger,
                         std::uint64_t timestamp_us = 0);
    void ResetOrientation();
    void ResetDeviceOrientation();
    void RecenterExternalRig();
    // External tracking is already in right-handed Y-up/-Z-forward coordinates.
    // Query receipt uses CLOCK_MONOTONIC; it is not a hardware sample timestamp.
    void SelectExternalHead(bool selected);
    void UpdateExternalHead(Pose pose, std::uint64_t received_ns);
    // The runtime recentred its LOCAL space (the player reset the view). Titles are told with a
    // ResetVrPosition system event; RecenterCount lets the session that owns the event queue see
    // each one once.
    void NotifyRecenter();
    [[nodiscard]] std::uint64_t RecenterCount() const;
    [[nodiscard]] Snapshot Read() const;
    // Whether a headset is there for the title: the virtual SBS one, or a running OpenXR
    // session. sceHmdGetDeviceInformation reports READY and VideoOut the VR_VIEW capability
    // exactly then.
    [[nodiscard]] bool HeadsetReady() const;
    [[nodiscard]] MoveInputSnapshot ReadMoveInput() const;

private:
    mutable std::mutex mutex;
    Snapshot snapshot;
    struct ExternalHead {
        bool selected{}, reference_valid{}, previous_valid{};
        Pose pose{}, origin{}, previous_raw{};
        std::array<float, 4> neutral_rotation{0, 0, 0, 1};
        std::array<float, 3> correction{};
        std::uint64_t received_ns{}, position_rebases{};
    } external;
    void ResetDeviceOrientationLocked();
    MoveInputSnapshot move_input;
    std::uint64_t xr_generation{};
    std::array<Pose, 2> render_eyes{};
    std::array<float, 2> haptics{-1, -1};
    // Head velocity from position differences, for runtimes that report none.
    struct HeadMotion {
        std::array<float, 3> position{}, velocity{};
        std::uint64_t time_ns{};
        bool seen{};
    } head_motion;
    std::uint64_t recenters{};
    std::array<std::shared_ptr<DebugPose>, 2> debug_poses{};
    std::array<TrackerSample, 2> tracker_samples{};
    struct RotationReference {
        std::array<float, 4> orientation{};
        std::array<float, 3> right{}, up{}, back{};
        unsigned display_rotation{};
        bool valid{};
    } rotation_reference;
};

} // namespace Core::HostRuntime
