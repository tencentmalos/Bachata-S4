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
// Guest code does NOT yet execute: the dispatcher enters and exits without
// translating a block on the first Run. See docs/fex-guest-execution-bringup.md
// for the measurements and the narrowed-down cause. Do not treat a Returned stop
// from this backend as proof that guest instructions ran until that is fixed.

#pragma once

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
// One arm = one Run of one thread. The controller must see the previous generation leave the gate
// before arming the next. No arm means WaitAtEntry returns immediately, so ordinary tests are
// unaffected. The gate is keyed by thread id, so arming one owner never parks a different thread.
class FexTestRunGate {
public:
    virtual ~FexTestRunGate() = default;
    // Owner side. Blocks only while armed for `thread_id`; returns false on timeout (the caller then
    // fails the run rather than executing the guest). Reports arrival and exit generations.
    virtual bool WaitAtEntry(std::uint64_t context_id, std::uint64_t thread_id,
                             std::uint64_t invocation, std::uint64_t timeout_ms) = 0;
    // Controller side: arm the next Run of one thread. Fails if the previous generation has not yet
    // exited the gate (controller must observe that first).
    virtual bool Arm(std::uint64_t thread_id) = 0;
    // Release the armed generation and block (bounded) until the owner has actually left the gate, so
    // a re-arm can never be coalesced with the previous hold.
    virtual bool Release(std::uint64_t timeout_ms) = 0;
    // True once the owner has reached the gate for the armed generation.
    virtual bool Arrived() = 0;
};

void* FexTestRunGatePointer(CpuContext& context);
#endif

} // namespace Core::GuestCpu::Fex
