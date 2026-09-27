// SPDX-FileCopyrightText: Copyright 2026 shadPS4 Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#pragma once

#include <optional>

#include "common/types.h"
#include "core/libraries/kernel/sync/object_table.h"
#include "core/libraries/kernel/threads/event_flag_state.h"

namespace Libraries::Kernel::Sync {

/// Orbis event flags (sceKernelCreateEventFlag family), shared by the desktop kernel and the
/// Android host runtime: the flag state machine (EventFlagState), the guest handle table and
/// the argument rules. The platform layers only move guest memory.

using EventFlagTable = ObjectTable<EventFlagState>;

struct EventFlagAttributes {
    EventFlagState::ThreadMode thread_mode;
    EventFlagState::QueueMode queue_mode;
};

/// Creation attribute: 0x01 FIFO or 0x02 priority queue, 0x10 single or 0x20 multiple waiters.
inline std::optional<EventFlagAttributes> DecodeEventFlagAttributes(u32 attr) {
    if ((attr & ~0x33u) != 0 || (attr & 0xfu) > 2 || (attr & 0xf0u) > 0x20) {
        return std::nullopt;
    }
    return EventFlagAttributes{
        (attr & 0x20) ? EventFlagState::ThreadMode::Multi : EventFlagState::ThreadMode::Single,
        (attr & 0x02) ? EventFlagState::QueueMode::ThreadPrio : EventFlagState::QueueMode::Fifo,
    };
}

struct EventFlagWaitMode {
    EventFlagState::WaitMode wait;
    EventFlagState::ClearMode clear;
};

/// Wait/poll mode: 0x01 all bits or 0x02 any bit, then 0x10 clear all or 0x20 clear the pattern.
inline std::optional<EventFlagWaitMode> DecodeEventFlagWaitMode(u32 mode) {
    if ((mode & 0xfu) < 1 || (mode & 0xfu) > 2 || (mode & ~0x3fu) != 0 || (mode & 0xf0u) == 0x30) {
        return std::nullopt;
    }
    return EventFlagWaitMode{
        (mode & 0xf) == 1 ? EventFlagState::WaitMode::And : EventFlagState::WaitMode::Or,
        (mode & 0x10)   ? EventFlagState::ClearMode::All
        : (mode & 0x20) ? EventFlagState::ClearMode::Bits
                        : EventFlagState::ClearMode::None,
    };
}

} // namespace Libraries::Kernel::Sync
