// SPDX-FileCopyrightText: Copyright 2026 shadPS4 Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#pragma once

#include <string>
#include <vector>

#include "common/types.h"

/// What the game asked of the controller motors and what became of it, on both hosts:
/// desktop (SDL rumble) and Android (OrbisPadAdapter -> InputHub -> controller vibrator).
namespace Libraries::Pad::Vibration {

enum class Outcome : u32 {
    Sent,          ///< Handed to the host controller (SDL rumble / Android haptic queue).
    NoHandle,      ///< Unknown pad handle.
    NotConnected,  ///< The pad is not connected.
    NoActuator,    ///< No physical controller with a motor is bound to this pad.
    HostRejected,  ///< The host refused it (SDL error, stale session, unsupported device).
    InvalidArgs,
    Count,
};

/// Records one scePadSetVibration call. The first calls and every change of outcome are logged.
void Record(s32 handle, u8 large, u8 small, Outcome outcome, const char* detail = nullptr);

/// DebugBus `pad_vibration status | test LARGE SMALL [HANDLE]`: the test goes through the same
/// scePadSetVibration entry the game uses.
std::string Command(const std::vector<std::string>& args);

} // namespace Libraries::Pad::Vibration
