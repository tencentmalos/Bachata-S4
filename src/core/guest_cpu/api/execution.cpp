// SPDX-FileCopyrightText: Copyright 2026 shadPS4 Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#include "core/guest_cpu/api/execution.h"
#include "core/guest_cpu/api/registers.h"

namespace Core::GuestCpu {

std::string_view ToString(Gpr reg) noexcept {
    switch (reg) {
    case Gpr::Rax: return "rax";
    case Gpr::Rcx: return "rcx";
    case Gpr::Rdx: return "rdx";
    case Gpr::Rbx: return "rbx";
    case Gpr::Rsp: return "rsp";
    case Gpr::Rbp: return "rbp";
    case Gpr::Rsi: return "rsi";
    case Gpr::Rdi: return "rdi";
    case Gpr::R8: return "r8";
    case Gpr::R9: return "r9";
    case Gpr::R10: return "r10";
    case Gpr::R11: return "r11";
    case Gpr::R12: return "r12";
    case Gpr::R13: return "r13";
    case Gpr::R14: return "r14";
    case Gpr::R15: return "r15";
    }
    return "?";
}

std::string_view ToString(SnapshotKind kind) noexcept {
    switch (kind) {
    case SnapshotKind::SafePoint: return "SafePoint";
    case SnapshotKind::HleBoundary: return "HleBoundary";
    case SnapshotKind::LastSafePoint: return "LastSafePoint";
    case SnapshotKind::Faulted: return "Faulted";
    case SnapshotKind::AsyncJitStop: return "AsyncJitStop";
    }
    return "Unknown";
}

std::string_view ToString(StopReason reason) noexcept {
    switch (reason) {
    case StopReason::Returned: return "Returned";
    case StopReason::PauseRequested: return "PauseRequested";
    case StopReason::Cancelled: return "Cancelled";
    case StopReason::StepComplete: return "StepComplete";
    case StopReason::HleBoundary: return "HleBoundary";
    case StopReason::GuestFault: return "GuestFault";
    case StopReason::Unsupported: return "Unsupported";
    case StopReason::BackendFailure: return "BackendFailure";
    }
    return "Unknown";
}

std::string_view ToString(InterruptReason reason) noexcept {
    switch (reason) {
    case InterruptReason::Pause: return "Pause";
    case InterruptReason::Cancel: return "Cancel";
    case InterruptReason::Shutdown: return "Shutdown";
    }
    return "Unknown";
}

StopReason SelectPrimaryReason(StopReasonBits pending) noexcept {
    // Order matters and is dictated by API contract §5:
    //   - A backend failure or guest fault must never be masked by a pause
    //     that happened to arrive at the same moment.
    //   - Cancel outranks pause when both are present.
    //   - Everything else keeps its own identity; nothing collapses into HLT.
    static constexpr StopReason kPriority[] = {
        StopReason::BackendFailure,
        StopReason::GuestFault,
        StopReason::Unsupported,
        StopReason::Cancelled,
        StopReason::PauseRequested,
        StopReason::StepComplete,
        StopReason::HleBoundary,
        StopReason::Returned,
    };
    for (const StopReason reason : kPriority) {
        if (Contains(pending, reason)) {
            return reason;
        }
    }
    return StopReason::Returned;
}

} // namespace Core::GuestCpu
