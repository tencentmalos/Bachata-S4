// SPDX-FileCopyrightText: Copyright 2026 shadPS4 Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

// HN2 Stage 0: prove REAL PS4 eboot (SELF) code translates and executes on the FEX
// guest backend, with NO Orbis-VMM/HLE integration. It uses the repo's Elf loader
// (correct SELF segment/decryption resolution), maps each PT_LOAD into a
// GuestAddressSpace at reservation_base + p_vaddr, applies R_X86_64_RELATIVE
// relocations (so the PIE's absolute pointers point at the load base), lays out a
// minimal EntryParams, then Runs the guest from e_entry. Real title bytes executing
// (a guest fault whose RIP is inside the module span, or a clean HLE boundary) is
// positive proof — not the synthetic decrement loop. Stage 4 wires the production
// Linker/Orbis path; this is a bring-up harness.

#include <algorithm>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <vector>

#include "core/guest_cpu/api/address_space.h"
#include "core/guest_cpu/api/context.h"
#include "core/guest_cpu/api/execution.h"
#include "core/guest_cpu/api/memory.h"
#include "core/guest_cpu/api/registers.h"
#include "core/guest_cpu/api/result.h"
#include "core/loader/elf.h"

using namespace Core::GuestCpu;
using Core::Loader::Elf;

namespace {

const char* StopReasonName(StopReason r) {
    switch (r) {
    case StopReason::Returned: return "Returned";
    case StopReason::PauseRequested: return "PauseRequested";
    case StopReason::Cancelled: return "Cancelled";
    case StopReason::StepComplete: return "StepComplete";
    case StopReason::HleBoundary: return "HleBoundary";
    case StopReason::GuestFault: return "GuestFault";
    case StopReason::Unsupported: return "Unsupported";
    case StopReason::BackendFailure: return "BackendFailure";
    default: return "?";
    }
}

std::uint64_t AlignDown(std::uint64_t v, std::uint64_t a) { return v & ~(a - 1); }
std::uint64_t AlignUp(std::uint64_t v, std::uint64_t a) { return (v + a - 1) & ~(a - 1); }

} // namespace

int main(int argc, char** argv) {
    std::setvbuf(stdout, nullptr, _IONBF, 0);
    if (argc != 2) {
        std::fprintf(stderr, "usage: %s <eboot.bin (decrypted SELF/ELF)>\n", argv[0]);
        return 2;
    }

    Elf elf;
    elf.Open(argv[1]);
    if (!elf.IsSelfFile() && !elf.IsElfFile()) {
        std::fprintf(stderr, "not a SELF/ELF: %s\n", argv[1]);
        return 1;
    }
    const auto eh = elf.GetElfHeader();
    const auto phdrs = elf.GetProgramHeader();
    std::printf("elf: type=%#x entry=%#llx phnum=%zu self=%d\n", eh.e_type,
                (unsigned long long)eh.e_entry, phdrs.size(), elf.IsSelfFile());

    std::uint64_t lo = ~0ull, hi = 0;
    for (const auto& ph : phdrs) {
        if (ph.p_type != PT_LOAD || ph.p_memsz == 0) continue;
        lo = std::min<std::uint64_t>(lo, ph.p_vaddr);
        hi = std::max<std::uint64_t>(hi, ph.p_vaddr + ph.p_memsz);
    }
    if (lo == ~0ull) {
        std::fprintf(stderr, "no PT_LOAD segments\n");
        return 1;
    }
    constexpr std::uint64_t kPage = 0x1000;
    lo = AlignDown(lo, kPage);
    hi = AlignUp(hi, kPage);
    const std::uint64_t module_span = hi - lo;
    std::printf("module span: [%#llx, %#llx) size=%#llx\n", (unsigned long long)lo,
                (unsigned long long)hi, (unsigned long long)module_span);

    const auto caps = QueryBackendCapabilities();
    std::printf("backend: max_guest_address=%#llx\n", (unsigned long long)caps.max_guest_address);

    constexpr std::uint64_t kStackSize = 0x200000;
    AddressSpaceConfig cfg{};
    cfg.reservation_size = AlignUp(module_span + kStackSize + 0x100000, kPage);
    auto space_r = GuestAddressSpace::Create(cfg);
    if (!space_r) {
        std::fprintf(stderr, "GuestAddressSpace::Create failed\n");
        return 1;
    }
    auto space = std::move(space_r.Value());
    const std::uint64_t rbase = space->ReservationBase().value;
    std::printf("reservation base=%#llx size=%#llx\n", (unsigned long long)rbase,
                (unsigned long long)cfg.reservation_size);

    auto ctx_r = CreateContext(CpuConfig{}, *space);
    if (!ctx_r) {
        std::fprintf(stderr, "CreateContext failed\n");
        return 1;
    }
    auto ctx = std::move(ctx_r.Value());

    // Map each PT_LOAD RW, load bytes via the Elf loader (correct SELF resolution),
    // then Protect. Also map any gap between consecutive segments (.bss / alignment
    // holes the crt touches) as RW so a real access there does not wild-fault.
    std::uint64_t prev_hi = 0;
    for (const auto& ph : phdrs) {
        if (ph.p_type != PT_LOAD || ph.p_memsz == 0) continue;
        const std::uint64_t gv = rbase + (ph.p_vaddr - lo);
        const std::uint64_t map_lo = AlignDown(gv, kPage);
        const std::uint64_t map_hi = AlignUp(gv + ph.p_memsz, kPage);
        // Fill the hole before this segment (relative to the previous segment's end).
        if (prev_hi != 0 && map_lo > prev_hi) {
            if (auto m = space->Map(GuestRange{GuestAddress{prev_hi}, map_lo - prev_hi},
                                    GuestPermission::Read | GuestPermission::Write);
                !m) {
                std::fprintf(stderr, "gap Map failed at %#llx\n", (unsigned long long)prev_hi);
                return 1;
            }
        }
        if (auto m = space->Map(GuestRange{GuestAddress{map_lo}, map_hi - map_lo},
                                GuestPermission::Read | GuestPermission::Write);
            !m) {
            std::fprintf(stderr, "Map failed at %#llx\n", (unsigned long long)map_lo);
            return 1;
        }
        prev_hi = map_hi;
        if (ph.p_filesz > 0) {
            std::vector<std::byte> buf(ph.p_filesz);
            elf.LoadSegment(reinterpret_cast<std::uint64_t>(buf.data()), ph.p_offset, ph.p_filesz);
            if (auto w = space->Write(GuestAddress{gv}, {buf.data(), buf.size()}); !w) {
                std::fprintf(stderr, "Write failed at %#llx\n", (unsigned long long)gv);
                return 1;
            }
        }
        const bool x = (ph.p_flags & PF_EXEC) != 0;
        const bool w = (ph.p_flags & PF_WRITE) != 0;
        GuestPermission perm = GuestPermission::Read;
        if (x) perm = perm | GuestPermission::Execute;
        if (w && !x) perm = perm | GuestPermission::Write;
        if (auto p = space->Protect(GuestRange{GuestAddress{map_lo}, map_hi - map_lo}, perm); !p) {
            std::fprintf(stderr, "Protect failed at %#llx\n", (unsigned long long)map_lo);
            return 1;
        }
        std::printf("  seg vaddr=%#llx -> guest=%#llx filesz=%#llx memsz=%#llx %c%c%c\n",
                    (unsigned long long)ph.p_vaddr, (unsigned long long)gv,
                    (unsigned long long)ph.p_filesz, (unsigned long long)ph.p_memsz,
                    (ph.p_flags & PF_READ) ? 'r' : '-', w ? 'w' : '-', x ? 'x' : '-');
    }

    // Apply R_X86_64_RELATIVE relocations. The Elf loader resolves PT_DYNAMIC and
    // PT_SCE_DYNLIBDATA; DT_SCE_RELA/RELASZ point into the dynlibdata blob. Load
    // both blobs via LoadSegment, then rebase (B+A - lo == reservation offset).
    {
        const elf_program_header* dyn = nullptr;
        const elf_program_header* dld = nullptr;
        for (const auto& ph : phdrs) {
            if (ph.p_type == PT_DYNAMIC) dyn = &ph;
            else if (ph.p_type == PT_SCE_DYNLIBDATA) dld = &ph;
        }
        int applied = 0;
        std::uint64_t rela_off = 0, rela_sz = 0, rela_ent = sizeof(elf_relocation);
        if (dyn != nullptr && dld != nullptr && dyn->p_filesz > 0 && dld->p_filesz > 0) {
            std::vector<std::byte> dynbuf(dyn->p_filesz), dldbuf(dld->p_filesz);
            elf.LoadSegment(reinterpret_cast<std::uint64_t>(dynbuf.data()), dyn->p_offset,
                            dyn->p_filesz);
            elf.LoadSegment(reinterpret_cast<std::uint64_t>(dldbuf.data()), dld->p_offset,
                            dld->p_filesz);
            const auto* d = reinterpret_cast<const elf_dynamic*>(dynbuf.data());
            const std::uint64_t dcount = dyn->p_filesz / sizeof(elf_dynamic);
            for (std::uint64_t i = 0; i < dcount; ++i) {
                if (d[i].d_tag == DT_NULL) break;
                if (d[i].d_tag == DT_SCE_RELA) rela_off = d[i].d_un.d_ptr;
                else if (d[i].d_tag == DT_SCE_RELASZ) rela_sz = d[i].d_un.d_val;
                else if (d[i].d_tag == DT_SCE_RELAENT) rela_ent = d[i].d_un.d_val;
            }
            if (rela_sz != 0 && rela_off + rela_sz <= dldbuf.size()) {
                for (std::uint64_t o = 0; o + sizeof(elf_relocation) <= rela_sz;
                     o += rela_ent) {
                    elf_relocation rl;
                    std::memcpy(&rl, dldbuf.data() + rela_off + o, sizeof(rl));
                    if (rl.GetType() != R_X86_64_RELATIVE) continue;
                    const std::uint64_t target = rbase + (rl.rel_offset - lo);
                    const std::uint64_t value =
                        rbase + static_cast<std::uint64_t>(rl.rel_addend) - lo;
                    std::byte vb[8];
                    std::memcpy(vb, &value, 8);
                    if (auto w = space->Write(GuestAddress{target}, {vb, 8}); w) ++applied;
                }
            }
        }
        std::printf("relocations: RELATIVE applied=%d (rela_off=%#llx sz=%#llx)\n", applied,
                    (unsigned long long)rela_off, (unsigned long long)rela_sz);
    }

    const std::uint64_t stack_hi = AlignDown(rbase + cfg.reservation_size - kPage, kPage);
    const std::uint64_t stack_lo = stack_hi - kStackSize;
    if (auto m = space->Map(GuestRange{GuestAddress{stack_lo}, kStackSize},
                            GuestPermission::Read | GuestPermission::Write);
        !m) {
        std::fprintf(stderr, "stack Map failed\n");
        return 1;
    }

    // Minimal EntryParams for OpenOrbis _start: argc=0, argv[0]=NULL.
    struct GuestEntryParams {
        std::int32_t argc;
        std::uint32_t padding;
        std::uint64_t argv[33];
        std::uint64_t entry_addr;
    } ep{};
    const std::uint64_t ep_addr = AlignDown(stack_hi - sizeof(ep) - 64, 16);
    if (auto w = space->Write(GuestAddress{ep_addr},
                              {reinterpret_cast<const std::byte*>(&ep), sizeof(ep)});
        !w) {
        std::fprintf(stderr, "EntryParams Write failed\n");
        return 1;
    }
    const std::uint64_t rsp = (ep_addr - 16) & ~std::uint64_t{15};
    const std::uint64_t entry = rbase + (eh.e_entry - lo);
    std::printf("entry guest=%#llx rsp=%#llx entry_params=%#llx\n", (unsigned long long)entry,
                (unsigned long long)rsp, (unsigned long long)ep_addr);

    ThreadInit init{};
    init.entry_rip = GuestCodeAddress{entry};
    init.initial_rsp = GuestAddress{rsp};
    init.guest_tid = 1;
    init.initial_state.fields = RegisterValidity::Gpr;
    init.initial_state.gpr_mask = (std::uint16_t{1} << static_cast<int>(Gpr::Rdi)) |
                                  (std::uint16_t{1} << static_cast<int>(Gpr::Rsi));
    init.initial_state.values.Set(Gpr::Rdi, ep_addr);
    init.initial_state.values.Set(Gpr::Rsi, rbase);
    auto th_r = ctx->CreateThread(init);
    if (!th_r) {
        std::fprintf(stderr, "CreateThread failed\n");
        return 1;
    }

    std::printf("running guest _start...\n");
    auto run = ctx->Run(th_r.Value(), RunOptions{});
    if (!run) {
        std::fprintf(stderr, "Run returned error status\n");
        return 1;
    }
    const RunResult& r = run.Value();
    const std::uint64_t stop_rip = r.guest_pc.value_or(r.snapshot.registers.rip);
    const bool in_module = stop_rip >= rbase && stop_rip < rbase + module_span;
    std::printf("STOP reason=%s stop_rip=%#llx in_module=%d\n", StopReasonName(r.primary_reason),
                (unsigned long long)stop_rip, in_module);
    if (r.fault.has_value()) {
        std::printf("fault guest_rip=%#llx fault_addr=%#llx\n",
                    (unsigned long long)r.fault->guest_rip.value_or(0),
                    (unsigned long long)r.fault->fault_address.value_or(0));
    }
    const bool executed = in_module || (r.fault.has_value() && r.fault->guest_rip.has_value() &&
                                        *r.fault->guest_rip >= rbase &&
                                        *r.fault->guest_rip < rbase + module_span);
    std::printf("%s\n", executed ? "STAGE0_PASS: executed real eboot bytes"
                                 : "STAGE0_INCONCLUSIVE");
    return executed ? 0 : 3;
}
