// SPDX-FileCopyrightText: Copyright 2024 shadPS4 Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#pragma once

#include "common/types.h"
#include "core/libraries/system/userservice.h"

namespace Core::Loader {
class SymbolsResolver;
}

namespace Libraries::Move {

struct OrbisMoveDeviceInfo {
    float sphere_radius;
    float accelerometer_offset[3];
};

struct OrbisMoveButtonData {
    u16 button_data;
    u16 trigger_data;
};

struct OrbisMoveExtensionPortData {
    u16 status;
    u16 digital0;
    u16 digital1;
    u16 analog_right_x;
    u16 analog_right_y;
    u16 analog_left_x;
    u16 analog_left_y;
    unsigned char custom[5];
};

struct OrbisMoveData {
    float accelerometer[3];
    float gyro[3];
    OrbisMoveButtonData button_data;
    OrbisMoveExtensionPortData extension_data;
    s64 timestamp;
    s32 count;
    float temperature;
};

// The low four bits are the face-button portion used by the Android SBS
// adapter.  These values follow the Move digital button ordering; the
// adapter only exposes the four PS4 face buttons and leaves the remaining
// device-specific bits clear.
enum OrbisMoveFaceButton : u16 {
    ORBIS_MOVE_BUTTON_TRIANGLE = 1u << 0,
    ORBIS_MOVE_BUTTON_CIRCLE = 1u << 1,
    ORBIS_MOVE_BUTTON_CROSS = 1u << 2,
    ORBIS_MOVE_BUTTON_SQUARE = 1u << 3,
};

static_assert(sizeof(OrbisMoveDeviceInfo) == 0x10);
static_assert(sizeof(OrbisMoveButtonData) == 0x04);
static_assert(sizeof(OrbisMoveExtensionPortData) == 0x14);
static_assert(sizeof(OrbisMoveData) == 0x40);

s32 PS4_SYSV_ABI sceMoveInit();
s32 PS4_SYSV_ABI sceMoveOpen(Libraries::UserService::OrbisUserServiceUserId user_id, s32 type,
                             s32 index);
s32 PS4_SYSV_ABI sceMoveGetDeviceInfo(s32 handle, OrbisMoveDeviceInfo* info);
s32 PS4_SYSV_ABI sceMoveReadStateLatest(s32 handle, OrbisMoveData* data);
s32 PS4_SYSV_ABI sceMoveReadStateRecent(s32 handle, s64 timestamp, OrbisMoveData* data,
                                        s32* out_count);
s32 PS4_SYSV_ABI sceMoveSetVibration(s32 handle, u8 intensity);
s32 PS4_SYSV_ABI sceMoveSetLightSphere(s32 handle, u8 red, u8 green, u8 blue);
s32 PS4_SYSV_ABI sceMoveTerm();

void RegisterLib(Core::Loader::SymbolsResolver* sym);
} // namespace Libraries::Move
