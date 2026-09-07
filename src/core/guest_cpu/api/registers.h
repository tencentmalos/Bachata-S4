// SPDX-FileCopyrightText: Copyright 2026 shadPS4 Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

// Public x86-64 register view.
//
// Two rules from API contract §3 drive this file:
//   1. GPR indices are a public enum. FEX's internal array order is not part
//      of this API, so a consumer never indexes by a backend constant.
//   2. Every snapshot carries a validity mask. A field that was not captured
//      reads as zero AND reports invalid; an all-zero array is never allowed
//      to pass for "captured, happened to be zero".

#pragma once

#include <array>
#include <cstdint>
#include <string_view>

namespace Core::GuestCpu {

// Public, stable numbering. Chosen to match the x86-64 architectural encoding
// so that a reader cross-checking a disassembly does not have to translate.
enum class Gpr : std::uint8_t {
    Rax = 0, Rcx = 1, Rdx = 2, Rbx = 3,
    Rsp = 4, Rbp = 5, Rsi = 6, Rdi = 7,
    R8 = 8, R9 = 9, R10 = 10, R11 = 11,
    R12 = 12, R13 = 13, R14 = 14, R15 = 15,
};

inline constexpr std::size_t kGprCount = 16;
inline constexpr std::size_t kXmmCount = 16;

[[nodiscard]] std::string_view ToString(Gpr reg) noexcept;

[[nodiscard]] constexpr std::size_t Index(Gpr reg) noexcept {
    return static_cast<std::size_t>(reg);
}

// 128-bit SSE register. V0 exposes XMM only; see RegisterValidity::Ymm for the
// AVX rule.
struct alignas(16) Xmm final {
    std::uint64_t low{};
    std::uint64_t high{};

    [[nodiscard]] friend bool operator==(const Xmm&, const Xmm&) = default;
};

// Which fields of a RegisterFile were actually captured. Bit set == field is
// meaningful. Anything not set must be treated as unknown, not as zero.
enum class RegisterValidity : std::uint32_t {
    None = 0,
    Gpr = 1u << 0,
    Rip = 1u << 1,
    Rflags = 1u << 2,
    Xmm = 1u << 3,
    Mxcsr = 1u << 4,
    SegmentBases = 1u << 5,  // FSBase / GSBase
    // Set only when an AVX-capable profile is negotiated AND the high halves
    // were genuinely captured. V0's SSE2 profile never sets this.
    Ymm = 1u << 6,
    // x87 / MMX are not validated in V0. The bit exists so that a future
    // capture can claim it; V0 must leave it clear.
    X87 = 1u << 7,
};

[[nodiscard]] constexpr RegisterValidity operator|(RegisterValidity a, RegisterValidity b) noexcept {
    return static_cast<RegisterValidity>(static_cast<std::uint32_t>(a) |
                                         static_cast<std::uint32_t>(b));
}
constexpr RegisterValidity& operator|=(RegisterValidity& a, RegisterValidity b) noexcept {
    a = a | b;
    return a;
}
[[nodiscard]] constexpr bool HasAll(RegisterValidity mask, RegisterValidity wanted) noexcept {
    return (static_cast<std::uint32_t>(mask) & static_cast<std::uint32_t>(wanted)) ==
           static_cast<std::uint32_t>(wanted);
}

// RSP has exactly one authoritative slot: gpr[Index(Gpr::Rsp)]. There is no
// second "stack pointer" member, so the two can never disagree.
struct RegisterFile final {
    std::array<std::uint64_t, kGprCount> gpr{};
    std::uint64_t rip{};
    std::uint64_t rflags{};
    std::array<Xmm, kXmmCount> xmm{};
    std::uint32_t mxcsr{};
    std::uint64_t fs_base{};
    std::uint64_t gs_base{};
    RegisterValidity validity{RegisterValidity::None};

    [[nodiscard]] std::uint64_t Get(Gpr reg) const noexcept {
        return gpr[Index(reg)];
    }
    void Set(Gpr reg, std::uint64_t value) noexcept {
        gpr[Index(reg)] = value;
    }
    [[nodiscard]] std::uint64_t Rsp() const noexcept {
        return Get(Gpr::Rsp);
    }
};

// Which fields a WriteRegisters call intends to change. Fields outside the
// mask are left alone, so a caller cannot accidentally zero a register it
// never read.
struct RegisterPatch final {
    RegisterFile values{};
    RegisterValidity fields{RegisterValidity::None};
    // Individual GPR selection within RegisterValidity::Gpr. Bit N == Gpr N.
    std::uint16_t gpr_mask{};
    // Individual XMM selection within RegisterValidity::Xmm. Bit N == XMM N.
    std::uint16_t xmm_mask{};
};

// How a snapshot was obtained. This is the difference between "the guest is
// parked at a boundary we defined" and "we interrupted the JIT and are
// reporting what we could recover" (spec §6, D03).
enum class SnapshotKind : std::uint8_t {
    // Guest stopped at a defined boundary; register values are authoritative.
    SafePoint = 0,
    // Boundary of an in-progress HLE call. Read-only.
    HleBoundary,
    // Last published safe point; the thread has since resumed or is in JIT.
    // Values describe the past, not the present.
    LastSafePoint,
    // A fault was taken. Validity mask says which fields survived.
    Faulted,
    // Asynchronous stop while inside JIT. Guest registers may live in host
    // registers and are NOT reconstructed. Most validity bits will be clear.
    // Never present this as precise guest state.
    AsyncJitStop,
};

[[nodiscard]] std::string_view ToString(SnapshotKind kind) noexcept;

struct CpuSnapshot final {
    static constexpr std::uint32_t kSchemaVersion = 1;

    std::uint32_t schema_version{kSchemaVersion};
    std::uint64_t thread_id{};
    std::uint64_t thread_generation{};
    std::uint64_t stop_epoch{};
    std::uint64_t invocation_id{};
    SnapshotKind kind{SnapshotKind::SafePoint};
    RegisterFile registers{};
    // Generations let a debugger notice that code/mapping changed underneath a
    // snapshot it is still displaying.
    std::uint64_t mapping_generation{};
    std::uint64_t code_generation{};
    // Real host PC when known (async stops, faults). 0 == not captured.
    std::uint64_t host_pc{};
};

} // namespace Core::GuestCpu
