// SPDX-FileCopyrightText: Copyright 2026 shadPS4 Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#include <algorithm>
#include <array>
#include <cstring>
#include <new>

#include "core/guest_cpu/hle/veneer_allocator.h"

namespace Core::GuestCpu::Hle {

Result<HleVeneerAllocator> HleVeneerAllocator::Create(GuestAddressSpace& space, GuestRange slab) {
    if (slab.size < kVeneerSize) {
        return MakeError(ErrorCategory::InvalidArgument, "HleVeneerAllocator::Create",
                         "veneer slab is smaller than a single veneer");
    }
    // Map the whole slab read/write; Seal() publishes the whole mapping after
    // padding unused slots with reserved-operation traps. Map validates host-page alignment and
    // reservation containment, so a bad slab is refused here rather than at first Allocate.
    if (auto mapped = space.Map(slab, GuestPermission::Read | GuestPermission::Write); !mapped) {
        auto error = mapped.GetError();
        error.operation = "HleVeneerAllocator::Create";
        return error;
    }
    return HleVeneerAllocator{space, slab};
}

HleVeneerAllocator::HleVeneerAllocator(GuestAddressSpace& space, GuestRange slab)
    : space_{&space}, slab_{slab} {}

HleVeneerAllocator::HleVeneerAllocator(HleVeneerAllocator&& other) noexcept {
    std::lock_guard guard{other.mutex_};
    space_ = other.space_;
    slab_ = other.slab_;
    next_offset_ = other.next_offset_;
    sealed_ = other.sealed_;
    veneers_ = std::move(other.veneers_);
    other.space_ = nullptr;
}

HleVeneerAllocator& HleVeneerAllocator::operator=(HleVeneerAllocator&& other) noexcept {
    if (this == &other) {
        return *this;
    }
    std::scoped_lock guard{mutex_, other.mutex_};
    space_ = other.space_;
    slab_ = other.slab_;
    next_offset_ = other.next_offset_;
    sealed_ = other.sealed_;
    veneers_ = std::move(other.veneers_);
    other.space_ = nullptr;
    return *this;
}

Result<GuestAddress> HleVeneerAllocator::Allocate(std::uint64_t operation) {
    std::lock_guard guard{mutex_};
    if (space_ == nullptr) {
        return MakeError(ErrorCategory::WrongState, "HleVeneerAllocator::Allocate",
                         "allocator has no address space");
    }
    if (operation == 0) {
        return MakeError(ErrorCategory::InvalidArgument, "HleVeneerAllocator::Allocate",
                         "operation zero is unregistered");
    }
    if (sealed_) {
        return MakeError(ErrorCategory::WrongState, "HleVeneerAllocator::Allocate",
                         "allocator is sealed; no new veneers after execution begins");
    }
    if (auto it = veneers_.find(operation); it != veneers_.end()) {
        return GuestAddress{it->second};
    }
    if (slab_.size - next_offset_ < kVeneerSize) {
        return MakeError(ErrorCategory::OutOfMemory, "HleVeneerAllocator::Allocate",
                         "veneer slab exhausted");
    }

    // Assemble the 16-byte stub with the operation baked as the movabs immediate.
    std::array<std::byte, kVeneerSize> stub{};
    auto* p = reinterpret_cast<std::uint8_t*>(stub.data());
    // mov r10, rcx
    p[0] = 0x49;
    p[1] = 0x89;
    p[2] = 0xca;
    // movabs rax, imm64
    p[3] = 0x48;
    p[4] = 0xb8;
    std::memcpy(p + 5, &operation, sizeof(operation));
    // syscall ; ret
    p[13] = 0x0f;
    p[14] = 0x05;
    p[15] = 0xc3;

    const GuestAddress va{slab_.base.value + next_offset_};
    // Allocate metadata first: an allocation failure cannot consume a slot or
    // leave bytes reachable through an operation the registry never recorded.
    try {
        veneers_.emplace(operation, va.value);
    } catch (const std::bad_alloc&) {
        return MakeError(ErrorCategory::OutOfMemory, "HleVeneerAllocator::Allocate",
                         "operation metadata allocation failed");
    }
    if (auto written = space_->Write(va, {stub.data(), stub.size()}); !written) {
        veneers_.erase(operation);
        auto error = written.GetError();
        error.operation = "HleVeneerAllocator::Allocate";
        return error;
    }
    next_offset_ += kVeneerSize;
    return va;
}

Status HleVeneerAllocator::Seal() {
    std::lock_guard guard{mutex_};
    if (space_ == nullptr) {
        return MakeError(ErrorCategory::WrongState, "HleVeneerAllocator::Seal",
                         "allocator has no address space");
    }
    if (sealed_) {
        return Ok();
    }
    if (next_offset_ == 0) {
        // Nothing to make executable; sealing an empty allocator is a no-op that
        // still forbids later allocation, so the state is consistent.
        sealed_ = true;
        return Ok();
    }
    // GuestAddressSpace does not split mappings. Publish the WHOLE slab RX,
    // with unused aligned slots invoking reserved operation zero, never bytes
    // that decode as stores. Busy before publication permits retry; no Allocate
    // is allowed after successful publication. A syscall/protection failure
    // retains GuestAddressSpace's poison/recovery contract. This is initial-load publication,
    // not a replacement for the live-code quiescence transaction.
    std::array<std::byte, 4096> padding{};
    for (std::size_t i = 0; i < padding.size(); i += kVeneerSize) {
        // mov r10,rcx; movabs rax,0; syscall; ret. Operation zero is never
        // registered, so the production per-owner NON-spill exit catches it.
        // Raw UD2 would go through an unimplemented guest SIGILL path.
        padding[i] = std::byte{0x49};
        padding[i + 1] = std::byte{0x89};
        padding[i + 2] = std::byte{0xca};
        padding[i + 3] = std::byte{0x48};
        padding[i + 4] = std::byte{0xb8};
        padding[i + 13] = std::byte{0x0f};
        padding[i + 14] = std::byte{0x05};
        padding[i + 15] = std::byte{0xc3};
    }
    for (auto offset = next_offset_; offset < slab_.size;) {
        const auto bytes = std::min<std::uint64_t>(padding.size(), slab_.size - offset);
        if (auto written = space_->Write(GuestAddress{slab_.base.value + offset},
                                         {padding.data(), static_cast<std::size_t>(bytes)});
            !written)
            return written.GetError();
        offset += bytes;
    }
    if (auto protect =
            space_->Protect(slab_, GuestPermission::Read | GuestPermission::Execute);
        !protect) {
        auto error = protect.GetError();
        error.operation = "HleVeneerAllocator::Seal";
        return error;
    }
    sealed_ = true;
    return Ok();
}

GuestRange HleVeneerAllocator::PopulatedRange() const {
    std::lock_guard guard{mutex_};
    return GuestRange{slab_.base, next_offset_};
}

std::size_t HleVeneerAllocator::Count() const {
    std::lock_guard guard{mutex_};
    return veneers_.size();
}

bool HleVeneerAllocator::IsSealed() const {
    std::lock_guard guard{mutex_};
    return sealed_;
}

} // namespace Core::GuestCpu::Hle
