// SPDX-FileCopyrightText: Copyright 2026 shadPS4 Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#include "core/host_runtime/session_backend.h"

namespace Core::HostRuntime {

const char* ToString(RunOutcome outcome) noexcept {
    switch (outcome) {
    case RunOutcome::Returned: return "Returned";
    case RunOutcome::Cancelled: return "Cancelled";
    case RunOutcome::Faulted: return "Faulted";
    case RunOutcome::BackendFailed: return "BackendFailed";
    case RunOutcome::Unsupported: return "Unsupported";
    case RunOutcome::Unexpected: return "Unexpected";
    case RunOutcome::StartFailed: return "StartFailed";
    }
    return "Unknown";
}

}  // namespace Core::HostRuntime
