// SPDX-FileCopyrightText: Copyright 2024 shadPS4 Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#include "common/logging/log.h"
#include <algorithm>
#include <array>
#include <atomic>
#include <cmath>
#include "core/libraries/error_codes.h"
#include "core/host_runtime/guest_vr_sensor.h"
#include "core/libraries/libs.h"
#include "core/libraries/move/move.h"
#include "core/libraries/move/move_error.h"
#include "move.h"

namespace Libraries::Move {

static bool g_library_initialized = false;
static std::array<s32, 2> g_virtual_handles{-1, -1};
static s32 g_next_handle = 0x30b0000;

constexpr u64 kPadTriangle = 0x1000;
constexpr u64 kPadCircle = 0x2000;
constexpr u64 kPadCross = 0x4000;
constexpr u64 kPadSquare = 0x8000;

enum class VirtualHand : u8 { Left = 0, Right = 1 };

VirtualHand HandForHandle(s32 handle) {
    return handle == g_virtual_handles[1] ? VirtualHand::Right : VirtualHand::Left;
}

bool IsVirtualHandle(s32 handle) {
    return handle == g_virtual_handles[0] || handle == g_virtual_handles[1];
}

u16 AxisToMove(u16 value) {
    return value;
}

u16 AxisToMove(float value) {
    const float normalized = std::clamp(value, -1.0f, 1.0f);
    return static_cast<u16>(std::lround((normalized * 0.5f + 0.5f) * 65535.0f));
}

u16 FaceButtonsToMove(u64 buttons) {
    u16 result = 0;
    if (buttons & kPadTriangle) result |= ORBIS_MOVE_BUTTON_TRIANGLE;
    if (buttons & kPadCircle) result |= ORBIS_MOVE_BUTTON_CIRCLE;
    if (buttons & kPadCross) result |= ORBIS_MOVE_BUTTON_CROSS;
    if (buttons & kPadSquare) result |= ORBIS_MOVE_BUTTON_SQUARE;
    return result;
}

void FillVirtualState(OrbisMoveData& data, VirtualHand hand) {
    data = {};
    const auto input = Core::HostRuntime::GuestVrSensor::Instance().ReadMoveInput();
    const auto quantize = [](float value) {
        return static_cast<u64>(std::clamp<int>(
            static_cast<int>(std::lround((std::clamp(value, -1.0f, 1.0f) + 1.0f) * 127.5f)),
            0, 255));
    };
    const u64 fingerprint = input.buttons ^ (quantize(input.left_x) << 32) ^
                            (quantize(input.left_y) << 40) ^ (quantize(input.right_x) << 48) ^
                            (quantize(input.right_y) << 56);
    static std::atomic<u64> last_fingerprint{~u64{0}};
    if (last_fingerprint.exchange(fingerprint, std::memory_order_relaxed) != fingerprint) {
        LOG_INFO(Lib_Move, "virtual Move input buttons={:#x} sticks=({}, {})/({}, {})",
                 input.buttons, input.left_x, input.left_y, input.right_x, input.right_y);
    }
    // A Move has no independent orientation record.  Keep its IMU sample
    // stable; the fixed pose is supplied by VrTracker below.  Gravity is
    // still present so games that sanity-check the sample do not reject it.
    data.accelerometer[1] = 9.81f;
    data.button_data.button_data = FaceButtonsToMove(input.buttons);
    const float trigger = hand == VirtualHand::Left ? input.left_trigger : input.right_trigger;
    data.button_data.trigger_data = static_cast<u16>(std::lround(trigger * 255.0f));
    if (hand == VirtualHand::Left) {
        data.extension_data.analog_left_x = AxisToMove(input.left_x);
        data.extension_data.analog_left_y = AxisToMove(input.left_y);
    } else {
        data.extension_data.analog_right_x = AxisToMove(input.right_x);
        data.extension_data.analog_right_y = AxisToMove(input.right_y);
    }
    data.timestamp = static_cast<s64>(input.timestamp_us);
    data.count = 1;
    data.temperature = 25.0f;
}

bool VirtualSbsEnabled() {
    return Core::HostRuntime::GuestVrSensor::Instance().Read().enabled;
}

s32 PS4_SYSV_ABI sceMoveInit() {
    if (g_library_initialized) {
        return ORBIS_MOVE_ERROR_ALREADY_INIT;
    }
    if (!VirtualSbsEnabled())
        LOG_WARNING(Lib_Move, "Move controllers are not supported yet");
    else
        LOG_INFO(Lib_Move, "virtual SBS Move provider initialized (gyro only)");
    g_library_initialized = true;
    return ORBIS_OK;
}

s32 PS4_SYSV_ABI sceMoveOpen(Libraries::UserService::OrbisUserServiceUserId user_id, s32 type,
                             s32 index) {
    LOG_DEBUG(Lib_Move, "called user={} type={} index={}", user_id, type, index);
    if (!g_library_initialized) {
        return ORBIS_MOVE_ERROR_NOT_INIT;
    }
    // Keep desktop's stable synthetic-handle behavior.  In SBS, retain the
    // left/right identity so both Move records and tracker results line up.
    if (VirtualSbsEnabled()) {
        const int slot = index == 1 ? 1 : 0;
        if (g_virtual_handles[slot] >= 0)
            return ORBIS_MOVE_ERROR_ALREADY_OPENED;
        const s32 handle = ++g_next_handle;
        g_virtual_handles[slot] = handle;
        return handle;
    }
    return ++g_next_handle;
}

s32 PS4_SYSV_ABI sceMoveGetDeviceInfo(s32 handle, OrbisMoveDeviceInfo* info) {
    LOG_TRACE(Lib_Move, "called");
    if (!g_library_initialized) {
        return ORBIS_MOVE_ERROR_NOT_INIT;
    }
    if (info == nullptr) {
        return ORBIS_MOVE_ERROR_INVALID_ARG;
    }
    if (VirtualSbsEnabled()) {
        *info = {};
        info->sphere_radius = 0.04f;
        return ORBIS_OK;
    }
    return ORBIS_MOVE_ERROR_NO_CONTROLLER_CONNECTED;
}

s32 PS4_SYSV_ABI sceMoveReadStateLatest(s32 handle, OrbisMoveData* data) {
    LOG_TRACE(Lib_Move, "(called");
    if (!g_library_initialized) {
        return ORBIS_MOVE_ERROR_NOT_INIT;
    }
    if (data == nullptr) {
        return ORBIS_MOVE_ERROR_INVALID_ARG;
    }
    if (VirtualSbsEnabled()) {
        if (!IsVirtualHandle(handle))
            return ORBIS_MOVE_ERROR_INVALID_HANDLE;
        static std::atomic<u32> state_logs{};
        if (state_logs.fetch_add(1, std::memory_order_relaxed) < 8) {
            const auto input = Core::HostRuntime::GuestVrSensor::Instance().ReadMoveInput();
            LOG_INFO(Lib_Move, "virtual Move latest handle={:#x} hand={} buttons={:#x} sticks=({}, {})/({}, {})",
                     handle, HandForHandle(handle) == VirtualHand::Left ? "left" : "right",
                     input.buttons, input.left_x, input.left_y, input.right_x, input.right_y);
        }
        FillVirtualState(*data, HandForHandle(handle));
        return ORBIS_OK;
    }
    return ORBIS_MOVE_ERROR_NO_CONTROLLER_CONNECTED;
}

s32 PS4_SYSV_ABI sceMoveReadStateRecent(s32 handle, s64 timestamp, OrbisMoveData* data,
                                        s32* out_count) {
    LOG_TRACE(Lib_Move, "called");
    if (!g_library_initialized) {
        return ORBIS_MOVE_ERROR_NOT_INIT;
    }
    if (timestamp < 0 || data == nullptr || out_count == nullptr) {
        return ORBIS_MOVE_ERROR_INVALID_ARG;
    }
    if (VirtualSbsEnabled()) {
        if (!IsVirtualHandle(handle))
            return ORBIS_MOVE_ERROR_INVALID_HANDLE;
        static std::atomic<u32> state_logs{};
        if (state_logs.fetch_add(1, std::memory_order_relaxed) < 8) {
            const auto input = Core::HostRuntime::GuestVrSensor::Instance().ReadMoveInput();
            LOG_INFO(Lib_Move, "virtual Move recent handle={:#x} hand={} buttons={:#x} sticks=({}, {})/({}, {})",
                     handle, HandForHandle(handle) == VirtualHand::Left ? "left" : "right",
                     input.buttons, input.left_x, input.left_y, input.right_x, input.right_y);
        }
        FillVirtualState(*data, HandForHandle(handle));
        if (data->timestamp == 0)
            data->timestamp = timestamp;
        *out_count = 1;
        return ORBIS_OK;
    }
    return ORBIS_MOVE_ERROR_NO_CONTROLLER_CONNECTED;
}

s32 PS4_SYSV_ABI sceMoveGetExtensionPortInfo(s32 handle, void* data) {
    LOG_TRACE(Lib_Move, "called");
    if (!g_library_initialized) {
        return ORBIS_MOVE_ERROR_NOT_INIT;
    }
    if (data == nullptr) {
        return ORBIS_MOVE_ERROR_INVALID_ARG;
    }
    if (VirtualSbsEnabled()) {
        // The SBS adapter has no physical Move extension port.  Keep the
        // documented no-device result rather than inventing an opaque ABI.
        return ORBIS_MOVE_ERROR_NO_CONTROLLER_CONNECTED;
    }
    return ORBIS_MOVE_ERROR_NO_CONTROLLER_CONNECTED;
}

s32 PS4_SYSV_ABI sceMoveSetVibration(s32 handle, u8 intensity) {
    LOG_TRACE(Lib_Move, "called");
    if (!g_library_initialized) {
        return ORBIS_MOVE_ERROR_NOT_INIT;
    }
    if (VirtualSbsEnabled())
        return ORBIS_OK;
    return ORBIS_MOVE_ERROR_NO_CONTROLLER_CONNECTED;
}

s32 PS4_SYSV_ABI sceMoveSetLightSphere(s32 handle, u8 red, u8 green, u8 blue) {
    LOG_TRACE(Lib_Move, "called");
    if (!g_library_initialized) {
        return ORBIS_MOVE_ERROR_NOT_INIT;
    }
    if (VirtualSbsEnabled())
        return ORBIS_OK;
    return ORBIS_MOVE_ERROR_NO_CONTROLLER_CONNECTED;
}

s32 PS4_SYSV_ABI sceMoveResetLightSphere(s32 handle) {
    LOG_TRACE(Lib_Move, "called");
    if (!g_library_initialized) {
        return ORBIS_MOVE_ERROR_NOT_INIT;
    }
    // Returns success, even if no controllers are connected.
    return ORBIS_OK;
}

s32 PS4_SYSV_ABI sceMoveClose(s32 handle) {
    LOG_DEBUG(Lib_Move, "called");
    if (!g_library_initialized) {
        return ORBIS_MOVE_ERROR_NOT_INIT;
    }
    if (VirtualSbsEnabled()) {
        for (auto& value : g_virtual_handles) {
            if (value == handle)
                value = -1;
        }
    }
    return ORBIS_OK;
}

s32 PS4_SYSV_ABI sceMoveTerm() {
    LOG_DEBUG(Lib_Move, "called");
    if (!g_library_initialized) {
        return ORBIS_MOVE_ERROR_NOT_INIT;
    }
    g_library_initialized = false;
    g_virtual_handles = {-1, -1};
    return ORBIS_OK;
}

void RegisterLib(Core::Loader::SymbolsResolver* sym) {
    LIB_FUNCTION("j1ITE-EoJmE", "libSceMove", 1, "libSceMove", sceMoveInit);
    LIB_FUNCTION("HzC60MfjJxU", "libSceMove", 1, "libSceMove", sceMoveOpen);
    LIB_FUNCTION("GWXTyxs4QbE", "libSceMove", 1, "libSceMove", sceMoveGetDeviceInfo);
    LIB_FUNCTION("ttU+JOhShl4", "libSceMove", 1, "libSceMove", sceMoveReadStateLatest);
    LIB_FUNCTION("f2bcpK6kJfg", "libSceMove", 1, "libSceMove", sceMoveReadStateRecent);
    LIB_FUNCTION("y5h7f8H1Jnk", "libSceMove", 1, "libSceMove", sceMoveGetExtensionPortInfo);
    LIB_FUNCTION("IFQwtT2CeY0", "libSceMove", 1, "libSceMove", sceMoveSetVibration);
    LIB_FUNCTION("T8KYHPs1JE8", "libSceMove", 1, "libSceMove", sceMoveSetLightSphere);
    LIB_FUNCTION("zuxWAg3HAac", "libSceMove", 1, "libSceMove", sceMoveResetLightSphere);
    LIB_FUNCTION("XX6wlxpHyeo", "libSceMove", 1, "libSceMove", sceMoveClose);
    LIB_FUNCTION("tsZi60H4ypY", "libSceMove", 1, "libSceMove", sceMoveTerm);
};

} // namespace Libraries::Move
