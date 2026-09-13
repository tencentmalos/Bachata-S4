// SPDX-FileCopyrightText: Copyright 2026 shadPS4 Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

// FEXCore backend for the guest CPU API.
//
// Everything FEX-specific lives behind this header's .cpp. The public API
// (api/context.h) names no FEX type, so a consumer still compiles with -I src
// alone; only the final link needs FEX.
//
// STATE: context creation, thread lifecycle, register transfer and the handle/
// ownership contracts work and are covered by tests/guest_cpu/guest_execution_tests.
// Real x86-64 fixtures execute through this backend. The initial no-translation
// failure in docs/fex-guest-execution-bringup.md is historical. A Returned stop
// alone still does not prove execution: tests must assert architectural effects.
// Production loader/HLE/callback integration is a separate acceptance boundary.

#pragma once

#include <cstdint>

#include "core/guest_cpu/api/context.h"

namespace Core::GuestCpu::Fex {

// Same contract as CreateContext, which forwards here when this backend is the
// one linked in. Exposed separately so a test can select it explicitly rather
// than depending on link order.
[[nodiscard]] Result<std::unique_ptr<CpuContext>> CreateFexContext(const CpuConfig& config,
                                                                   GuestAddressSpace& space);

[[nodiscard]] BackendCapabilities QueryFexCapabilities();

// Install a typed native HLE function on a FEX context and return the guest operation number. The
// guest places that number in rax before the syscall gate; arguments follow the SysV callgate (r10
// holding the 4th integer). Typed by the adapter layer so this header stays free of FEX types.
void* FexHleRegistryPointer(CpuContext& context);

#if defined(GUEST_CPU_TEST_HOOKS)
// Test-only deterministic Run-entry delay (G24). Compiled ONLY into test builds; release products
// never contain this wait.
//
// An owner thread calls WaitAtEntry() from inside Run(), AFTER Run has set the thread running and
// acquired its execution lease but released every coordinator/context/address-space lock, and
// BEFORE entering FEX ExecuteThread(). At that instant it holds no lock a coordinator needs and is
// not inside a signal handler, so pausing here lets QuiesceContext issue its requests, refuse
// admission and drain another, genuinely-JIT-running owner while this one is provably not going to
// reach a stop -- replacing the old arbitrary-PC SIGUSR1 freeze that could land while a FEX lock was
// held. On release the owner enters the JIT and immediately services the already-pending Pause/Cancel
// at the block-entry fault page, exactly as a normally-running owner would.
//
// One arm = one hold of one thread's Run. Each Arm returns a generation token; every later
// controller call names that token, so a stale/wrong/double release cannot touch a different
// generation and one owner's exit cannot confirm two releases. States: Idle -> Armed -> Arrived ->
// Releasing -> Exited (clean) or TimedOut (owner did not leave in budget; the gate stays closed).
// The controller must observe Exited for the previous token before Arm accepts the next. No arm
// means WaitAtEntry returns immediately, so ordinary tests are unaffected. Keyed by thread id and
// checked against context/thread/invocation at arrival, so an arm for one owner never parks another.
class FexTestRunGate {
public:
    virtual ~FexTestRunGate() = default;

    // Controller side: arm the next hold of `thread_id`. Returns a nonzero generation token, or 0
    // if a previous generation is still live (controller must wait for its Exit first).
    virtual std::uint64_t Arm(std::uint64_t thread_id, std::uint64_t context_id) = 0;

    // True once the armed owner has reached the gate for THIS token (matching thread/context; the
    // invocation it bound is reported via BoundInvocation for identity checks).
    virtual bool Arrived(std::uint64_t token) = 0;

    // Release THIS token's owner and block (bounded) until that exact owner has left the gate.
    // Idempotent for the same already-released token (returns the recorded result); a stale or
    // unknown token is rejected (false) without touching the current generation.
    virtual bool Release(std::uint64_t token, std::uint64_t timeout_ms) = 0;

    // True once THIS token has fully Exited (owner returned from WaitAtEntry, clean or timed-out).
    virtual bool Exited(std::uint64_t token) = 0;

    // The Run invocation the arrived owner bound to this token (0 if not arrived / no such token).
    virtual std::uint64_t BoundInvocation(std::uint64_t token) = 0;

    // Owner side. Called from Run at the lock-free entry point. If an armed token matches this
    // thread/context, block until released or timeout; returns true on release, false on timeout (the
    // caller fails the run). Reports arrival and exit for exactly its own token.
    virtual bool WaitAtEntry(std::uint64_t context_id, std::uint64_t thread_id,
                             std::uint64_t invocation, std::uint64_t timeout_ms) = 0;
};

void* FexTestRunGatePointer(CpuContext& context);
// Test-only hold between release observation and continuation admission.
void* FexTestContinuationGatePointer(CpuContext& context);
std::uint64_t FexTestContinuationRetries(CpuContext& context);

// N3 probe syscall-point diagnostic facts (populated when trace is enabled; test builds only).
struct FexSyscallTraceRecord {
    std::uint64_t shim_sp{};
    std::uint64_t shim_x28{};
    std::uint64_t shim_lr{};
    std::uint64_t returning_stack{};
    std::uint64_t in_syscall{};
    std::uint64_t guest_rip{};
    std::uint64_t guest_rcx{};
    std::uint64_t guest_r11{};
    std::uint64_t callret_sp{};
    std::uint64_t frame_addr{};
    int invocations{};
};
void FexTestSetSyscallTrace(CpuContext& context, bool trace);
const FexSyscallTraceRecord* FexTestSyscallTrace();
#endif

} // namespace Core::GuestCpu::Fex
