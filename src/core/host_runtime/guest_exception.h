// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once
#include <array>
#include <cstring>
#include "core/guest_cpu/api/registers.h"
#include "core/libraries/kernel/threads/exception.h"

namespace Core::HostRuntime {
// Orbis ucontext ABI prefix; excludes every host-only field in Ucontext.
struct GuestExceptionContext {
    std::array<u8, 0x40> signal_mask_and_reserved{};
    Libraries::Kernel::Mcontext machine{};
    std::array<u8, 0x40> tail{};
};
static_assert(offsetof(GuestExceptionContext, machine) ==
              offsetof(Libraries::Kernel::Ucontext, uc_mcontext));
static_assert(sizeof(GuestExceptionContext) <= 0x600);
// PS4Util's arch_user_handler reads the interrupted stack from ucontext + 0xf8.
static_assert(offsetof(GuestExceptionContext, machine) +
                  offsetof(Libraries::Kernel::Mcontext, mc_rsp) == 0xf8);

inline GuestExceptionContext MakeGuestExceptionContext(const GuestCpu::RegisterFile& r) {
    using GuestCpu::Gpr;
    GuestExceptionContext out{};
    auto& m = out.machine;
#define REG(name, reg) m.mc_##name = r.Get(Gpr::reg)
    REG(rdi, Rdi); REG(rsi, Rsi); REG(rdx, Rdx); REG(rcx, Rcx);
    REG(r8, R8); REG(r9, R9); REG(rax, Rax); REG(rbx, Rbx); REG(rbp, Rbp);
    REG(r10, R10); REG(r11, R11); REG(r12, R12); REG(r13, R13);
    REG(r14, R14); REG(r15, R15); REG(rsp, Rsp);
#undef REG
    m.mc_rip = r.rip;
    m.mc_rflags = r.rflags;
    m.mc_fsbase = r.fs_base;
    m.mc_gsbase = r.gs_base;
    m.mc_len = sizeof(m);
    m.mc_fpformat = 0x10002; // _MC_FPFMT_XMM
    m.mc_ownedfp = 0x20001; // _MC_FPOWNED_FPU
    auto* fx = reinterpret_cast<u8*>(m.mc_fpstate);
    std::memcpy(fx + 24, &r.mxcsr, sizeof(r.mxcsr));
    std::memcpy(fx + 160, r.xmm.data(), sizeof(r.xmm));
    return out;
}
} // namespace Core::HostRuntime
