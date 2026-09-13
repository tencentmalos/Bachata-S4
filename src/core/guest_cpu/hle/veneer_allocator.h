// SPDX-FileCopyrightText: Copyright 2026 shadPS4 Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

// HLE veneer allocator.
//
// An HLE import cannot be resolved to a host function pointer written into the
// guest GOT: under FEX the guest is translated x86-64 and cannot `call` a native
// ARM64 address. Instead each HLE operation gets a tiny x86 stub -- a "veneer" --
// whose address goes into the GOT. When the guest calls through the GOT it lands
// in the veneer, which loads the operation number into rax and executes a
// `syscall`; FEX traps that to the backend's syscall handler, which dispatches
// the operation to the typed HLE registry and resumes the guest at the veneer's
// `ret`.
//
// The 16-byte stub, per operation:
//     49 89 ca              mov  r10, rcx    ; syscall clobbers rcx; SysV puts the
//                                            ; 4th integer arg there, so stage it
//                                            ; in r10 (HandleSyscall reads r10 back
//                                            ; into the decode-view rcx)
//     48 b8 <imm64>         movabs rax, op   ; operation number the handler dispatches
//     0f 05                 syscall
//     c3                    ret              ; returns to the caller's return address
//
// DELIBERATE DIVERGENCE from references/shadps4-arm64, which mmaps a bare host
// page per veneer OUTSIDE the guest VMM: those pages can land above the FEX
// guest-address policy limit (1<<36) where the JIT's block lookup would alias
// them. This allocator publishes the veneers INSIDE the owned GuestAddressSpace
// reservation (a slab the caller carves below the limit), so a veneer VA is a
// real guest-executable address the JIT resolves like any other guest code.

#pragma once

#include <cstddef>
#include <cstdint>
#include <mutex>
#include <unordered_map>

#include "core/guest_cpu/api/address_space.h"
#include "core/guest_cpu/api/memory.h"
#include "core/guest_cpu/api/result.h"

namespace Core::GuestCpu::Hle {

class HleVeneerAllocator final {
public:
    // Each veneer is exactly this many bytes; slots are packed 16-byte aligned.
    static constexpr std::size_t kVeneerSize = 16;

    // `slab` is a range inside `space`'s reservation, below the guest-address
    // policy limit, reserved for veneers. The allocator maps it read/write on
    // construction; Seal() pads unused slots with operation-zero traps and flips the whole slab to
    // read/execute before the guest first runs. `space` owns the mapping and
    // must outlive the allocator; destroying/move-assigning an allocator does
    // not unmap published code. The loader tears down the address space.
    [[nodiscard]] static Result<HleVeneerAllocator> Create(GuestAddressSpace& space,
                                                           GuestRange slab);

    HleVeneerAllocator(const HleVeneerAllocator&) = delete;
    HleVeneerAllocator& operator=(const HleVeneerAllocator&) = delete;
    HleVeneerAllocator(HleVeneerAllocator&&) noexcept;
    HleVeneerAllocator& operator=(HleVeneerAllocator&&) noexcept;
    ~HleVeneerAllocator() = default;

    // Returns the guest VA of the veneer for `operation`, emitting it on first
    // request and returning the cached VA thereafter. Fails when the slab is
    // exhausted or a write fails; a bad operation is never silently mapped to a
    // reused slot. Must not be called after Seal() (the slab is read/execute).
    [[nodiscard]] Result<GuestAddress> Allocate(std::uint64_t operation);

    // Flips the whole slab from read/write to read/execute (no mapping split). Call
    // once, after every veneer is allocated and before the first guest Run. After
    // this, Allocate() is refused. An empty slab remains non-executable.
    [[nodiscard]] Status Seal();

    // The range actually populated with veneers, so the backend/loader can report
    // it as guest-executable. Empty (size 0) until at least one Allocate().
    [[nodiscard]] GuestRange PopulatedRange() const;

    [[nodiscard]] std::size_t Count() const;
    [[nodiscard]] bool IsSealed() const;

private:
    HleVeneerAllocator(GuestAddressSpace& space, GuestRange slab);

    mutable std::mutex mutex_;
    GuestAddressSpace* space_{};
    GuestRange slab_{};
    std::uint64_t next_offset_{};
    bool sealed_{false};
    std::unordered_map<std::uint64_t, std::uint64_t> veneers_;  // operation -> guest VA
};

} // namespace Core::GuestCpu::Hle
