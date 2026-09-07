// SPDX-FileCopyrightText: Copyright 2026 shadPS4 Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

// Public guest-CPU status/error vocabulary.
//
// Nothing in this header may reference a backend type. A consumer that only
// wants to inspect results must be able to include it with no FEX include
// path and no FEX compile definitions (acceptance B05).

#pragma once

#include <cstdint>
#include <string>
#include <string_view>

namespace Core::GuestCpu {

// Why an operation failed. Deliberately not a bool: the acceptance matrix
// requires a caller to be able to distinguish "you asked at the wrong time"
// from "the backend broke" without parsing a log line.
enum class ErrorCategory : std::uint32_t {
    None = 0,
    InvalidArgument,   // Caller-supplied value is malformed on its face.
    InvalidHandle,     // Handle is stale, destroyed, or from another context.
    AlreadyActive,     // A second live context/thread was requested.
    AlreadyRunning,    // Target is executing; the request needs a stopped one.
    Busy,              // Momentarily unavailable; retry is meaningful.
    Timeout,           // A bounded wait expired without reaching the state.
    WrongThread,       // Issued from a thread that does not own the target.
    WrongState,        // Legal call, illegal current state.
    StaleEpoch,        // Caller's stop epoch is older than the current one.
    Unsupported,       // Understood, deliberately not implemented in V0.
    UnsupportedMemoryMode,  // e.g. SoftwareCallbacks / MMIO was requested.
    PermissionDenied,  // Guest permissions forbid the access.
    OutOfMemory,
    BackendFailure,    // Backend reported an internal failure.
};

[[nodiscard]] std::string_view ToString(ErrorCategory category) noexcept;

// A failure that keeps enough context to be actionable after the fact.
struct Error final {
    ErrorCategory category{ErrorCategory::None};
    // Stable identifier of the failing operation, e.g. "GuestMemory::Protect".
    std::string_view operation{};
    // Thread and stop epoch at failure time; 0 when not thread-scoped.
    std::uint64_t thread_id{};
    std::uint64_t stop_epoch{};
    // errno, or a backend-specific code. 0 when neither applies.
    std::int64_t system_error{};
    // Short structured note. Not a formatted user message.
    std::string detail{};

    [[nodiscard]] bool IsError() const noexcept {
        return category != ErrorCategory::None;
    }
};

[[nodiscard]] Error MakeError(ErrorCategory category, std::string_view operation,
                              std::string detail = {});

[[nodiscard]] std::string Describe(const Error& error);

} // namespace Core::GuestCpu
