#include "core/libraries/move/move.h"
// SPDX-FileCopyrightText: Copyright 2025 shadPS4 Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#include "common/logging/log.h"
#include "common/path_util.h"
#include "core/libraries/error_codes.h"
#include "core/host_runtime/ds4_placement.h"
#include "core/host_runtime/guest_vr_sensor.h"
#include "core/host_runtime/vr_geometry.h"
#include "core/host_runtime/vr_time.h"
#include "core/libraries/kernel/time.h"
#include "core/libraries/libs.h"
#include "core/libraries/vr_tracker/vr_tracker.h"
#include "core/libraries/vr_tracker/vr_tracker_devices.h"
#include "core/libraries/vr_tracker/vr_tracker_error.h"
#include "core/libraries/vr_tracker/vr_tracker_play_area.h"
#include "core/memory.h"
#include "video_core/amdgpu/liverpool.h"

#include <algorithm>
#include <cstring>
#include <fstream>
#include <nlohmann/json.hpp>
#include <array>

namespace Libraries::VrTracker {

using Core::HostRuntime::GuestVrSensor;

static bool VirtualSbsEnabled() {
    return GuestVrSensor::Instance().Read().enabled;
}

static bool g_library_initialized = false;

// Internal memory
static void* g_garlic_memory_pointer = nullptr;
static u32 g_garlic_size = 0;
static void* g_onion_memory_pointer = nullptr;
static u32 g_onion_size = 0;
static void* g_work_memory_pointer = nullptr;
static u32 g_work_size = 0;

// Registered handles
static PadRegistry g_pads;
static Recalibrations g_recalibrations;
// The player's DualShock 4 placed by the headset's controllers (AstroQuest plan D2, tier A).
static std::mutex g_ds4_mutex;
static Core::HostRuntime::Ds4Placement g_ds4;
static std::uint64_t g_ds4_recenters{};
static bool g_ds4_own_loaded{};

static std::filesystem::path Ds4OwnPlacePath() {
    return Common::FS::GetUserPath(Common::FS::PathType::UserDir) / "vr_controller.json";
}
// Reads the player's own place once per process. g_ds4_mutex held.
static void LoadDs4OwnPlaceLocked() {
    if (g_ds4_own_loaded)
        return;
    g_ds4_own_loaded = true;
    std::ifstream file{Ds4OwnPlacePath()};
    if (!file)
        return;
    const auto json = nlohmann::json::parse(file, nullptr, false);
    if (json.is_discarded() || !json.contains("own_place") || !json["own_place"].is_array() ||
        json["own_place"].size() != 3)
        return;
    std::array<float, 3> place{};
    for (unsigned i = 0; i < 3; ++i) {
        if (!json["own_place"][i].is_number())
            return;
        place[i] = json["own_place"][i].get<float>();
    }
    g_ds4.SetOwnOffset(place, json.value("used", true));
}
static void SaveDs4OwnPlaceLocked() {
    const auto place = g_ds4.OwnOffset();
    if (!place)
        return;
    std::ofstream file{Ds4OwnPlacePath()};
    file << nlohmann::json{{"own_place", {(*place)[0], (*place)[1], (*place)[2]}},
                           {"used", g_ds4.OwnPlaceUsed()}}
                .dump()
         << '\n';
}
static std::array<s32, 2> g_move_handles{-1, -1};
static s32 g_gun_handle = -1;
static s32 g_hmd_handle = -1;
s32 PS4_SYSV_ABI sceVrTrackerQueryMemory(const OrbisVrTrackerQueryMemoryParam* param,
                                         OrbisVrTrackerQueryMemoryResult* result) {
    LOG_DEBUG(Lib_VrTracker, "called");
    if (param == nullptr || result == nullptr ||
        param->size != sizeof(OrbisVrTrackerQueryMemoryParam) ||
        (param->profile != OrbisVrTrackerProfile::ORBIS_VR_TRACKER_PROFILE_000 &&
         param->profile != OrbisVrTrackerProfile::ORBIS_VR_TRACKER_PROFILE_100) ||
        param->calibration_settings.pad_position >
            OrbisVrTrackerCalibrationMode::ORBIS_VR_TRACKER_CALIBRATION_AUTO ||
        // Hmd doesn't support auto calibration
        param->calibration_settings.hmd_position >
            OrbisVrTrackerCalibrationMode::ORBIS_VR_TRACKER_CALIBRATION_MANUAL ||
        param->calibration_settings.move_position >
            OrbisVrTrackerCalibrationMode::ORBIS_VR_TRACKER_CALIBRATION_AUTO ||
        param->calibration_settings.gun_position >
            OrbisVrTrackerCalibrationMode::ORBIS_VR_TRACKER_CALIBRATION_AUTO) {
        return ORBIS_VR_TRACKER_ERROR_ARGUMENT_INVALID;
    }

    // Setting move_position to ORBIS_VR_TRACKER_CALIBRATION_AUTO doubles required onion memory.
    u32 required_onion_size = ORBIS_VR_TRACKER_BASE_ONION_SIZE;
    if (param->calibration_settings.move_position ==
        OrbisVrTrackerCalibrationMode::ORBIS_VR_TRACKER_CALIBRATION_AUTO) {
        required_onion_size *= 2;
    }

    result->direct_memory_onion_size = required_onion_size;
    result->direct_memory_onion_alignment = ORBIS_VR_TRACKER_MEMORY_ALIGNMENT;
    result->direct_memory_garlic_size = ORBIS_VR_TRACKER_GARLIC_SIZE;
    result->direct_memory_garlic_alignment = ORBIS_VR_TRACKER_MEMORY_ALIGNMENT;
    result->work_memory_size = ORBIS_VR_TRACKER_WORK_SIZE;
    result->work_memory_alignment = ORBIS_VR_TRACKER_MEMORY_ALIGNMENT;

    return ORBIS_OK;
}

s32 PS4_SYSV_ABI sceVrTrackerInit(const OrbisVrTrackerInitParam* param) {
    if (g_library_initialized) {
        return ORBIS_VR_TRACKER_ERROR_ALREADY_INITIALIZED;
    }
    if (param == nullptr) {
        return ORBIS_VR_TRACKER_ERROR_ARGUMENT_INVALID;
    }

    // Calculate correct onion size for parameter checks
    u32 required_onion_size = ORBIS_VR_TRACKER_BASE_ONION_SIZE;
    if (param->calibration_settings.move_position == ORBIS_VR_TRACKER_CALIBRATION_AUTO) {
        required_onion_size *= 2;
    }

    // Parameter checks are fairly thorough here.
    if (param->size != sizeof(OrbisVrTrackerInitParam) ||
        // Check garlic memory parameters
        param->direct_memory_garlic == nullptr ||
        param->direct_memory_garlic_alignment != ORBIS_VR_TRACKER_MEMORY_ALIGNMENT ||
        param->direct_memory_garlic_size != ORBIS_VR_TRACKER_GARLIC_SIZE ||
        // Check onion memory parameters
        param->direct_memory_onion == nullptr ||
        param->direct_memory_onion_alignment != ORBIS_VR_TRACKER_MEMORY_ALIGNMENT ||
        param->direct_memory_onion_size != required_onion_size ||
        // Check work memory parameters
        param->work_memory == nullptr ||
        param->work_memory_alignment != ORBIS_VR_TRACKER_MEMORY_ALIGNMENT ||
        param->work_memory_size != ORBIS_VR_TRACKER_WORK_SIZE ||
        // Check compute queue parameters
        param->gpu_pipe_id >= AmdGpu::Liverpool::NumComputePipes ||
        param->gpu_queue_id >= AmdGpu::Liverpool::NumQueuesPerPipe ||
        // Check calibration settings
        param->calibration_settings.pad_position >
            OrbisVrTrackerCalibrationMode::ORBIS_VR_TRACKER_CALIBRATION_AUTO ||
        param->calibration_settings.hmd_position >
            OrbisVrTrackerCalibrationMode::ORBIS_VR_TRACKER_CALIBRATION_MANUAL ||
        param->calibration_settings.move_position >
            OrbisVrTrackerCalibrationMode::ORBIS_VR_TRACKER_CALIBRATION_AUTO ||
        param->calibration_settings.gun_position >
            OrbisVrTrackerCalibrationMode::ORBIS_VR_TRACKER_CALIBRATION_AUTO) {
        return ORBIS_VR_TRACKER_ERROR_ARGUMENT_INVALID;
    }

    if (VirtualSbsEnabled()) {
        // The Android adapter already checked the guest ranges before this
        // desktop-facing routine is called. Keep the numeric ownership for
        // diagnostics but do not query the desktop Core::Memory singleton:
        // Android has its own GuestAddressSpace.
        g_garlic_memory_pointer = param->direct_memory_garlic;
        g_garlic_size = param->direct_memory_garlic_size;
        g_onion_memory_pointer = param->direct_memory_onion;
        g_onion_size = param->direct_memory_onion_size;
        g_work_memory_pointer = param->work_memory;
        g_work_size = param->work_memory_size;
        g_library_initialized = true;
        return ORBIS_OK;
    }

    // Real hardware will segfault if any of the supplied mappings aren't long enough,
    // Validate each of them to ensure nothing weird can occur when this library is implemented.
    auto* memory = Core::Memory::Instance();
    Libraries::Kernel::OrbisVirtualQueryInfo info;
    // The memory type for the whole range should be the same here,
    // so the memory should be contained in one VMA.
    VAddr addr_to_check = std::bit_cast<VAddr>(param->direct_memory_garlic);
    s32 result = memory->VirtualQuery(addr_to_check, 0, &info);
    ASSERT_MSG(result == 0 && info.end - addr_to_check >= param->direct_memory_garlic_size,
               "Insufficient garlic memory provided");

    g_garlic_memory_pointer = param->direct_memory_garlic;
    g_garlic_size = param->direct_memory_garlic_size;

    addr_to_check = std::bit_cast<VAddr>(param->direct_memory_onion);
    result = memory->VirtualQuery(addr_to_check, 0, &info);
    ASSERT_MSG(result == 0 && info.end - addr_to_check >= param->direct_memory_onion_size,
               "Insufficient onion memory provided");

    g_onion_memory_pointer = param->direct_memory_onion;
    g_onion_size = param->direct_memory_onion_size;

    addr_to_check = std::bit_cast<VAddr>(param->work_memory);
    result = memory->VirtualQuery(addr_to_check, 0, &info);
    ASSERT_MSG(result == 0 && info.end - addr_to_check >= param->work_memory_size,
               "Insufficient work memory provided");

    g_work_memory_pointer = param->work_memory;
    g_work_size = param->work_memory_size;

    // All initialization checks passed.
    LOG_WARNING(Lib_VrTracker, "PSVR headsets are not supported yet");
    g_library_initialized = true;

    return ORBIS_OK;
}

s32 PS4_SYSV_ABI sceVrTrackerRegisterDevice(const OrbisVrTrackerDeviceType device_type,
                                            const s32 handle) {
    LOG_TRACE(Lib_VrTracker, "redirected to sceVrTrackerRegisterDeviceInternal");
    return sceVrTrackerRegisterDeviceInternal(device_type, handle, -1, 0);
}

s32 PS4_SYSV_ABI sceVrTrackerRegisterDevice2(const OrbisVrTrackerDeviceType device_type,
                                             const s32 handle) {
    LOG_TRACE(Lib_VrTracker, "redirected to sceVrTrackerRegisterDeviceInternal");
    return sceVrTrackerRegisterDeviceInternal(device_type, handle, -1, 1);
}

s32 PS4_SYSV_ABI sceVrTrackerRegisterDeviceInternal(const OrbisVrTrackerDeviceType device_type,
                                                    const s32 handle, s32 unk0, s32 unk1) {
    LOG_WARNING(Lib_VrTracker, "(STUBBED) called, device_type = {}, handle = {}",
                static_cast<u32>(device_type), handle);
    if (!g_library_initialized) {
        return ORBIS_VR_TRACKER_ERROR_NOT_INIT;
    }
    if (device_type > OrbisVrTrackerDeviceType::ORBIS_VR_TRACKER_DEVICE_GUN || unk0 > 4) {
        return ORBIS_VR_TRACKER_ERROR_ARGUMENT_INVALID;
    }

    // Ignore handle handle validation for now, since most of that logic isn't really handled.
    switch (device_type) {
    case OrbisVrTrackerDeviceType::ORBIS_VR_TRACKER_DEVICE_HMD: {
        if (g_hmd_handle != -1) {
            return ORBIS_VR_TRACKER_ERROR_DEVICE_ALREADY_REGISTERED;
        }
        g_hmd_handle = handle;
        break;
    }
    case OrbisVrTrackerDeviceType::ORBIS_VR_TRACKER_DEVICE_DUALSHOCK4: {
        // Every logged-in player's controller may be registered; only the first one is the
        // player's. unk0 is the light bar colour the title asks for, negative for any free one.
        OrbisVrTrackerLedColor color{};
        if (const s32 status = g_pads.Register(handle, unk0, &color); status != ORBIS_OK)
            return status;
        LOG_INFO(Lib_VrTracker, "DualShock 4 {} registered, light bar colour {}{}", handle,
                 static_cast<s32>(color), g_pads.IsPlayers(handle) ? " (player's)" : "");
        break;
    }
    case OrbisVrTrackerDeviceType::ORBIS_VR_TRACKER_DEVICE_MOVE: {
        for (const auto value : g_move_handles) {
            if (value == handle)
                return ORBIS_VR_TRACKER_ERROR_DEVICE_ALREADY_REGISTERED;
        }
        auto slot = std::find(g_move_handles.begin(), g_move_handles.end(), -1);
        if (slot == g_move_handles.end())
            return ORBIS_VR_TRACKER_ERROR_DEVICE_ALREADY_REGISTERED;
        *slot = handle;
        break;
    }
    case OrbisVrTrackerDeviceType::ORBIS_VR_TRACKER_DEVICE_GUN: {
        if (g_gun_handle != -1) {
            return ORBIS_VR_TRACKER_ERROR_DEVICE_ALREADY_REGISTERED;
        }
        g_gun_handle = handle;
        break;
    }
    default: {
        // Shouldn't be possible to hit this.
        UNREACHABLE();
    }
    }

    return ORBIS_OK;
}

s32 PS4_SYSV_ABI sceVrTrackerCpuProcess(const OrbisVrTrackerCpuProcessParam* param) {
    if (!g_library_initialized) return ORBIS_VR_TRACKER_ERROR_NOT_INIT;
    if (!param || param->size != sizeof(*param) ||
        (param->operation_mode != ORBIS_VR_TRACKER_CPU_PROCESS_OPERATION_MODE_WHOLE &&
         param->operation_mode != ORBIS_VR_TRACKER_CPU_PROCESS_OPERATION_MODE_HANDLE) ||
        std::ranges::any_of(param->reserved, [](u32 v) { return v != 0; }))
        return ORBIS_VR_TRACKER_ERROR_ARGUMENT_INVALID;
    if (!VirtualSbsEnabled()) return ORBIS_VR_TRACKER_ERROR_NOT_EXECUTE_GPU_SUBMIT;
    if (param->operation_mode == ORBIS_VR_TRACKER_CPU_PROCESS_OPERATION_MODE_HANDLE &&
        (param->handle < 0 || (param->handle != g_hmd_handle &&
            std::ranges::find(g_move_handles, param->handle) == g_move_handles.end())))
        return ORBIS_VR_TRACKER_ERROR_DEVICE_NOT_REGISTERED;
    // The SBS provider integrates gyro samples on arrival. GetResult reads that
    // synchronized pose directly; no optical-camera GPU/CPU job remains to run.
    return ORBIS_OK;
}

s32 PS4_SYSV_ABI sceVrTrackerGetPlayAreaWarningInfo(OrbisVrTrackerPlayAreaWarningInfo* info) {
    if (VirtualSbsEnabled()) {
        if (!g_library_initialized)
            return ORBIS_VR_TRACKER_ERROR_NOT_INIT;
        if (!PlayAreaWarningInfoShapeValid(info))
            return ORBIS_VR_TRACKER_ERROR_ARGUMENT_INVALID;
        info->is_out_of_play_area = 0;
        info->is_distance_data_valid = 1;
        info->distance_from_vertical_boundary = 10.0f;
        info->distance_from_horizontal_boundary = 10.0f;
        return ORBIS_OK;
    }
    return PlayAreaWarningInfoNoProvider(g_library_initialized, info);
}

// Metres from the PS camera to the seated player (see Core::HostRuntime::TrackerSeatOffset).
static constexpr float SeatDistance = 1.5f;

s32 PS4_SYSV_ABI sceVrTrackerGetResult(const OrbisVrTrackerGetResultParam* param,
                                       OrbisVrTrackerResultData* result) {
    if (!g_library_initialized)
        return ORBIS_VR_TRACKER_ERROR_NOT_INIT;
    if (!param || !result || param->size != sizeof(OrbisVrTrackerGetResultParam))
        return ORBIS_VR_TRACKER_ERROR_ARGUMENT_INVALID;
    if (VirtualSbsEnabled()) {
        const auto sensor = GuestVrSensor::Instance().Read();
        const auto move_it = std::find(g_move_handles.begin(), g_move_handles.end(), param->handle);
        const bool is_hmd = g_hmd_handle >= 0 && param->handle == g_hmd_handle;
        const auto pad = param->handle >= 0 ? g_pads.Find(param->handle) : std::nullopt;
        if (!is_hmd && move_it == g_move_handles.end() && !pad)
            return ORBIS_VR_TRACKER_ERROR_DEVICE_NOT_REGISTERED;
        std::memset(result, 0, sizeof(*result));
        const u64 now = Libraries::Kernel::sceKernelGetProcessTime();
        result->handle = param->handle;
        result->user_frame_number = param->user_frame_number;
        result->connected = 1;
        result->timestamp = now;
        result->device_timestamp = Core::HostRuntime::VrTime::SteadyNsToProcessUs(
            sensor.timestamp_ns, Core::HostRuntime::VrTime::SteadyNowNs(), now);
        // The virtual camera shares the fixed forward-facing reference frame.
        // A zero-filled quaternion is not an identity rotation.
        result->camera_orientation_w = 1.0f;
        const auto device_type = is_hmd ? ORBIS_VR_TRACKER_DEVICE_HMD
                                 : pad  ? ORBIS_VR_TRACKER_DEVICE_DUALSHOCK4
                                        : ORBIS_VR_TRACKER_DEVICE_MOVE;
        // A recalibration is reported whether or not the device can be tracked afterwards: the
        // title waits for CALIBRATING to come and go.
        const bool recalibrating = g_recalibrations.Report(device_type, now);
        const float seat_z = Core::HostRuntime::TrackerSeatOffset() ? SeatDistance : 0.0f;
        if (pad && sensor.openxr &&
            Core::HostRuntime::GetDs4PoseSource() != Core::HostRuntime::Ds4PoseSource::Off &&
            g_pads.IsPlayers(param->handle)) {
            const auto& hardware = sensor.hardware;
            Core::HostRuntime::Ds4Placement::Result placed{};
            {
                std::scoped_lock lock{g_ds4_mutex};
                LoadDs4OwnPlaceLocked();
                // A view reset turns the tracking space: what the controller points at now is
                // ahead again.
                const auto recenters = GuestVrSensor::Instance().RecenterCount();
                if (recenters != g_ds4_recenters) {
                    g_ds4_recenters = recenters;
                    g_ds4.Recalibrate(hardware.head);
                }
                placed = g_ds4.Update(Core::HostRuntime::GetDs4PoseSource(), hardware.head,
                                      {hardware.hands[0].grip, hardware.hands[1].grip},
                                      {hardware.hands[0].active, hardware.hands[1].active},
                                      Core::HostRuntime::VrTime::SteadyNowNs(), hardware.palms);
            }
            if (placed.placed) {
                const auto& pose = placed.pose;
                result->led_color = pad->color;
                result->connected = hardware.running;
                // Out of sight it is still where the player keeps it, as a title expects of a
                // controller the camera briefly lost.
                const auto quality = placed.seen ? ORBIS_VR_TRACKER_QUALITY_FULL
                                                 : ORBIS_VR_TRACKER_QUALITY_PARTIAL;
                result->position_quality = quality;
                result->orientation_quality = quality;
                result->status = recalibrating ? ORBIS_VR_TRACKER_STATUS_CALIBRATING
                                               : ORBIS_VR_TRACKER_STATUS_TRACKING;
                auto& dst = result->pad_info.device_pose;
                dst.position_x = pose.position[0];
                dst.position_y = pose.position[1];
                dst.position_z = pose.position[2] + seat_z;
                dst.orientation_x = pose.orientation[0];
                dst.orientation_y = pose.orientation[1];
                dst.orientation_z = pose.orientation[2];
                dst.orientation_w = pose.orientation[3];
                result->angular_velocity_x = pose.angular_velocity[0];
                result->angular_velocity_y = pose.angular_velocity[1];
                result->angular_velocity_z = pose.angular_velocity[2];
                result->velocity_x = pose.linear_velocity[0];
                result->velocity_y = pose.linear_velocity[1];
                result->velocity_z = pose.linear_velocity[2];
                return ORBIS_OK;
            }
        }
        if (pad) {
            // Nothing places this controller (no pose source, or not the player's): it is
            // connected and its light bar has a colour, but it is not tracked.
            result->led_color = pad->color;
            result->status = recalibrating ? ORBIS_VR_TRACKER_STATUS_CALIBRATING
                                           : ORBIS_VR_TRACKER_STATUS_NOT_TRACKING;
            result->position_quality = ORBIS_VR_TRACKER_QUALITY_NONE;
            result->orientation_quality = ORBIS_VR_TRACKER_QUALITY_NONE;
            return ORBIS_OK;
        }
        result->status = ORBIS_VR_TRACKER_STATUS_TRACKING;
        result->position_quality = ORBIS_VR_TRACKER_QUALITY_FULL;
        result->orientation_quality = ORBIS_VR_TRACKER_QUALITY_FULL;
        // Only a device that is tracked can be said to be recalibrating.
        const auto calibrating = [&] {
            if (recalibrating && result->status == ORBIS_VR_TRACKER_STATUS_TRACKING)
                result->status = ORBIS_VR_TRACKER_STATUS_CALIBRATING;
        };
        if (sensor.openxr) {
            const auto& hardware = sensor.hardware;
            const auto copy = [seat_z](auto& dst, const GuestVrSensor::Pose& src) {
                dst.position_x = src.position[0]; dst.position_y = src.position[1];
                dst.position_z = src.position[2] + seat_z;
                dst.orientation_x = src.orientation[0]; dst.orientation_y = src.orientation[1];
                dst.orientation_z = src.orientation[2]; dst.orientation_w = src.orientation[3];
            };
            const int hand_index = is_hmd ? 0 : Libraries::Move::HandIndexForHandle(param->handle);
            if (hand_index < 0) return ORBIS_VR_TRACKER_ERROR_DEVICE_NOT_REGISTERED;
            const auto pose = is_hmd ? hardware.head :
                Core::HostRuntime::VrGeometry::MoveSpherePose(hardware.hands[hand_index].grip);
            result->connected = hardware.running && (is_hmd || hardware.hands[hand_index].active);
            result->orientation_quality = pose.orientation_valid ? ORBIS_VR_TRACKER_QUALITY_FULL : ORBIS_VR_TRACKER_QUALITY_NONE;
            result->position_quality = pose.position_valid ? ORBIS_VR_TRACKER_QUALITY_FULL : ORBIS_VR_TRACKER_QUALITY_NONE;
            result->status = result->connected && pose.orientation_valid ? ORBIS_VR_TRACKER_STATUS_TRACKING : ORBIS_VR_TRACKER_STATUS_NOT_TRACKING;
            result->angular_velocity_x = pose.angular_velocity[0];
            result->angular_velocity_y = pose.angular_velocity[1];
            result->angular_velocity_z = pose.angular_velocity[2];
            result->velocity_x = pose.linear_velocity[0];
            result->velocity_y = pose.linear_velocity[1];
            result->velocity_z = pose.linear_velocity[2];
            if (is_hmd) {
                GuestVrSensor::Instance().RecordHmdQuery(hardware);
                copy(result->hmd_info.device_pose, pose);
                copy(result->hmd_info.head_pose, pose);
                copy(result->hmd_info.left_eye_pose, hardware.eyes[0]);
                copy(result->hmd_info.right_eye_pose, hardware.eyes[1]);
                result->hmd_info.sensor_read_system_timestamp = result->timestamp;
            } else {
                copy(result->move_info.device_pose, pose);
            }
            calibrating();
            return ORBIS_OK;
        }
        if (is_hmd) {
            result->angular_velocity_x = sensor.angular_velocity_x;
            result->angular_velocity_y = sensor.angular_velocity_y;
            result->angular_velocity_z = sensor.angular_velocity_z;
            result->hmd_info.device_pose.orientation_x = sensor.orientation_x;
            result->hmd_info.device_pose.orientation_y = sensor.orientation_y;
            result->hmd_info.device_pose.orientation_z = sensor.orientation_z;
            result->hmd_info.device_pose.orientation_w = sensor.orientation_w;
            result->hmd_info.head_pose = result->hmd_info.device_pose;
            result->hmd_info.left_eye_pose = result->hmd_info.device_pose;
            result->hmd_info.right_eye_pose = result->hmd_info.device_pose;
            const auto left_eye = sensor.EyeOffset(-0.0315f);
            const auto right_eye = sensor.EyeOffset(0.0315f);
            result->hmd_info.left_eye_pose.position_x = left_eye[0];
            result->hmd_info.left_eye_pose.position_y = left_eye[1];
            result->hmd_info.left_eye_pose.position_z = left_eye[2];
            result->hmd_info.right_eye_pose.position_x = right_eye[0];
            result->hmd_info.right_eye_pose.position_y = right_eye[1];
            result->hmd_info.right_eye_pose.position_z = right_eye[2];
            result->hmd_info.rear_tracking_status = ORBIS_VR_TRACKER_REAR_TRACKING_READY;
            result->hmd_info.sensor_read_system_timestamp = result->timestamp;
        } else {
            const bool left = move_it == g_move_handles.begin();
            auto& pose = result->move_info.device_pose;
            // The SBS test controller has a deterministic, room-scale pose.
            // It is independent of the headset's gyroscope orientation.
            pose.position_x = left ? -0.25f : 0.25f;
            pose.position_y = -0.15f;
            pose.position_z = -0.45f;
            pose.orientation_w = 1.0f;
        }
        calibrating();
        return ORBIS_OK;
    }
    LOG_ERROR(Lib_VrTracker, "(INCOMPLETE) called without a camera/HMD provider");
    // No camera/HMD provider or validated diagnostic provider is attached.
    // Do not write a fabricated connected/full-quality result.
    return ORBIS_VR_TRACKER_ERROR_PLAYSTATION_CAMERA_NOT_CONNECTED;
}

s32 PS4_SYSV_ABI sceVrTrackerGetTime(u64* time) {
    LOG_TRACE(Lib_VrTracker, "called");
    if (!g_library_initialized) {
        return ORBIS_VR_TRACKER_ERROR_NOT_INIT;
    }
    if (time == nullptr) {
        return ORBIS_VR_TRACKER_ERROR_ARGUMENT_INVALID;
    }
    *time = Libraries::Kernel::sceKernelGetProcessTime();
    return ORBIS_OK;
}

s32 PS4_SYSV_ABI sceVrTrackerGpuSubmit(const OrbisVrTrackerGpuSubmitParam* param) {
    LOG_ERROR(Lib_VrTracker, "(STUBBED) called");
    if (!g_library_initialized) {
        return ORBIS_VR_TRACKER_ERROR_NOT_INIT;
    }

    if (VirtualSbsEnabled()) {
        if (!param || param->size != sizeof(OrbisVrTrackerGpuSubmitParam))
            return ORBIS_VR_TRACKER_ERROR_ARGUMENT_INVALID;
        return ORBIS_OK;
    }
    // Impossible to submit valid data here since sceCameraGetFrameData returns an error.
    return ORBIS_VR_TRACKER_ERROR_ARGUMENT_INVALID;
}

s32 PS4_SYSV_ABI sceVrTrackerGpuWait(const OrbisVrTrackerGpuWaitParam* param) {
    LOG_ERROR(Lib_VrTracker, "(STUBBED) called");
    if (!g_library_initialized) {
        return ORBIS_VR_TRACKER_ERROR_NOT_INIT;
    }
    if (param == nullptr || param->size != sizeof(OrbisVrTrackerGpuWaitParam)) {
        return ORBIS_VR_TRACKER_ERROR_ARGUMENT_INVALID;
    }

    if (VirtualSbsEnabled())
        return ORBIS_OK;
    // Impossible to perform GPU submits
    return ORBIS_VR_TRACKER_ERROR_NOT_EXECUTE_GPU_SUBMIT;
}

s32 PS4_SYSV_ABI sceVrTrackerGpuWaitAndCpuProcess() {
    LOG_ERROR(Lib_VrTracker, "(STUBBED) called");
    if (!g_library_initialized) {
        return ORBIS_VR_TRACKER_ERROR_NOT_INIT;
    }

    if (VirtualSbsEnabled())
        return ORBIS_OK;
    // Impossible to perform GPU submits
    return ORBIS_VR_TRACKER_ERROR_NOT_EXECUTE_GPU_SUBMIT;
}

s32 PS4_SYSV_ABI
sceVrTrackerNotifyEndOfCpuProcess(const OrbisVrTrackerNotifyEndOfCpuProcessParam* param) {
    if (!g_library_initialized) return ORBIS_VR_TRACKER_ERROR_NOT_INIT;
    if (!param || param->size != sizeof(*param) ||
        std::ranges::any_of(param->reserved, [](u32 v) { return v != 0; }))
        return ORBIS_VR_TRACKER_ERROR_ARGUMENT_INVALID;
    if (!VirtualSbsEnabled()) return ORBIS_VR_TRACKER_ERROR_NOT_EXECUTE_GPU_SUBMIT;
    // All virtual sensor updates are synchronous. There are no optical jobs or
    // retained camera buffers to notify; real-camera mode remains unavailable.
    return ORBIS_OK;
}

s32 PS4_SYSV_ABI sceVrTrackerRecalibrate(const OrbisVrTrackerRecalibrateParam* param) {
    LOG_INFO(Lib_VrTracker, "called, device_type = {}",
             param ? static_cast<u32>(param->device_type) : 0u);
    if (!g_library_initialized) {
        return ORBIS_VR_TRACKER_ERROR_NOT_INIT;
    }
    if (param == nullptr || param->size != sizeof(OrbisVrTrackerRecalibrateParam) ||
        param->device_type > OrbisVrTrackerDeviceType::ORBIS_VR_TRACKER_DEVICE_GUN) {
        return ORBIS_VR_TRACKER_ERROR_ARGUMENT_INVALID;
    }

    OrbisVrTrackerDeviceType device_type = param->device_type;
    switch (device_type) {
    case OrbisVrTrackerDeviceType::ORBIS_VR_TRACKER_DEVICE_HMD: {
        // Seems like the lack of a connected hmd results in this?
        if (!VirtualSbsEnabled() || g_hmd_handle == -1)
            return ORBIS_VR_TRACKER_ERROR_DEVICE_NOT_REGISTERED;
        break;
    }
    case OrbisVrTrackerDeviceType::ORBIS_VR_TRACKER_DEVICE_DUALSHOCK4: {
        if (g_pads.Empty()) {
            return ORBIS_VR_TRACKER_ERROR_DEVICE_NOT_REGISTERED;
        }
        break;
    }
    case OrbisVrTrackerDeviceType::ORBIS_VR_TRACKER_DEVICE_MOVE: {
        if (std::none_of(g_move_handles.begin(), g_move_handles.end(),
                         [](s32 value) { return value != -1; })) {
            return ORBIS_VR_TRACKER_ERROR_DEVICE_NOT_REGISTERED;
        }
        break;
    }
    case OrbisVrTrackerDeviceType::ORBIS_VR_TRACKER_DEVICE_GUN: {
        if (g_gun_handle == -1) {
            return ORBIS_VR_TRACKER_ERROR_DEVICE_NOT_REGISTERED;
        }
        break;
    }
    default: {
        // Shouldn't be possible to hit this.
        UNREACHABLE();
    }
    }

    if (device_type == OrbisVrTrackerDeviceType::ORBIS_VR_TRACKER_DEVICE_DUALSHOCK4 &&
        VirtualSbsEnabled()) {
        // Whatever the stand-in controller points at now is straight ahead and level.
        const auto sensor = GuestVrSensor::Instance().Read();
        if (sensor.openxr) {
            std::scoped_lock lock{g_ds4_mutex};
            g_ds4.Recalibrate(sensor.hardware.head);
        }
    }
    // Results for this kind of device say CALIBRATING for a moment (see Recalibrations).
    g_recalibrations.Begin(device_type, Libraries::Kernel::sceKernelGetProcessTime());
    return ORBIS_OK;
}

s32 PS4_SYSV_ABI sceVrTrackerResetAll() {
    LOG_ERROR(Lib_VrTracker, "(STUBBED) called");
    return ORBIS_OK;
}

s32 PS4_SYSV_ABI sceVrTrackerResetOrientationRelative(const OrbisVrTrackerDeviceType device_type,
                                                      const s32 handle) {
    LOG_ERROR(Lib_VrTracker, "(STUBBED) called");
    if (VirtualSbsEnabled() && device_type == ORBIS_VR_TRACKER_DEVICE_HMD) {
        if (!g_library_initialized || handle != g_hmd_handle)
            return ORBIS_VR_TRACKER_ERROR_DEVICE_NOT_REGISTERED;
        GuestVrSensor::Instance().ResetOrientation();
        return ORBIS_OK;
    }
    return ORBIS_OK;
}

s32 PS4_SYSV_ABI sceVrTrackerSaveInternalBuffers() {
    LOG_ERROR(Lib_VrTracker, "(STUBBED) called");
    return ORBIS_OK;
}

s32 PS4_SYSV_ABI sceVrTrackerSetDurationUntilStatusNotTracking(
    const OrbisVrTrackerDeviceType device_type, const u32 duration_camera_frames) {
    LOG_ERROR(Lib_VrTracker, "(STUBBED) called");
    if (!g_library_initialized) {
        return ORBIS_VR_TRACKER_ERROR_NOT_INIT;
    }
    if (device_type > OrbisVrTrackerDeviceType::ORBIS_VR_TRACKER_DEVICE_GUN) {
        return ORBIS_VR_TRACKER_ERROR_ARGUMENT_INVALID;
    }

    // Seems to unconditionally return 0 when parameters are valid.
    return ORBIS_OK;
}

s32 PS4_SYSV_ABI sceVrTrackerSetExtendedMode() {
    LOG_ERROR(Lib_VrTracker, "(STUBBED) called");
    return ORBIS_OK;
}

s32 PS4_SYSV_ABI sceVrTrackerSetLEDBrightness() {
    LOG_ERROR(Lib_VrTracker, "(STUBBED) called");
    return ORBIS_OK;
}

s32 PS4_SYSV_ABI sceVrTrackerSetRestingMode() {
    LOG_ERROR(Lib_VrTracker, "(STUBBED) called");
    return ORBIS_OK;
}

s32 PS4_SYSV_ABI
sceVrTrackerUpdateMotionSensorData(const OrbisVrTrackerUpdateMotionSensorDataParam* param) {
    LOG_ERROR(Lib_VrTracker, "(STUBBED) called");
    if (VirtualSbsEnabled()) {
        if (!g_library_initialized || !param || param->size != sizeof(*param) ||
            param->device_type != ORBIS_VR_TRACKER_DEVICE_HMD)
            return ORBIS_VR_TRACKER_ERROR_ARGUMENT_INVALID;
        return ORBIS_OK;
    }
    return ORBIS_OK;
}

s32 PS4_SYSV_ABI Func_0FA4C949F8D3024E() {
    LOG_ERROR(Lib_VrTracker, "(STUBBED) called");
    return ORBIS_OK;
}

s32 PS4_SYSV_ABI Func_285C6AFC09C42F7E() {
    LOG_ERROR(Lib_VrTracker, "(STUBBED) called");
    return ORBIS_OK;
}

s32 PS4_SYSV_ABI Func_9A6CDB2103664F8A() {
    LOG_ERROR(Lib_VrTracker, "(STUBBED) called");
    return ORBIS_OK;
}

s32 PS4_SYSV_ABI Func_B4D26B7D8B18DF06(const OrbisVrTrackerDeviceType device_type, const s32 handle,
                                       const s32 led_color) {
    // Undocumented registration variant whose third argument picks the light bar colour (0..4)
    // instead of leaving it to the system. The ABI is AstroQuest's reading of ASTRO BOT's calls
    // (vr_tracker.cpp:618-625), not checked against the firmware here.
    LOG_INFO(Lib_VrTracker, "called, device_type = {}, handle = {}, led_color = {}",
             static_cast<u32>(device_type), handle, led_color);
    return sceVrTrackerRegisterDeviceInternal(device_type, handle, led_color, 1);
}

s32 PS4_SYSV_ABI sceVrTrackerSetDeviceRejection() {
    LOG_ERROR(Lib_VrTracker, "(STUBBED) called");
    return ORBIS_OK;
}

s32 PS4_SYSV_ABI Func_1119B0BE399F37E7() {
    LOG_ERROR(Lib_VrTracker, "(STUBBED) called");
    return ORBIS_OK;
}

s32 PS4_SYSV_ABI Func_4928B43816BC440D() {
    LOG_ERROR(Lib_VrTracker, "(STUBBED) called");
    return ORBIS_OK;
}

s32 PS4_SYSV_ABI Func_863EF32EFCB0FA9C() {
    LOG_ERROR(Lib_VrTracker, "(STUBBED) called");
    return ORBIS_OK;
}

s32 PS4_SYSV_ABI Func_E6E726CBC85C48F9() {
    LOG_ERROR(Lib_VrTracker, "(STUBBED) called");
    return ORBIS_OK;
}

s32 PS4_SYSV_ABI Func_F6407E46C66DF383() {
    LOG_ERROR(Lib_VrTracker, "(STUBBED) called");
    return ORBIS_OK;
}

s32 PS4_SYSV_ABI sceVrTrackerCpuPopMarker() {
    LOG_ERROR(Lib_VrTracker, "(STUBBED) called");
    return ORBIS_OK;
}

s32 PS4_SYSV_ABI sceVrTrackerCpuPushMarker() {
    LOG_ERROR(Lib_VrTracker, "(STUBBED) called");
    return ORBIS_OK;
}

s32 PS4_SYSV_ABI sceVrTrackerGetLiveCaptureId() {
    LOG_ERROR(Lib_VrTracker, "(STUBBED) called");
    return ORBIS_OK;
}

s32 PS4_SYSV_ABI sceVrTrackerStartLiveCapture() {
    LOG_ERROR(Lib_VrTracker, "(STUBBED) called");
    return ORBIS_OK;
}

s32 PS4_SYSV_ABI sceVrTrackerStopLiveCapture() {
    LOG_ERROR(Lib_VrTracker, "(STUBBED) called");
    return ORBIS_OK;
}

s32 PS4_SYSV_ABI sceVrTrackerUnregisterDevice(const s32 handle) {
    LOG_DEBUG(Lib_VrTracker, "called");
    if (!g_library_initialized) {
        return ORBIS_VR_TRACKER_ERROR_NOT_INIT;
    }
    if (handle < 0) {
        return ORBIS_VR_TRACKER_ERROR_ARGUMENT_INVALID;
    }
    // Since this function only takes a handle, compare the handle to registered handles.
    if (handle == g_hmd_handle) {
        g_hmd_handle = -1;
    } else if (g_pads.Unregister(handle)) {
    } else if (std::find(g_move_handles.begin(), g_move_handles.end(), handle) !=
               g_move_handles.end()) {
        for (auto& value : g_move_handles) {
            if (value == handle)
                value = -1;
        }
    } else if (handle == g_gun_handle) {
        g_gun_handle = -1;
    } else {
        // If none of the handles match up, then return an error.
        return ORBIS_VR_TRACKER_ERROR_ARGUMENT_INVALID;
    }

    return ORBIS_OK;
}

s32 PS4_SYSV_ABI sceVrTrackerTerm() {
    LOG_DEBUG(Lib_VrTracker, "called");
    if (!g_library_initialized) {
        return ORBIS_VR_TRACKER_ERROR_NOT_INIT;
    }
    g_library_initialized = false;
    g_move_handles = {-1, -1};
    g_hmd_handle = g_gun_handle = -1;
    g_pads.Clear();
    g_recalibrations.Clear();
    {
        std::scoped_lock lock{g_ds4_mutex};
        g_ds4.Reset();
        g_ds4_recenters = GuestVrSensor::Instance().RecenterCount();
    }
    g_garlic_memory_pointer = g_onion_memory_pointer = g_work_memory_pointer = nullptr;
    g_garlic_size = g_onion_size = g_work_size = 0;
    return ORBIS_OK;
}

void RegisterLib(Core::Loader::SymbolsResolver* sym) {
    LIB_FUNCTION("24kDA+A0Ox0", "libSceVrTrackerFourDeviceAllowed", 1, "libSceVrTracker",
                 sceVrTrackerRegisterDevice2);
    LIB_FUNCTION("5IFOAYv-62g", "libSceVrTracker", 1, "libSceVrTracker", sceVrTrackerCpuProcess);
    LIB_FUNCTION("zvyKP0Z3UvU", "libSceVrTracker", 1, "libSceVrTracker",
                 sceVrTrackerGetPlayAreaWarningInfo);
    LIB_FUNCTION("76OBvrrQXUc", "libSceVrTracker", 1, "libSceVrTracker", sceVrTrackerGetResult);
    LIB_FUNCTION("XoeWzXlrnMw", "libSceVrTracker", 1, "libSceVrTracker", sceVrTrackerGetTime);
    LIB_FUNCTION("TVegDMLaBB8", "libSceVrTracker", 1, "libSceVrTracker", sceVrTrackerGpuSubmit);
    LIB_FUNCTION("gkGuO9dd57M", "libSceVrTracker", 1, "libSceVrTracker", sceVrTrackerGpuWait);
    LIB_FUNCTION("ARhgpXvwoR0", "libSceVrTracker", 1, "libSceVrTracker",
                 sceVrTrackerGpuWaitAndCpuProcess);
    LIB_FUNCTION("QkRl7pART9M", "libSceVrTracker", 1, "libSceVrTracker", sceVrTrackerInit);
    LIB_FUNCTION("VItTwN8DmS8", "libSceVrTracker", 1, "libSceVrTracker",
                 sceVrTrackerNotifyEndOfCpuProcess);
    LIB_FUNCTION("K7yhYrsIBPc", "libSceVrTracker", 1, "libSceVrTracker", sceVrTrackerQueryMemory);
    LIB_FUNCTION("EUCaQtXXYNI", "libSceVrTracker", 1, "libSceVrTracker", sceVrTrackerRecalibrate);
    LIB_FUNCTION("sIh8GwcevaQ", "libSceVrTracker", 1, "libSceVrTracker",
                 sceVrTrackerRegisterDevice);
    LIB_FUNCTION("ufexf4aNiwg", "libSceVrTracker", 1, "libSceVrTracker",
                 sceVrTrackerRegisterDeviceInternal);
    LIB_FUNCTION("CtWUbFgmq+I", "libSceVrTracker", 1, "libSceVrTracker", sceVrTrackerResetAll);
    LIB_FUNCTION("E0P0sN-wy+4", "libSceVrTracker", 1, "libSceVrTracker",
                 sceVrTrackerResetOrientationRelative);
    LIB_FUNCTION("bDGZVTwwZ1A", "libSceVrTracker", 1, "libSceVrTracker",
                 sceVrTrackerSaveInternalBuffers);
    LIB_FUNCTION("qBjnR0HtMYI", "libSceVrTracker", 1, "libSceVrTracker",
                 sceVrTrackerSetDurationUntilStatusNotTracking);
    LIB_FUNCTION("NhPkY3V8E+8", "libSceVrTracker", 1, "libSceVrTracker",
                 sceVrTrackerSetExtendedMode);
    LIB_FUNCTION("vpsLLotiSUg", "libSceVrTracker", 1, "libSceVrTracker",
                 sceVrTrackerSetLEDBrightness);
    LIB_FUNCTION("lgWSHQ8p4i4", "libSceVrTracker", 1, "libSceVrTracker",
                 sceVrTrackerSetRestingMode);
    LIB_FUNCTION("IBv4P3q1pQ0", "libSceVrTracker", 1, "libSceVrTracker", sceVrTrackerTerm);
    LIB_FUNCTION("Q8skQqEwn5c", "libSceVrTracker", 1, "libSceVrTracker",
                 sceVrTrackerUnregisterDevice);
    LIB_FUNCTION("9fvHMUbsom4", "libSceVrTracker", 1, "libSceVrTracker",
                 sceVrTrackerUpdateMotionSensorData);
    LIB_FUNCTION("D6TJSfjTAk4", "libSceVrTracker", 1, "libSceVrTracker", Func_0FA4C949F8D3024E);
    LIB_FUNCTION("KFxq-AnEL34", "libSceVrTracker", 1, "libSceVrTracker", Func_285C6AFC09C42F7E);
    LIB_FUNCTION("mmzbIQNmT4o", "libSceVrTracker", 1, "libSceVrTracker", Func_9A6CDB2103664F8A);
    LIB_FUNCTION("tNJrfYsY3wY", "libSceVrTracker", 1, "libSceVrTracker", Func_B4D26B7D8B18DF06);
    LIB_FUNCTION("jGqEkPy0iLU", "libSceVrTrackerDeviceRejection", 1, "libSceVrTracker",
                 sceVrTrackerSetDeviceRejection);
    LIB_FUNCTION("ERmwvjmfN+c", "libSceVrTrackerGpuTest", 1, "libSceVrTracker",
                 Func_1119B0BE399F37E7);
    LIB_FUNCTION("SSi0OBa8RA0", "libSceVrTrackerGpuTest", 1, "libSceVrTracker",
                 Func_4928B43816BC440D);
    LIB_FUNCTION("hj7zLvyw+pw", "libSceVrTrackerGpuTest", 1, "libSceVrTracker",
                 Func_863EF32EFCB0FA9C);
    LIB_FUNCTION("5ucmy8hcSPk", "libSceVrTrackerGpuTest", 1, "libSceVrTracker",
                 Func_E6E726CBC85C48F9);
    LIB_FUNCTION("9kB+RsZt84M", "libSceVrTrackerGpuTest", 1, "libSceVrTracker",
                 Func_F6407E46C66DF383);
    LIB_FUNCTION("sBkAqyF5Gns", "libSceVrTrackerLiveCapture", 1, "libSceVrTracker",
                 sceVrTrackerCpuPopMarker);
    LIB_FUNCTION("rvCywCbc7Pk", "libSceVrTrackerLiveCapture", 1, "libSceVrTracker",
                 sceVrTrackerCpuPushMarker);
    LIB_FUNCTION("lm6T1Ur6JRk", "libSceVrTrackerLiveCapture", 1, "libSceVrTracker",
                 sceVrTrackerGetLiveCaptureId);
    LIB_FUNCTION("qa1+CeXKDPc", "libSceVrTrackerLiveCapture", 1, "libSceVrTracker",
                 sceVrTrackerStartLiveCapture);
    LIB_FUNCTION("3YCwwpHkHIg", "libSceVrTrackerLiveCapture", 1, "libSceVrTracker",
                 sceVrTrackerStopLiveCapture);
};

} // namespace Libraries::VrTracker

namespace Core::HostRuntime {
static std::atomic<Ds4PoseSource> g_ds4_source{Ds4PoseSource::Off};
void SetDs4PoseSource(Ds4PoseSource source) {
    g_ds4_source.store(source, std::memory_order_relaxed);
}
Ds4PoseSource GetDs4PoseSource() {
    return g_ds4_source.load(std::memory_order_relaxed);
}
static std::atomic<bool> g_seat_offset{true};
void SetTrackerSeatOffset(bool on) {
    g_seat_offset.store(on, std::memory_order_relaxed);
}
bool TrackerSeatOffset() {
    return g_seat_offset.load(std::memory_order_relaxed);
}
} // namespace Core::HostRuntime

namespace Core::HostRuntime {
void SetDs4RightOffset(const std::array<float, 3>& offset) {
    using namespace Libraries::VrTracker;
    std::scoped_lock lock{g_ds4_mutex};
    g_ds4.SetRightOffset(offset);
}
std::array<float, 3> Ds4RightOffset() {
    using namespace Libraries::VrTracker;
    std::scoped_lock lock{g_ds4_mutex};
    return g_ds4.RightOffset();
}
void MoveDs4OwnPlace(const std::array<float, 3>& by) {
    using namespace Libraries::VrTracker;
    std::scoped_lock lock{g_ds4_mutex};
    LoadDs4OwnPlaceLocked();
    const auto place = g_ds4.MoveOwnOffset(by);
    SaveDs4OwnPlaceLocked();
    LOG_INFO(Lib_VrTracker,
             "DualShock 4 own place: {:.2f} m right of the head, {:.2f} below, {:.2f} ahead",
             place[0], -place[1], -place[2]);
}
void SwitchDs4OwnPlace() {
    using namespace Libraries::VrTracker;
    std::scoped_lock lock{g_ds4_mutex};
    LoadDs4OwnPlaceLocked();
    if (!g_ds4.SwitchOwnPlace())
        return;
    SaveDs4OwnPlaceLocked();
    LOG_INFO(Lib_VrTracker, "DualShock 4 out of sight is held at the {} place",
             g_ds4.OwnPlaceUsed() ? "player's own" : "seen/standard");
}
} // namespace Core::HostRuntime
