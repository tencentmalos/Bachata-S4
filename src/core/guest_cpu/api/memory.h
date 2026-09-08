// SPDX-FileCopyrightText: Copyright 2026 shadPS4 Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

// Guest address space and memory transactions.
//
// The single most important thing in this header is that there are three
// distinct page-size quantities and they do not share a symbol (decision
// DEC-04). Collapsing them is exactly the mistake the V0 spec forbids:
//
//   HostPageSize()        runtime, from sysconf. 16384 or 4096. Never a
//                         compile-time constant, because acceptance B04 runs
//                         one APK on both.
//   kGuestAbiPageSize     4096 always. x86-64 PS4 ABI mapping granularity.
//   kCodeIndexGranularity 4096 always. Internal decode-index granularity,
//                         which the backend's generated code pins at >>12.

#pragma once

#include <cstddef>
#include <cstdint>
#include <memory>
#include <span>
#include <string_view>

#include "core/guest_cpu/api/result.h"

namespace Core::GuestCpu {

// x86-64 guest ABI mapping granularity. Independent of the host.
inline constexpr std::uint64_t kGuestAbiPageSize = 4096;

// Internal decode/lookup index granularity. Stays 4 KiB on every host: the
// backend's dispatcher emits a fixed shift-by-12, so this is not ours to
// change (see page-size-audit.md §3).
inline constexpr std::uint64_t kCodeIndexGranularity = 4096;

// Real host page size, discovered at runtime. Cached after the first call.
[[nodiscard]] std::uint64_t HostPageSize();

[[nodiscard]] inline bool IsHostPageAligned(std::uint64_t value) {
    return (value & (HostPageSize() - 1)) == 0;
}
[[nodiscard]] inline std::uint64_t AlignDownToHostPage(std::uint64_t value) {
    return value & ~(HostPageSize() - 1);
}
// Returns 0 on overflow rather than wrapping, so a caller cannot turn a huge
// length into a small one (acceptance M04).
[[nodiscard]] std::uint64_t AlignUpToHostPage(std::uint64_t value);

// Strong guest address types. A guest code address and a guest data address
// are not interchangeable (API contract §7.1), so they are distinct types
// rather than two typedefs for uint64_t.
struct GuestAddress final {
    std::uint64_t value{};
    [[nodiscard]] friend auto operator<=>(const GuestAddress&, const GuestAddress&) = default;
};

struct GuestCodeAddress final {
    std::uint64_t value{};
    [[nodiscard]] friend auto operator<=>(const GuestCodeAddress&, const GuestCodeAddress&) = default;
};

struct GuestRange final {
    GuestAddress base{};
    std::uint64_t size{};

    [[nodiscard]] std::uint64_t End() const noexcept {
        return base.value + size;  // Callers must have validated via Checked().
    }
    // Rejects zero length and any base+size that would overflow.
    [[nodiscard]] static Result<GuestRange> Checked(GuestAddress base, std::uint64_t size);
};

enum class GuestPermission : std::uint32_t {
    None = 0,
    Read = 1u << 0,
    Write = 1u << 1,
    Execute = 1u << 2,
};

[[nodiscard]] constexpr GuestPermission operator|(GuestPermission a, GuestPermission b) noexcept {
    return static_cast<GuestPermission>(static_cast<std::uint32_t>(a) |
                                        static_cast<std::uint32_t>(b));
}
[[nodiscard]] constexpr bool HasPermission(GuestPermission set, GuestPermission wanted) noexcept {
    return (static_cast<std::uint32_t>(set) & static_cast<std::uint32_t>(wanted)) ==
           static_cast<std::uint32_t>(wanted);
}

// Why a host page currently carries a protection. Several reasons can apply at
// once; the effective host protection is the *intersection* of what they all
// allow. Spec §5.1: one observer's temporary write must never single-handedly
// relax the real protection.
enum class ProtectionReason : std::uint32_t {
    GuestPermission = 1u << 0,
    GuardPage = 1u << 1,
    CodeWriteTracking = 1u << 2,
    GpuDirtyTracking = 1u << 3,
};

[[nodiscard]] constexpr ProtectionReason operator|(ProtectionReason a, ProtectionReason b) noexcept {
    return static_cast<ProtectionReason>(static_cast<std::uint32_t>(a) |
                                         static_cast<std::uint32_t>(b));
}

// V0 supports exactly one memory mode. A caller asking for software callbacks
// or MMIO is refused at init rather than silently given something weaker
// (API contract §7.1).
enum class MemoryMode : std::uint8_t {
    DirectMapped = 0,
    SoftwareCallbacks,  // Not implemented. Requesting it => UnsupportedMemoryMode.
};

// How self-modifying guest code becomes visible. V0 implements
// ExplicitPublication only, for the reason recorded in DEC-05.
enum class SmcMode : std::uint8_t {
    ExplicitPublication = 0,
    TransparentSmc,  // Not implemented. Requesting it => Unsupported.
};

[[nodiscard]] std::string_view ToString(SmcMode mode) noexcept;

enum class InvalidationReason : std::uint8_t {
    HostWrite = 0,      // Host/HLE rewrote guest code.
    GuestPublish,       // Guest wrote code and called PublishGuestCode.
    Unmap,
    AliasUpdate,
    FullFlush,
};

// Shared liveness marker for handles that outlive their address space.
//
// A QuiescenceToken or PinnedSpan is supposed to be released before the space
// it came from is destroyed, but "supposed to" is not a memory-safety
// guarantee: an orphaned handle would otherwise call back into freed memory
// during its own destructor. Both hold a weak reference to this block, so a
// late release becomes a no-op instead of undefined behaviour.
struct AddressSpaceLiveness final {
    class GuestAddressSpace* space{};
};

// Proof that every relevant guest owner is stopped and no HLE writer is still
// running. Required for any transaction that changes code or mappings
// (API contract §7.2). Move-only: a token must not be duplicated and used to
// authorise two overlapping transactions.
class QuiescenceToken final {
public:
    QuiescenceToken() = default;
    QuiescenceToken(const QuiescenceToken&) = delete;
    QuiescenceToken& operator=(const QuiescenceToken&) = delete;
    QuiescenceToken(QuiescenceToken&&) noexcept;
    QuiescenceToken& operator=(QuiescenceToken&&) noexcept;
    ~QuiescenceToken();

    [[nodiscard]] bool IsValid() const noexcept {
        return epoch != 0;
    }
    [[nodiscard]] std::uint64_t Epoch() const noexcept {
        return epoch;
    }
    [[nodiscard]] std::size_t StoppedThreadCount() const noexcept {
        return stopped_threads;
    }
    // False once the originating address space has been destroyed. A token in
    // that state can no longer authorise anything.
    [[nodiscard]] bool OwnerAlive() const noexcept {
        return !owner.expired();
    }
    // True when this token came from `space` and that space is still alive.
    //
    // A backend must check this, not just IsValid(): a token from a different
    // address space is structurally valid and would otherwise be accepted as
    // authorisation to discard translations for a space it says nothing about.
    [[nodiscard]] bool IsFrom(const GuestAddressSpace* space) const noexcept {
        const auto live = owner.lock();
        return live != nullptr && space != nullptr && live->space == space;
    }

private:
    friend class GuestAddressSpace;
    QuiescenceToken(std::weak_ptr<AddressSpaceLiveness> owner, std::uint64_t epoch,
                    std::size_t threads);
    void ReleaseIfOwned() noexcept;

    std::weak_ptr<AddressSpaceLiveness> owner{};
    std::uint64_t epoch{};
    std::size_t stopped_threads{};
};

// A borrowed host view of guest memory, handed to HLE. While alive, the
// underlying mapping cannot be unmapped or reprotected out from under it
// (acceptance M13). Move-only so the lease has exactly one holder.
class PinnedSpan final {
public:
    PinnedSpan() = default;
    PinnedSpan(const PinnedSpan&) = delete;
    PinnedSpan& operator=(const PinnedSpan&) = delete;
    PinnedSpan(PinnedSpan&&) noexcept;
    PinnedSpan& operator=(PinnedSpan&&) noexcept;
    ~PinnedSpan();

    [[nodiscard]] bool IsValid() const noexcept {
        return host_data != nullptr;
    }

    // Read view. Always available for a live lease.
    //
    // Named Bytes() and returning const so the ordinary call is the safe one: the previous
    // signature handed back a mutable span regardless of the lease kind, so a read-only pin could
    // be written with no cast and no diagnostic, which is how it bypassed the writer admission
    // rule (2026-09-08 publication review, P1-A).
    [[nodiscard]] std::span<const std::byte> Bytes() const noexcept {
        return {host_data, host_size};
    }

    // Write view. Empty unless this lease was acquired writable, so a caller that ignores the
    // result writes nothing instead of writing through a read-only lease. Check IsValid() on the
    // lease and emptiness here rather than assuming success.
    [[nodiscard]] std::span<std::byte> WritableBytes() const noexcept {
        if (!writable) {
            return {};
        }
        return {host_data, host_size};
    }

    [[nodiscard]] GuestAddress Base() const noexcept {
        return guest_base;
    }
    [[nodiscard]] bool Writable() const noexcept {
        return writable;
    }
    // False once the originating address space has been destroyed. The bytes
    // are not safe to touch in that state.
    [[nodiscard]] bool OwnerAlive() const noexcept {
        return !owner.expired();
    }
    void Release();

private:
    friend class GuestAddressSpace;
    PinnedSpan(std::weak_ptr<AddressSpaceLiveness> owner, GuestAddress base, std::byte* data,
               std::size_t size, bool writable, std::uint64_t lease_id);

    std::weak_ptr<AddressSpaceLiveness> owner{};
    GuestAddress guest_base{};
    std::byte* host_data{};
    std::size_t host_size{};
    bool writable{false};
    std::uint64_t lease_id{};
};

struct MappingInfo final {
    GuestRange range{};
    GuestPermission permission{GuestPermission::None};
    ProtectionReason reasons{ProtectionReason::GuestPermission};
    std::uint64_t mapping_generation{};
    bool host_owned{false};
};

} // namespace Core::GuestCpu
