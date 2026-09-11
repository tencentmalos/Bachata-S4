// SPDX-FileCopyrightText: Copyright 2026 shadPS4 Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

// The real ISessionBackend, backed by guest_cpu_fex.
//
// This is the FEX half of the HN0.1 split: SessionCore owns lifecycle/threading/
// generation, and this class owns the guest_cpu resources (address space, context,
// thread) and the actual x86-64 smoke routine. It links guest_cpu_fex; the
// lifecycle core does not, so the core stays host-unit-testable with a fake.
//
// The smoke routine is still the bounded decrement loop the CPU-alive proof needs
// (HN0.1.7). It is NOT a real PS4 game: the Android host is not yet native (that
// is HN1/HN2). What changed from the old fex_session_jni.cpp is that this body no
// longer owns any lifecycle state -- no global session, no owner thread, no
// stop/join. It exposes Prepare/Run/RequestCancel/WaitStopped/Destroy and lets
// SessionCore sequence them safely.

#include <array>
#include <cstdint>
#include <mutex>
#include <unordered_map>
#include <vector>

#include "core/guest_cpu/api/address_space.h"
#include "core/guest_cpu/api/context.h"
#include "core/guest_cpu/api/execution.h"
#include "core/guest_cpu/api/memory.h"
#include "core/guest_cpu/api/registers.h"
#include "core/guest_cpu/api/result.h"
#include "core/guest_cpu/api/status.h"
#include "core/host_runtime/session_backend_fex.h"

namespace Core::HostRuntime {

using namespace Core::GuestCpu;

namespace {

constexpr std::uint64_t kReservationSize = std::uint64_t{1} << 28;
constexpr std::uint64_t kMappingSize = 0x4000;
constexpr std::uint64_t kCodeOffset = 0x10000;
constexpr std::uint64_t kStackOffset = 0x20000;

// Bounded decrement loop; rdi = iteration count. Ends by jumping to the backend
// return gate (StopReason::Returned). Large-but-finite so a Cancel can interrupt
// it while a natural return is still reachable.
std::vector<std::uint8_t> BuildLoopRoutine(std::uint64_t gate_address) {
    std::vector<std::uint8_t> code;
    auto emit = [&](std::initializer_list<std::uint8_t> bytes) {
        for (auto b : bytes) code.push_back(b);
    };
    emit({0x48, 0x83, 0xef, 0x01});  // sub rdi, 1
    emit({0x75, 0xfa});              // jne -6
    emit({0x49, 0xbf});              // movabs r15, imm64
    for (int i = 0; i < 8; ++i)
        code.push_back(static_cast<std::uint8_t>((gate_address >> (8 * i)) & 0xff));
    emit({0x41, 0xff, 0xe7});        // jmp r15
    return code;
}

// The FEX-backed per-session runtime: the guest_cpu resources SessionCore holds
// as an opaque shared_ptr<SessionRuntime>.
struct FexSessionRuntime final : SessionRuntime {
    std::unique_ptr<GuestAddressSpace> space;
    std::unique_ptr<CpuContext> context;
    ThreadHandle thread{};

    // RequestCancel stores the ticket keyed by a monotonic StopTicket value, so
    // concurrent stoppers do not clobber each other's ticket. WaitStopped looks
    // it up. Guarded by its own mutex; independent of SessionCore's lock.
    std::mutex ticket_mtx;
    std::uint64_t next_ticket{1};
    std::unordered_map<std::uint64_t, InterruptTicket> tickets;
};

}  // namespace

Result<std::shared_ptr<SessionRuntime>> FexSessionBackend::Prepare(const SessionParams& params) {
    auto rt = std::make_shared<FexSessionRuntime>();

    AddressSpaceConfig cfg{};
    cfg.reservation_size = kReservationSize;
    cfg.max_address = QueryBackendCapabilities().max_guest_address;
    auto space_r = GuestAddressSpace::Create(cfg);
    if (!space_r)
        return space_r.GetError();
    rt->space = std::move(space_r).Value();

    const std::uint64_t base = rt->space->ReservationBase().value;
    const std::uint64_t code_base = base + kCodeOffset;
    const std::uint64_t stack_top = base + kStackOffset + kMappingSize - 16;

    if (auto m = rt->space->Map(GuestRange{GuestAddress{code_base}, kMappingSize},
                                GuestPermission::Read | GuestPermission::Write);
        !m)
        return m.GetError();
    if (auto m = rt->space->Map(GuestRange{GuestAddress{stack_top - kMappingSize + 16}, kMappingSize},
                                GuestPermission::Read | GuestPermission::Write);
        !m)
        return m.GetError();

    auto ctx_r = CreateContext(CpuConfig{}, *rt->space);
    if (!ctx_r)
        return ctx_r.GetError();
    rt->context = std::move(ctx_r).Value();

    const std::uint64_t gate = rt->context->Capabilities().return_gate_address;
    if (gate == 0)
        return MakeError(ErrorCategory::BackendFailure, "FexSessionBackend::Prepare",
                         "backend reported no return gate");

    auto code = BuildLoopRoutine(gate);
    if (auto w = rt->space->Write(GuestAddress{code_base},
                                  {reinterpret_cast<const std::byte*>(code.data()), code.size()});
        !w)
        return w.GetError();
    if (auto p = rt->space->Protect(GuestRange{GuestAddress{code_base}, kMappingSize},
                                    GuestPermission::Read | GuestPermission::Execute);
        !p)
        return p.GetError();

    const std::uint64_t iters = params.iterations != 0 ? params.iterations : (std::uint64_t{1} << 32);
    ThreadInit init{};
    init.entry_rip = GuestCodeAddress{code_base};
    init.initial_rsp = GuestAddress{stack_top};
    init.guest_tid = 1;
    init.initial_state.fields = RegisterValidity::Gpr;
    init.initial_state.gpr_mask = (1u << Index(Gpr::Rdi));
    init.initial_state.values.Set(Gpr::Rdi, iters);

    auto th_r = rt->context->CreateThread(init);
    if (!th_r)
        return th_r.GetError();
    rt->thread = th_r.Value();

    return std::shared_ptr<SessionRuntime>(std::move(rt));
}

RunReport FexSessionBackend::Run(SessionRuntime& runtime) {
    auto& rt = static_cast<FexSessionRuntime&>(runtime);
    RunReport report;
    auto run = rt.context->Run(rt.thread, RunOptions{});
    if (!run) {
        report.outcome = RunOutcome::BackendFailed;
        report.error_category = static_cast<std::uint32_t>(run.GetError().category);
        report.detail = Describe(run.GetError());
        return report;
    }
    const RunResult& r = run.Value();
    switch (r.primary_reason) {
    case StopReason::Returned:
        report.outcome = RunOutcome::Returned;
        break;
    case StopReason::Cancelled:
        report.outcome = RunOutcome::Cancelled;
        break;
    case StopReason::GuestFault: {
        report.outcome = RunOutcome::Faulted;
        std::string detail = "guest fault";
        if (r.fault && r.fault->guest_rip)
            detail += " guest_rip=0x" + [](std::uint64_t v) {
                static const char* h = "0123456789abcdef";
                std::string s;
                for (int i = 60; i >= 0; i -= 4) s += h[(v >> i) & 0xf];
                return s;
            }(*r.fault->guest_rip);
        report.detail = std::move(detail);
        break;
    }
    case StopReason::BackendFailure:
        report.outcome = RunOutcome::BackendFailed;
        report.detail = "backend failure";
        break;
    case StopReason::Unsupported:
        report.outcome = RunOutcome::Unsupported;
        report.detail = "unsupported";
        break;
    case StopReason::PauseRequested:
    case StopReason::StepComplete:
    case StopReason::HleBoundary:
    default:
        report.outcome = RunOutcome::Unexpected;
        report.detail = "unexpected stop reason for smoke session";
        break;
    }
    return report;
}

Result<StopTicket> FexSessionBackend::RequestCancel(SessionRuntime& runtime) {
    auto& rt = static_cast<FexSessionRuntime&>(runtime);
    auto ticket = rt.context->RequestInterrupt(rt.thread, InterruptReason::Cancel);
    if (!ticket)
        return ticket.GetError();
    std::lock_guard lock{rt.ticket_mtx};
    const std::uint64_t id = rt.next_ticket++;
    rt.tickets.emplace(id, ticket.Value());
    return StopTicket{id, true};
}

Status FexSessionBackend::WaitStopped(SessionRuntime& runtime, const StopTicket& ticket,
                                      std::uint64_t timeout_ns) {
    auto& rt = static_cast<FexSessionRuntime&>(runtime);
    InterruptTicket it{};
    {
        std::lock_guard lock{rt.ticket_mtx};
        auto found = rt.tickets.find(ticket.value);
        if (found == rt.tickets.end())
            return MakeError(ErrorCategory::InvalidArgument, "FexSessionBackend::WaitStopped",
                             "unknown stop ticket");
        it = found->second;
    }
    auto receipt = rt.context->WaitStopped(it, timeout_ns);
    if (!receipt)
        return receipt.GetError();
    return Ok();
}

void FexSessionBackend::Destroy(SessionRuntime& runtime) {
    auto& rt = static_cast<FexSessionRuntime&>(runtime);
    if (rt.context && rt.thread.IsValid())
        (void)rt.context->DestroyThread(rt.thread);
    rt.context.reset();
    rt.space.reset();
    rt.thread = ThreadHandle{};
}

}  // namespace Core::HostRuntime
