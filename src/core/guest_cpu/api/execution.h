// SPDX-FileCopyrightText: Copyright 2026 shadPS4 Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

// Run / Step / interrupt vocabulary.
//
// Spec §6 is explicit that a normal return, a pause, a cancel, a guest fault, a
// finished single step and an HLE entry must not all arrive as "HLT". Hence a
// primary reason plus a bitset of everything else that was pending, so a
// simultaneous fault and pause cannot hide one another.

#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <string_view>

#include "core/guest_cpu/api/registers.h"
#include "core/guest_cpu/api/status.h"

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

    // Set when this fault is an attributed guest syscall HLE error (R2-H05). The values identify the
    // exact failing crossing: the guest operation number, the owning context/thread/generation/
    // invocation, and the backend error category/errno. Presence of `operation` marks this as a
    // syscall-fault (vs an instruction/HLT fault); these stay unset for the latter.
    std::optional<std::uint64_t> syscall_operation{};
    std::uint64_t context_id{};
    std::uint64_t thread_generation{};
    std::uint64_t invocation_id{};
    ErrorCategory syscall_category{ErrorCategory::None};
    std::optional<int> syscall_errno{};
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

// A re-entrant guest function call (InvokeGuest). The PS4 boot chain calls into
// guest code from the host: DT_INIT / module init, `_malloc_init`,
// `sceLibcInternalMemoryMutexEnable`, and the program entry itself are guest
// addresses, not native function pointers. On x86 the desktop core casts the
// guest VA to a host pointer and calls it; that is invalid under FEX (the guest
// cannot be entered by a native call), so those sites route through InvokeGuest.
//
// Integer arguments follow the SysV order rdi, rsi, rdx, rcx, r8, r9; anything
// past six spills to the guest stack. The call runs on an existing owned guest
// thread so its stack, FS/TLS and guest heap persist across the boot sequence
// (DT_INIT for one module must see the heap another init already touched). The
// backend pushes its return gate as the call's return address and detects the
// return by the thread landing on that gate; a fault mid-call is reported as a
// fault, never a zero return value.
struct GuestCallArgs final {
    // At most six register arguments plus any stack spill. Bounded so a bad
    // caller cannot ask the backend to write an unbounded amount onto the guest
    // stack; the boot sites use zero to three.
    static constexpr std::size_t kMaxArguments = 8;
    std::array<std::uint64_t, kMaxArguments> values{};
    std::size_t count{};
};

struct GuestCallResult final {
    // rax after the guest function returned. Only meaningful when
    // reason == StopReason::Returned.
    std::uint64_t return_value{};
    StopReason reason{StopReason::Returned};
    // Populated when the call did not return cleanly (fault, unresolved import,
    // cancellation). The caller propagates this instead of trusting return_value.
    std::optional<GuestFaultInfo> fault{};
    // The snapshot at the stop, same authoritativeness rules as RunResult.
    CpuSnapshot snapshot{};
};

// Options for InvokeGuest. The FS base carries the guest TLS pointer (Orbis TCB)
// the call must run under; zero leaves the owned thread's current FS base in
// place, which is correct for a call on a thread already set up for TLS.
struct GuestCallOptions final {
    std::optional<std::uint64_t> fs_base{};
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
//
// `context_id` binds the ticket to the context that issued it. Without it a
// ticket from a destroyed context could be presented to its replacement and
// match on thread id and epoch alone, since both restart their numbering
// (Round 2 spec 3.1: ticket/receipt bind context identity, thread generation
// and request epoch).
struct InterruptTicket final {
    std::uint64_t context_id{};
    std::uint64_t thread_id{};
    std::uint64_t thread_generation{};
    std::uint64_t epoch{};
    InterruptReason reason{InterruptReason::Pause};

    [[nodiscard]] bool IsValid() const noexcept {
        return context_id != 0 && epoch != 0;
    }
};

// Proof that the owner left the JIT, published its state, and will not write
// guest state again until resumed.
//
// `request_epoch` is the ticket this receipt answers; `stop_epoch` is the stop
// it produced. They are different numbers on purpose: "the request was seen" and
// "the thread is stopped at a published safe point" are different facts, and
// conflating them is how a controller ends up reading a snapshot that the owner
// had not finished writing.
struct StopReceipt final {
    std::uint64_t context_id{};
    std::uint64_t thread_id{};
    std::uint64_t request_epoch{};
    std::uint64_t stop_epoch{};
    StopReason reason{StopReason::PauseRequested};
    // Which requests are still outstanding. A Pause that arrives while a Cancel
    // is pending does not erase the Cancel; §3.1 fixes the precedence as
    // Fault/BackendFailure > Cancel > Pause.
    std::uint32_t pending_reasons{};
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
