// SPDX-FileCopyrightText: Copyright 2026 shadPS4 Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#include "core/guest_cpu/api/status.h"

#include <array>

namespace Core::GuestCpu {

std::string_view ToString(ErrorCategory category) noexcept {
    switch (category) {
    case ErrorCategory::None: return "None";
    case ErrorCategory::InvalidArgument: return "InvalidArgument";
    case ErrorCategory::InvalidHandle: return "InvalidHandle";
    case ErrorCategory::AlreadyActive: return "AlreadyActive";
    case ErrorCategory::AlreadyRunning: return "AlreadyRunning";
    case ErrorCategory::Busy: return "Busy";
    case ErrorCategory::Timeout: return "Timeout";
    case ErrorCategory::WrongThread: return "WrongThread";
    case ErrorCategory::WrongState: return "WrongState";
    case ErrorCategory::StaleEpoch: return "StaleEpoch";
    case ErrorCategory::Unsupported: return "Unsupported";
    case ErrorCategory::UnsupportedMemoryMode: return "UnsupportedMemoryMode";
    case ErrorCategory::PermissionDenied: return "PermissionDenied";
    case ErrorCategory::OutOfMemory: return "OutOfMemory";
    case ErrorCategory::BackendFailure: return "BackendFailure";
    }
    return "Unknown";
}

Error MakeError(ErrorCategory category, std::string_view operation, std::string detail) {
    Error error{};
    error.category = category;
    error.operation = operation;
    error.detail = std::move(detail);
    return error;
}

std::string Describe(const Error& error) {
    std::string out;
    out.reserve(96);
    out.append(ToString(error.category));
    out.append(" in ");
    out.append(error.operation);
    if (error.thread_id != 0) {
        out.append(" thread=").append(std::to_string(error.thread_id));
    }
    if (error.stop_epoch != 0) {
        out.append(" epoch=").append(std::to_string(error.stop_epoch));
    }
    if (error.system_error != 0) {
        out.append(" code=").append(std::to_string(error.system_error));
    }
    if (!error.detail.empty()) {
        out.append(": ").append(error.detail);
    }
    return out;
}

} // namespace Core::GuestCpu
