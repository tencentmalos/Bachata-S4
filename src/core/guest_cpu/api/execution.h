// SPDX-FileCopyrightText: Copyright 2026 shadPS4 Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

// Run / Step / interrupt vocabulary.
//
// Spec §6 is explicit that a normal return, a pause, a cancel, a guest fault, a
// finished single step and an HLE entry must not all arrive as "HLT". Hence a
// primary reason plus a bitset of everything else that was pending, so a
// simultaneous fault and pause cannot hide one another.

#pragma once

#include <cstdint>
#include <optional>
#include <string_view>

#include "core/guest_cpu/api/registers.h"

namespace Core::GuestCpu {

enum class StopReason : std::uint32_t {
    // Guest reached a registered return gate. A real guest HLT or an illegal
    // opcode is NOT this (D05).
    Returned = 0,
    PauseRequested,
    Cancelled,
    StepComplete,
    HleBoundary,
    GuestFault,
    Unsupported,
    BackendFailure,
};

[[nodiscard]] std::string_view ToString(StopReason reason) noexcept;

// Bit positions mirror StopReason so several pending causes can be reported
// alongside the primary one.
enum class StopReasonBits : std::uint32_t {
    None = 0,
    Returned = 1u << static_cast<std::uint32_t>(StopReason::Returned),
    PauseRequested = 1u << static_cast<std::uint32_t>(StopReason::PauseRequested),
    Cancelled = 1u << static_cast<std::uint32_t>(StopReason::Cancelled),
    StepComplete = 1u << static_cast<std::uint32_t>(StopReason::StepComplete),
    HleBoundary = 1u << static_cast<std::uint32_t>(StopReason::HleBoundary),
    GuestFault = 1u << static_cast<std::uint32_t>(StopReason::GuestFault),
    Unsupported = 1u << static_cast<std::uint32_t>(StopReason::Unsupported),
    BackendFailure = 1u << static_cast<std::uint32_t>(StopReason::BackendFailure),
};

[[nodiscard]] constexpr StopReasonBits operator|(StopReasonBits a, StopReasonBits b) noexcept {
    return static_cast<StopReasonBits>(static_cast<std::uint32_t>(a) |
                                       static_cast<std::uint32_t>(b));
}
constexpr StopReasonBits& operator|=(StopReasonBits& a, StopReasonBits b) noexcept {
    a = a | b;
    return a;
}
[[nodiscard]] constexpr StopReasonBits BitOf(StopReason reason) noexcept {
    return static_cast<StopReasonBits>(1u << static_cast<std::uint32_t>(reason));
}
[[nodiscard]] constexpr bool Contains(StopReasonBits bits, StopReason reason) noexcept {
    return (static_cast<std::uint32_t>(bits) & static_cast<std::uint32_t>(BitOf(reason))) != 0;
}

// Priority when several causes land together (§5): a fault or backend failure
// must never be masked by a pause that arrived at the same moment, and an
// explicit cancel outranks a pause.
[[nodiscard]] StopReason SelectPrimaryReason(StopReasonBits pending) noexcept;

enum class GuestAccessKind : std::uint8_t {
    Read = 0,
    Write,
    Execute,
    Unknown,
};

// A fault carries separate validity flags for guest RIP and fault address.
// D03 forbids labelling a stale RIP as the fault RIP, so "we don't know" has
// to be representable.
struct GuestFaultInfo final {
    std::optional<std::uint32_t> x86_vector{};
    std::optional<std::uint64_t> guest_rip{};
    std::optional<std::uint64_t> fault_address{};
    GuestAccessKind access{GuestAccessKind::Unknown};
    // Host-side truth. These are what the signal handler actually observed.
    std::optional<int> host_signal{};
    std::optional<int> host_si_code{};
    std::optional<std::uint64_t> host_pc{};
    bool recoverable{false};
};

struct StepInfo final {
    // Guest RIP before the stepped instruction.
    std::uint64_t start_rip{};
    // Exactly one architectural instruction retired, or the instruction
    // faulted. A step that could not run is refused before execution (T07),
    // so this is never used to mean "ran a block".
    bool instruction_retired{false};
    std::uint32_t instruction_length{};
};

struct RunResult final {
    StopReason primary_reason{StopReason::Returned};
    StopReasonBits pending_reasons{StopReasonBits::None};
    std::uint64_t thread_id{};
    std::uint64_t thread_generation{};
    std::uint64_t invocation_id{};
    std::uint64_t stop_epoch{};
    // Present only where the boundary makes a PC trustworthy.
    std::optional<std::uint64_t> guest_pc{};
    CpuSnapshot snapshot{};
    std::optional<GuestFaultInfo> fault{};
    std::optional<StepInfo> step{};
};

enum class InterruptReason : std::uint8_t {
    Pause = 0,
    Cancel,
    Shutdown,
};

[[nodiscard]] std::string_view ToString(InterruptReason reason) noexcept;

// Epoch-carrying interrupt request. §5 requires that a later request is not
// erased by an earlier Resume, so the epoch is part of the ticket rather than
// a global "halt" flag that Run could clear unconditionally.
struct InterruptTicket final {
    std::uint64_t thread_id{};
    std::uint64_t thread_generation{};
    std::uint64_t epoch{};
    InterruptReason reason{InterruptReason::Pause};
};

// Proof that the owner left the JIT, published its state, and will not write
// guest state again until resumed.
struct StopReceipt final {
    std::uint64_t thread_id{};
    std::uint64_t stop_epoch{};
    StopReason reason{StopReason::PauseRequested};
    CpuSnapshot snapshot{};
};

struct RunOptions final {
    // Consume an already-acknowledged interrupt epoch before running. Requests
    // newer than this stay pending.
    std::uint64_t resume_after_epoch{};
    // 0 == run until a natural boundary.
    std::uint64_t deadline_ns{};
};

// V0 implements exactly one architectural instruction per Step. Categories
// outside StepScope are refused *before* execution, leaving guest state
// untouched (T07).
enum class StepScope : std::uint32_t {
    None = 0,
    IntegerArithmetic = 1u << 0,
    Branch = 1u << 1,          // taken, not-taken, self-jump
    LoadStore = 1u << 2,
    Sse2 = 1u << 3,
};

[[nodiscard]] constexpr StepScope operator|(StepScope a, StepScope b) noexcept {
    return static_cast<StepScope>(static_cast<std::uint32_t>(a) | static_cast<std::uint32_t>(b));
}

struct StepOptions final {
    std::uint64_t resume_after_epoch{};
};

} // namespace Core::GuestCpu
