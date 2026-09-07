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

private:
    friend class GuestAddressSpace;
    QuiescenceToken(class GuestAddressSpace* owner, std::uint64_t epoch, std::size_t threads);

    class GuestAddressSpace* owner{};
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
    [[nodiscard]] std::span<std::byte> Bytes() const noexcept {
        return {host_data, host_size};
    }
    [[nodiscard]] GuestAddress Base() const noexcept {
        return guest_base;
    }
    [[nodiscard]] bool Writable() const noexcept {
        return writable;
    }
    void Release();

private:
    friend class GuestAddressSpace;
    PinnedSpan(class GuestAddressSpace* owner, GuestAddress base, std::byte* data,
               std::size_t size, bool writable, std::uint64_t lease_id);

    class GuestAddressSpace* owner{};
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
