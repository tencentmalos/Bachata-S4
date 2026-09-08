// SPDX-FileCopyrightText: Copyright 2026 shadPS4 Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#include "core/guest_cpu/api/address_space.h"

#include <algorithm>
#include <cerrno>
#include <cstring>
#include <limits>

#include <sys/mman.h>
#include <unistd.h>

namespace Core::GuestCpu {
namespace {

std::uint64_t DiscoverHostPageSize() {
    const long value = ::sysconf(_SC_PAGESIZE);
    // A non-positive or non-power-of-two answer means we cannot reason about
    // alignment at all. Falling back to a guess would be worse than failing
    // loudly later, but we must return something; use the smallest sane value
    // so that alignment checks stay conservative.
    if (value <= 0) {
        return 4096;
    }
    const auto page = static_cast<std::uint64_t>(value);
    if ((page & (page - 1)) != 0) {
        return 4096;
    }
    return page;
}

int ToHostProtection(GuestPermission permission) {
    int prot = PROT_NONE;
    if (HasPermission(permission, GuestPermission::Read)) {
        prot |= PROT_READ;
    }
    if (HasPermission(permission, GuestPermission::Write)) {
        prot |= PROT_WRITE;
    }
    if (HasPermission(permission, GuestPermission::Execute)) {
        prot |= PROT_EXEC;
    }
    return prot;
}

bool RangesOverlap(const GuestRange& a, const GuestRange& b) {
    return a.base.value < b.base.value + b.size && b.base.value < a.base.value + a.size;
}

// mmap failures are not all "out of memory". A W^X host refuses RWX with
// EACCES, and reporting that as OutOfMemory would send a reader looking for a
// memory leak instead of a permission policy.
ErrorCategory CategoriseMapFailure(int error) {
    switch (error) {
    case EACCES:
    case EPERM:
        return ErrorCategory::PermissionDenied;
    case EINVAL:
        return ErrorCategory::InvalidArgument;
    case ENOMEM:
    case EAGAIN:
        return ErrorCategory::OutOfMemory;
    default:
        return ErrorCategory::BackendFailure;
    }
}

} // namespace

std::uint64_t HostPageSize() {
    static const std::uint64_t cached = DiscoverHostPageSize();
    return cached;
}

std::uint64_t AlignUpToHostPage(std::uint64_t value) {
    const std::uint64_t page = HostPageSize();
    if (value > std::numeric_limits<std::uint64_t>::max() - (page - 1)) {
        return 0;  // Overflow: report failure rather than wrapping to a small value.
    }
    return (value + page - 1) & ~(page - 1);
}

std::string_view ToString(SmcMode mode) noexcept {
    switch (mode) {
    case SmcMode::ExplicitPublication: return "ExplicitPublication";
    case SmcMode::TransparentSmc: return "TransparentSMC";
    }
    return "Unknown";
}

Result<GuestRange> GuestRange::Checked(GuestAddress base, std::uint64_t size) {
    if (size == 0) {
        return MakeError(ErrorCategory::InvalidArgument, "GuestRange::Checked",
                         "zero-length range");
    }
    if (base.value > std::numeric_limits<std::uint64_t>::max() - size) {
        return MakeError(ErrorCategory::InvalidArgument, "GuestRange::Checked",
                         "base + size overflows");
    }
    return GuestRange{base, size};
}

// --- QuiescenceToken --------------------------------------------------------

QuiescenceToken::QuiescenceToken(std::weak_ptr<AddressSpaceLiveness> owner_,
                                 std::uint64_t epoch_, std::size_t threads)
    : owner{std::move(owner_)}, epoch{epoch_}, stopped_threads{threads} {}

QuiescenceToken::QuiescenceToken(QuiescenceToken&& other) noexcept
    : owner{std::move(other.owner)}, epoch{other.epoch},
      stopped_threads{other.stopped_threads} {
    other.owner.reset();
    other.epoch = 0;
    other.stopped_threads = 0;
}

QuiescenceToken& QuiescenceToken::operator=(QuiescenceToken&& other) noexcept {
    if (this != &other) {
        ReleaseIfOwned();
        owner = std::move(other.owner);
        epoch = other.epoch;
        stopped_threads = other.stopped_threads;
        other.owner.reset();
        other.epoch = 0;
        other.stopped_threads = 0;
    }
    return *this;
}

QuiescenceToken::~QuiescenceToken() {
    ReleaseIfOwned();
}

void QuiescenceToken::ReleaseIfOwned() noexcept {
    if (epoch == 0) {
        return;
    }
    // lock() fails when the address space is already gone. Releasing into a
    // destroyed space is then correctly a no-op rather than a call through a
    // dangling pointer.
    if (auto alive = owner.lock(); alive && alive->space != nullptr) {
        alive->space->ReleaseQuiescence(epoch);
    }
    owner.reset();
    epoch = 0;
}

// --- ExecutionLease ---------------------------------------------------------
//
// Held by the backend for the duration of one Run or Step. Unlike QuiescenceToken this stores a
// raw pointer rather than a weak reference: the backend cannot outlive the address space it was
// created against (CreateContext documents that `space` must outlive the context), and a lease
// never escapes a single Run call.

ExecutionLease::ExecutionLease(ExecutionLease&& other) noexcept : space{other.space} {
    other.space = nullptr;
}

ExecutionLease& ExecutionLease::operator=(ExecutionLease&& other) noexcept {
    if (this != &other) {
        ReleaseIfOwned();
        space = other.space;
        other.space = nullptr;
    }
    return *this;
}

ExecutionLease::~ExecutionLease() {
    ReleaseIfOwned();
}

void ExecutionLease::ReleaseIfOwned() noexcept {
    if (space == nullptr) {
        return;
    }
    space->ReleaseExecutionLease();
    space = nullptr;
}

// --- PinnedSpan -------------------------------------------------------------

PinnedSpan::PinnedSpan(std::weak_ptr<AddressSpaceLiveness> owner_, GuestAddress base,
                       std::byte* data, std::size_t size, bool writable_, std::uint64_t lease)
    : owner{std::move(owner_)}, guest_base{base}, host_data{data}, host_size{size},
      writable{writable_}, lease_id{lease} {}

PinnedSpan::PinnedSpan(PinnedSpan&& other) noexcept
    : owner{std::move(other.owner)}, guest_base{other.guest_base}, host_data{other.host_data},
      host_size{other.host_size}, writable{other.writable}, lease_id{other.lease_id} {
    other.owner.reset();
    other.host_data = nullptr;
    other.host_size = 0;
    other.lease_id = 0;
}

PinnedSpan& PinnedSpan::operator=(PinnedSpan&& other) noexcept {
    if (this != &other) {
        Release();
        owner = std::move(other.owner);
        guest_base = other.guest_base;
        host_data = other.host_data;
        host_size = other.host_size;
        writable = other.writable;
        lease_id = other.lease_id;
        other.owner.reset();
        other.host_data = nullptr;
        other.host_size = 0;
        other.lease_id = 0;
    }
    return *this;
}

PinnedSpan::~PinnedSpan() {
    Release();
}

void PinnedSpan::Release() {
    if (lease_id != 0) {
        // Same rule as QuiescenceToken: a lease that outlived its space is
        // dropped silently instead of writing through freed memory.
        if (auto alive = owner.lock(); alive && alive->space != nullptr) {
            alive->space->ReleasePin(lease_id);
        }
    }
    owner.reset();
    host_data = nullptr;
    host_size = 0;
    lease_id = 0;
}

// --- GuestAddressSpace ------------------------------------------------------

GuestAddressSpace::GuestAddressSpace(void* reservation, std::uint64_t size,
                                     const AddressSpaceConfig& config)
    : reservation_host{reservation},
      reservation_base{GuestAddress{reinterpret_cast<std::uint64_t>(reservation)}},
      reservation_size{size}, memory_mode{config.memory_mode}, smc_mode{config.smc_mode},
      liveness{std::make_shared<AddressSpaceLiveness>()} {
    liveness->space = this;
}

Result<std::unique_ptr<GuestAddressSpace>> GuestAddressSpace::Create(
    const AddressSpaceConfig& config) {
    // Refuse unimplemented modes up front. Downgrading silently would let a
    // caller believe it had MMIO or transparent SMC support (DEC-05).
    if (config.memory_mode != MemoryMode::DirectMapped) {
        return MakeError(ErrorCategory::UnsupportedMemoryMode, "GuestAddressSpace::Create",
                         "V0 implements DirectMapped only");
    }
    if (config.smc_mode != SmcMode::ExplicitPublication) {
        return MakeError(ErrorCategory::Unsupported, "GuestAddressSpace::Create",
                         "V0 implements ExplicitPublication SMC only; see DEC-05");
    }

    const std::uint64_t size = AlignUpToHostPage(config.reservation_size);
    if (size == 0) {
        return MakeError(ErrorCategory::InvalidArgument, "GuestAddressSpace::Create",
                         "reservation size is zero or overflows when aligned");
    }

    // PROT_NONE reservation. The kernel picks the address: we never scan
    // /proc/self/maps for a hole and then MAP_FIXED into it, because ART or a
    // driver can claim that hole in between (spec §5.1).
    //
    // When the caller caps the address, pass a hint and then check what we got.
    // A hint is advisory -- the kernel may ignore it -- so the check below is
    // what actually enforces the limit; the hint just makes success likely.
    void* hint = nullptr;
    if (config.max_address != 0) {
        if (size > config.max_address) {
            return MakeError(ErrorCategory::InvalidArgument, "GuestAddressSpace::Create",
                             "reservation is larger than the requested maximum address");
        }
        // Aim well below the cap so the mapping has room to fall back downward.
        hint = reinterpret_cast<void*>((config.max_address - size) / 2);
    }

    void* reservation = ::mmap(hint, static_cast<std::size_t>(size), PROT_NONE,
                               MAP_PRIVATE | MAP_ANONYMOUS | MAP_NORESERVE, -1, 0);
    if (reservation == MAP_FAILED) {
        const int saved = errno;
        auto error = MakeError(CategoriseMapFailure(saved), "GuestAddressSpace::Create",
                               "reservation mmap failed");
        error.system_error = saved;
        return error;
    }

    if (config.max_address != 0) {
        const auto base = reinterpret_cast<std::uint64_t>(reservation);
        if (base + size > config.max_address) {
            // Fail rather than proceed: a backend that cannot address this range
            // would mis-handle it silently -- FEXCore's block lookup, for one,
            // masks the guest RIP and would alias unrelated addresses together.
            ::munmap(reservation, static_cast<std::size_t>(size));
            return MakeError(ErrorCategory::OutOfMemory, "GuestAddressSpace::Create",
                             "the kernel placed the reservation above the requested maximum "
                             "address and no lower region was available");
        }
    }

    return std::unique_ptr<GuestAddressSpace>(
        new GuestAddressSpace(reservation, size, config));
}

GuestAddressSpace::~GuestAddressSpace() {
    // Publish "gone" before releasing memory, so any surviving token or span
    // sees an expired owner rather than a stale pointer.
    if (liveness) {
        liveness->space = nullptr;
    }
    if (reservation_host != nullptr) {
        ::munmap(reservation_host, static_cast<std::size_t>(reservation_size));
    }
}

std::uint64_t GuestAddressSpace::MappingGeneration() const {
    std::lock_guard guard{lock};
    return mapping_generation;
}

std::uint64_t GuestAddressSpace::CodeGeneration() const {
    std::lock_guard guard{lock};
    return code_generation;
}

std::byte* GuestAddressSpace::HostPointer(GuestAddress address) const {
    return reinterpret_cast<std::byte*>(address.value);
}

Result<MappingInfo> GuestAddressSpace::Map(GuestRange range, GuestPermission permission) {
    if (range.size == 0) {
        return MakeError(ErrorCategory::InvalidArgument, "GuestAddressSpace::Map",
                         "zero-length range");
    }
    if (range.base.value > std::numeric_limits<std::uint64_t>::max() - range.size) {
        return MakeError(ErrorCategory::InvalidArgument, "GuestAddressSpace::Map",
                         "base + size overflows");
    }
    // Host-page alignment, not guest-page alignment. Accepting a 4 KiB-aligned
    // request on a 16 KiB host and widening it silently would grant
    // permissions on memory the caller never asked about (spec §5.1).
    if (!IsHostPageAligned(range.base.value) || !IsHostPageAligned(range.size)) {
        return MakeError(ErrorCategory::InvalidArgument, "GuestAddressSpace::Map",
                         "range must be host-page aligned; host page is " +
                             std::to_string(HostPageSize()));
    }
    if (range.base.value < reservation_base.value ||
        range.base.value + range.size > reservation_base.value + reservation_size) {
        return MakeError(ErrorCategory::InvalidArgument, "GuestAddressSpace::Map",
                         "range is outside this address space's reservation");
    }

    std::lock_guard guard{lock};
    for (const auto& existing : mappings) {
        if (RangesOverlap(existing.range, range)) {
            return MakeError(ErrorCategory::InvalidArgument, "GuestAddressSpace::Map",
                             "range overlaps an existing mapping");
        }
    }

    // MAP_FIXED is safe here and only here: the target is inside a reservation
    // this object already owns, so nothing else can be holding it.
    void* result = ::mmap(HostPointer(range.base), static_cast<std::size_t>(range.size),
                          ToHostProtection(permission),
                          MAP_PRIVATE | MAP_ANONYMOUS | MAP_FIXED, -1, 0);
    if (result == MAP_FAILED) {
        const int saved = errno;
        auto error = MakeError(CategoriseMapFailure(saved), "GuestAddressSpace::Map",
                               "commit mmap failed");
        error.system_error = saved;
        return error;
    }

    ++mapping_generation;
    Mapping mapping{range, permission, ProtectionReason::GuestPermission, mapping_generation};
    mappings.push_back(mapping);

    MappingInfo info{};
    info.range = range;
    info.permission = permission;
    info.reasons = mapping.reasons;
    info.mapping_generation = mapping.generation;
    info.host_owned = true;
    return info;
}

Status GuestAddressSpace::Unmap(GuestRange range) {
    std::lock_guard guard{lock};

    // A live HLE lease wins: the span must stay valid for its whole lifetime
    // (acceptance M13). Report Busy rather than pulling it out underneath.
    for (const auto& pin : pins) {
        if (RangesOverlap(pin.range, range)) {
            return MakeError(ErrorCategory::Busy, "GuestAddressSpace::Unmap",
                             "range has a live pinned HLE span");
        }
    }

    const auto found = std::find_if(mappings.begin(), mappings.end(), [&](const Mapping& m) {
        return m.range.base == range.base && m.range.size == range.size;
    });
    if (found == mappings.end()) {
        return MakeError(ErrorCategory::InvalidArgument, "GuestAddressSpace::Unmap",
                         "no mapping exactly matches this range");
    }

    // Return the range to PROT_NONE while keeping the reservation: dropping it
    // with munmap would let an unrelated allocation land inside our space.
    void* result = ::mmap(HostPointer(range.base), static_cast<std::size_t>(range.size),
                          PROT_NONE, MAP_PRIVATE | MAP_ANONYMOUS | MAP_FIXED, -1, 0);
    if (result == MAP_FAILED) {
        auto error = MakeError(ErrorCategory::BackendFailure, "GuestAddressSpace::Unmap",
                               "failed to restore reservation protection");
        error.system_error = errno;
        return error;
    }

    mappings.erase(found);
    std::erase_if(aliases, [&](const Alias& alias) {
        return RangesOverlap(alias.primary, range);
    });
    ++mapping_generation;
    ++code_generation;
    return Ok();
}

Status GuestAddressSpace::Protect(GuestRange range, GuestPermission permission) {
    if (range.size == 0) {
        return MakeError(ErrorCategory::InvalidArgument, "GuestAddressSpace::Protect",
                         "zero-length range");
    }

    // The M03 case. On a 16 KiB host a 4 KiB sub-range cannot get its own
    // protection, so we refuse *before* touching anything: permissions,
    // contents and bookkeeping all stay exactly as they were (DEC-06).
    if (!IsHostPageAligned(range.base.value) || !IsHostPageAligned(range.size)) {
        return MakeError(
            ErrorCategory::Unsupported, "GuestAddressSpace::Protect",
            "DirectMapped cannot give sub-host-page ranges distinct permissions; host page is " +
                std::to_string(HostPageSize()) + ", nothing was modified");
    }

    std::lock_guard guard{lock};
    const auto found = std::find_if(mappings.begin(), mappings.end(), [&](const Mapping& m) {
        return m.range.base.value <= range.base.value &&
               range.base.value + range.size <= m.range.base.value + m.range.size;
    });
    if (found == mappings.end()) {
        return MakeError(ErrorCategory::InvalidArgument, "GuestAddressSpace::Protect",
                         "range is not inside a single mapping");
    }

    // Reject a partial range instead of recording the new permission for the whole mapping.
    //
    // The 2026-09-08 review (R3) reproduced the bug this replaces: mprotect was applied to the
    // sub-range while `found->permission` was overwritten for the entire mapping, so ValidateRange
    // then approved writes to pages the kernel still had read-only, and a handed-out writable span
    // faulted on first use. Splitting mappings would be the general answer; refusing is the
    // honest one until that exists, and it never leaves the table disagreeing with the kernel.
    if (range.base.value != found->range.base.value || range.size != found->range.size) {
        return MakeError(ErrorCategory::Unsupported, "GuestAddressSpace::Protect",
                         "this implementation cannot split a mapping; Protect must cover the whole "
                         "mapping, and nothing was modified");
    }

    // A live pin means someone holds a host pointer obtained under the current permission.
    // Changing it underneath them would invalidate a span they are still allowed to use, which
    // M13 forbids; the review recorded the missing check as R4.
    if (AnyPinOverlapsLocked(range)) {
        return MakeError(ErrorCategory::Busy, "GuestAddressSpace::Protect",
                         "a pinned span overlaps this range; release it before reprotecting");
    }

    // Re-granting execute over a poisoned range would undo the only protection a failed
    // publication has: the bytes changed, the backend still holds a translation of the old ones,
    // and revoking execute here is what keeps that translation unreachable. The review made a
    // failed publication executable again with exactly this call, and the guest re-ran the stale
    // constant (R1).
    if (code_poisoned && HasPermission(permission, GuestPermission::Execute) &&
        RangesOverlap(range, poisoned_range)) {
        return MakeError(ErrorCategory::WrongState, "GuestAddressSpace::Protect",
                         "code publication failed over this range and has not been repaired; "
                         "execute cannot be granted");
    }

    if (::mprotect(HostPointer(range.base), static_cast<std::size_t>(range.size),
                   ToHostProtection(permission)) != 0) {
        const int saved = errno;
        auto error = MakeError(CategoriseMapFailure(saved), "GuestAddressSpace::Protect",
                               "mprotect failed");
        error.system_error = saved;
        return error;
    }

    found->permission = permission;
    ++mapping_generation;
    return Ok();
}

Result<MappingInfo> GuestAddressSpace::Query(GuestAddress address) const {
    std::lock_guard guard{lock};
    for (const auto& mapping : mappings) {
        if (address.value >= mapping.range.base.value &&
            address.value < mapping.range.base.value + mapping.range.size) {
            MappingInfo info{};
            info.range = mapping.range;
            info.permission = mapping.permission;
            info.reasons = mapping.reasons;
            info.mapping_generation = mapping.generation;
            info.host_owned = true;
            return info;
        }
    }
    return MakeError(ErrorCategory::InvalidArgument, "GuestAddressSpace::Query",
                     "address is not mapped");
}

std::vector<MappingInfo> GuestAddressSpace::Mappings() const {
    std::lock_guard guard{lock};
    std::vector<MappingInfo> out;
    out.reserve(mappings.size());
    for (const auto& mapping : mappings) {
        MappingInfo info{};
        info.range = mapping.range;
        info.permission = mapping.permission;
        info.reasons = mapping.reasons;
        info.mapping_generation = mapping.generation;
        info.host_owned = true;
        out.push_back(info);
    }
    return out;
}

Status GuestAddressSpace::ValidateRange(GuestRange range, GuestPermission required) const {
    std::lock_guard guard{lock};
    return ValidateRangeLocked(range, required);
}

// Requires lock. Separated from ValidateRange so a caller that must validate and then act
// atomically -- AcquirePinnedSpan, Read, Write -- can hold one lock across both instead of
// revalidating against state that may already have changed.
Status GuestAddressSpace::ValidateRangeLocked(GuestRange range, GuestPermission required) const {
    if (range.size == 0) {
        return MakeError(ErrorCategory::InvalidArgument, "GuestAddressSpace::ValidateRange",
                         "zero-length range");
    }
    if (range.base.value > std::numeric_limits<std::uint64_t>::max() - range.size) {
        return MakeError(ErrorCategory::InvalidArgument, "GuestAddressSpace::ValidateRange",
                         "base + size overflows");
    }

    for (const auto& mapping : mappings) {
        if (range.base.value >= mapping.range.base.value &&
            range.base.value + range.size <= mapping.range.base.value + mapping.range.size) {
            if (!HasPermission(mapping.permission, required)) {
                return MakeError(ErrorCategory::PermissionDenied,
                                 "GuestAddressSpace::ValidateRange",
                                 "mapping lacks the requested permission");
            }
            return Ok();
        }
    }
    // Deliberately not merging adjacent mappings: a span crossing two separate
    // mappings is rejected, because their permissions can differ.
    return MakeError(ErrorCategory::InvalidArgument, "GuestAddressSpace::ValidateRange",
                     "range is not contained in a single mapping");
}

Status GuestAddressSpace::Read(GuestAddress from, std::span<std::byte> into) const {
    const auto range = GuestRange::Checked(from, into.size());
    if (!range) {
        return range.GetError();
    }
    // Copy under the same lock that validated the range: releasing it first would let an Unmap or
    // Protect land between the check and the memcpy (2026-09-08 review, R4).
    std::lock_guard guard{lock};
    if (auto status = ValidateRangeLocked(range.Value(), GuestPermission::Read); !status) {
        return status;
    }
    std::memcpy(into.data(), HostPointer(from), into.size());
    return Ok();
}

Status GuestAddressSpace::Write(GuestAddress to, std::span<const std::byte> from) {
    const auto range = GuestRange::Checked(to, from.size());
    if (!range) {
        return range.GetError();
    }
    {
        std::lock_guard guard{lock};

        // Same admission rule as AcquirePinnedSpan. Without it a transaction holding quiescence
        // could still be undercut through the explicit write path, which would make the token mean
        // "no pinned writers" rather than "no writers" -- and those are only the same if every
        // writer happens to go through a pin.
        //
        // The transaction owner is not blocked by this: it publishes through PublishCode, which
        // has its own epoch check and does not route here.
        if (active_quiescence != 0) {
            return MakeError(ErrorCategory::Busy, "GuestAddressSpace::Write",
                             "a quiescence transaction is in progress; publish through the token");
        }

        if (auto status = ValidateRangeLocked(range.Value(), GuestPermission::Write); !status) {
            return status;
        }
        std::memcpy(HostPointer(to), from.data(), from.size());
    }
    // Observers are notified with the lock released: a callback may re-enter the address space,
    // and holding the lock across it would deadlock.
    NotifyObservers(range.Value());
    return Ok();
}

Result<PinnedSpan> GuestAddressSpace::AcquirePinnedSpan(GuestRange range, bool writable) {
    const auto required = writable ? (GuestPermission::Read | GuestPermission::Write)
                                   : GuestPermission::Read;

    // Validation and lease registration happen under one lock hold. Splitting them -- validate,
    // unlock, relock, register -- let an Unmap or Protect land in between and hand back a span for
    // a range that had already changed. The review recorded that window as part of R4.
    std::lock_guard guard{lock};

    // A transaction holds quiescence precisely so it can change code or mappings without a writer
    // underneath it. Admitting a new writable pin during one would reopen the window the token is
    // supposed to have closed (2026-09-08 review, R5). Readers are still allowed: the transaction
    // owner needs them, and they cannot invalidate its assumptions.
    if (writable && active_quiescence != 0) {
        return MakeError(ErrorCategory::Busy, "GuestAddressSpace::AcquirePinnedSpan",
                         "a quiescence transaction is in progress; no new writable span is granted")
            ;
    }

    if (auto status = ValidateRangeLocked(range, required); !status) {
        return status.GetError();
    }

    const std::uint64_t lease = next_lease_id++;
    pins.push_back(Pin{lease, range, writable});
    return PinnedSpan{liveness, range.base, HostPointer(range.base),
                      static_cast<std::size_t>(range.size), writable, lease};
}

// Requires lock. True when any live lease overlaps `range`.
bool GuestAddressSpace::AnyPinOverlapsLocked(GuestRange range) const {
    return std::any_of(pins.begin(), pins.end(),
                       [&](const Pin& pin) { return RangesOverlap(pin.range, range); });
}

void GuestAddressSpace::ReleasePin(std::uint64_t lease_id) {
    std::lock_guard guard{lock};
    std::erase_if(pins, [&](const Pin& pin) { return pin.lease_id == lease_id; });
}

Result<QuiescenceToken> GuestAddressSpace::Quiesce(std::uint64_t timeout_ns) {
    std::lock_guard guard{lock};
    if (active_quiescence != 0) {
        return MakeError(ErrorCategory::Busy, "GuestAddressSpace::Quiesce",
                         "another transaction already holds quiescence");
    }
    // An outstanding execution lease means a guest thread is inside the JIT right now. V0 does not
    // interrupt a running thread, so this reports Busy and changes nothing rather than publishing
    // underneath it. This gate is required even when the backend believes every thread is stopped:
    // without it a Run could start between PublishCode's memcpy and the backend's own check
    // (2026-09-08 poison review, R3).
    if (execution_leases != 0) {
        return MakeError(ErrorCategory::Busy, "GuestAddressSpace::Quiesce",
                         "guest code is executing: " + std::to_string(execution_leases) +
                             " execution lease(s) outstanding");
    }
    // In-flight HLE writers hold pins. Reporting Timeout without changing
    // anything is required: a partial stop must not be used to authorise a
    // transaction (API contract §7.2).
    if (!pins.empty()) {
        auto error = MakeError(ErrorCategory::Timeout, "GuestAddressSpace::Quiesce",
                               "HLE spans still pinned: " + std::to_string(pins.size()));
        error.system_error = static_cast<std::int64_t>(timeout_ns);
        return error;
    }

    const std::uint64_t epoch = next_quiescence_epoch++;
    active_quiescence = epoch;
    // Thread stopping is the CpuContext's responsibility; it supplies the
    // count. With no attached context this is a memory-only transaction.
    return QuiescenceToken{liveness, epoch, 0};
}

void GuestAddressSpace::ReleaseQuiescence(std::uint64_t epoch) {
    std::lock_guard guard{lock};
    if (active_quiescence == epoch) {
        active_quiescence = 0;
    }
}

// Requires lock. Checks that a token authorises a transaction on *this* space right now.
//
// Epoch alone is not enough: every space counts epochs independently, so A's first token and B's
// first token are both epoch 1, and B would accept A's token while B's own transaction was live.
// The 2026-09-08 publication review reproduced exactly that (P1-B). Provenance is checked through
// the liveness block the token already carries, which also rejects a token whose space is gone.
Status GuestAddressSpace::CheckTokenLocked(const QuiescenceToken& token,
                                           std::string_view operation) const {
    if (!token.IsValid()) {
        return MakeError(ErrorCategory::InvalidArgument, operation,
                         "an invalid quiescence token cannot authorise this operation");
    }
    if (!token.IsFrom(this)) {
        return MakeError(ErrorCategory::InvalidArgument, operation,
                         "the token belongs to a different address space, or its space has been "
                         "destroyed");
    }
    if (token.Epoch() != active_quiescence) {
        return MakeError(ErrorCategory::StaleEpoch, operation,
                         "token epoch does not match the active transaction");
    }
    return Ok();
}

Status GuestAddressSpace::SetCodeInvalidationSink(CodeInvalidationSink* sink) {
    if (sink == nullptr) {
        return MakeError(ErrorCategory::InvalidArgument,
                         "GuestAddressSpace::SetCodeInvalidationSink",
                         "sink must not be null; use ClearCodeInvalidationSink");
    }
    std::lock_guard guard{lock};
    if (code_sink != nullptr && code_sink != sink) {
        // Replacing it would strand the previous backend's translations: nothing
        // would ever invalidate them again.
        return MakeError(ErrorCategory::AlreadyActive,
                         "GuestAddressSpace::SetCodeInvalidationSink",
                         "another backend is already registered with this address space");
    }
    if (code_sink != sink) {
        ++sink_generation;
    }
    code_sink = sink;
    return Ok();
}

void GuestAddressSpace::ClearCodeInvalidationSink(CodeInvalidationSink* sink) {
    std::unique_lock guard{lock};
    if (code_sink != sink) {
        // Not the registered sink. Still wait: a publication that copied this pointer before an
        // earlier Clear may be running right now, and the caller is about to destroy the object.
        sink_idle.wait(guard, [&] { return sink_calls_in_flight == 0; });
        return;
    }
    code_sink = nullptr;
    ++sink_generation;
    // Wait for callbacks that already took the pointer. Clearing the slot alone is not enough:
    // publications call the sink with this lock released, so a call can be in progress with a
    // pointer the caller is about to free. The review reproduced Clear returning while a callback
    // was still active, a replacement registering, and the stale callback then reporting success.
    sink_idle.wait(guard, [&] { return sink_calls_in_flight == 0; });
}

bool GuestAddressSpace::HasPoisonedCode() const {
    std::lock_guard guard{lock};
    return code_poisoned;
}

Result<ExecutionLease> GuestAddressSpace::AcquireExecutionLease() {
    std::lock_guard guard{lock};
    if (code_poisoned) {
        // A failed publication left bytes that no longer match the backend's translation of them.
        // Executing anything in this space would risk running that translation.
        return MakeError(ErrorCategory::WrongState, "GuestAddressSpace::AcquireExecutionLease",
                         "code publication failed and has not been repaired; execution is blocked");
    }
    if (active_quiescence != 0) {
        return MakeError(ErrorCategory::Busy, "GuestAddressSpace::AcquireExecutionLease",
                         "a publication transaction holds this address space");
    }
    ++execution_leases;
    return ExecutionLease{this};
}

void GuestAddressSpace::ReleaseExecutionLease() {
    {
        std::lock_guard guard{lock};
        if (execution_leases > 0) {
            --execution_leases;
        }
        if (execution_leases != 0) {
            return;
        }
    }
    leases_idle.notify_all();
}

// Requires lock. Used when a publication cannot complete its backend half.
//
// Whole mappings only, matching Protect's refusal to split. A partial overlap therefore revokes
// execute from the entire containing mapping: over-revoking is safe here, leaving the range
// executable is not.
void GuestAddressSpace::RevokeExecuteLocked(GuestRange range) {
    for (auto& mapping : mappings) {
        if (!RangesOverlap(mapping.range, range)) {
            continue;
        }
        if (!HasPermission(mapping.permission, GuestPermission::Execute)) {
            continue;
        }
        const auto without_execute = static_cast<GuestPermission>(
            static_cast<std::uint32_t>(mapping.permission) &
            ~static_cast<std::uint32_t>(GuestPermission::Execute));
        if (::mprotect(HostPointer(mapping.range.base),
                       static_cast<std::size_t>(mapping.range.size),
                       ToHostProtection(without_execute)) == 0) {
            mapping.permission = without_execute;
            ++mapping_generation;
        }
        // If even mprotect fails there is nothing further this layer can do; code_poisoned still
        // records that the range must not be trusted, and PublishCode returns the sink's error.
    }
}

// Calls the registered sink with the lock released, counting the call so ClearCodeInvalidationSink
// can wait for it. `guard` owns `lock` on entry and on return.
//
// `committed` is false when the registration changed while the callback ran: the sink that answered
// is no longer the one that owns translated code, so its answer cannot authorise anything.
Status GuestAddressSpace::CallSinkUnlocked(std::unique_lock<std::mutex>& guard, GuestRange range,
                                           InvalidationReason reason, bool& committed) {
    committed = false;
    CodeInvalidationSink* sink = code_sink;
    if (sink == nullptr) {
        // No backend attached: a memory-only transaction. The generation bump is the whole of it.
        committed = true;
        return Ok();
    }

    const std::uint64_t generation_at_entry = sink_generation;
    ++sink_calls_in_flight;
    guard.unlock();

    // Unlocked: the lock order is context -> space, because the backend reads CodeGeneration()
    // under its own lock to build a snapshot. Calling a sink from under `lock` would invert it.
    Status status = sink->DiscardTranslations(range, reason);

    guard.lock();
    --sink_calls_in_flight;
    const bool same_registration = sink_generation == generation_at_entry;
    if (sink_calls_in_flight == 0) {
        // Notify with the lock held; Clear waits on this condition and re-checks the predicate.
        sink_idle.notify_all();
    }
    if (!same_registration) {
        return MakeError(ErrorCategory::WrongState, "GuestAddressSpace::PublishCode",
                         "the backend was unregistered while its invalidation was in flight");
    }
    committed = status.HasValue();
    return status;
}

Status GuestAddressSpace::InvalidateCode(const QuiescenceToken& token, GuestRange range,
                                         InvalidationReason reason) {
    std::unique_lock guard{lock};
    if (auto status = CheckTokenLocked(token, "GuestAddressSpace::InvalidateCode"); !status) {
        return status;
    }

    bool committed = false;
    auto status = CallSinkUnlocked(guard, range, reason, committed);

    // Re-check: the token could have been released while the sink ran unlocked.
    if (auto token_status = CheckTokenLocked(token, "GuestAddressSpace::InvalidateCode");
        !token_status) {
        return token_status;
    }
    if (!committed) {
        // Unlike PublishCode, no bytes changed here, so there is nothing to poison: the existing
        // translation still matches the existing bytes. Report the failure and change nothing.
        return status ? MakeError(ErrorCategory::BackendFailure,
                                  "GuestAddressSpace::InvalidateCode",
                                  "invalidation did not commit")
                      : status;
    }

    // Bumping the generation is what makes "success" mean stale decode can no
    // longer be entered. Every executable alias of this backing is covered
    // because the generation is space-wide, not per-VA (acceptance M10).
    ++code_generation;
    // A successful invalidation over the poisoned range is a repair: the translations that no
    // longer matched the bytes are gone.
    ClearPoisonIfRepairedLocked(range);
    return Ok();
}

Status GuestAddressSpace::PublishCode(const QuiescenceToken& token, GuestRange range,
                                      std::span<const std::byte> code) {
    if (code.size() != range.size) {
        return MakeError(ErrorCategory::InvalidArgument, "GuestAddressSpace::PublishCode",
                         "code length does not match the target range");
    }

    std::unique_lock guard{lock};
    if (auto status = CheckTokenLocked(token, "GuestAddressSpace::PublishCode"); !status) {
        return status;
    }

    // Validate and copy under one lock hold. Releasing it between the two would let a Protect or
    // Unmap land in the middle of a publication, which is precisely the state the token excludes.
    if (auto status = ValidateRangeLocked(range, GuestPermission::Write); !status) {
        return status;
    }
    std::memcpy(HostPointer(range.base), code.data(), code.size());

    bool committed = false;
    auto status = CallSinkUnlocked(guard, range, InvalidationReason::HostWrite, committed);

    if (!committed) {
        // The bytes are already new. From here the range must not become executable again: the
        // backend still holds a translation of the bytes that were there before, and running it
        // would execute code that no longer exists. Revoking execute on the guest pages is not
        // sufficient on its own -- the translation lives in the backend's own executable memory --
        // so poison additionally blocks execution leases and re-granting execute.
        RevokeExecuteLocked(range);
        code_poisoned = true;
        poisoned_range = range;
        // Still advance: the bytes did change, so a generation captured before this call must not
        // continue to compare equal.
        ++code_generation;
        return status ? MakeError(ErrorCategory::BackendFailure, "GuestAddressSpace::PublishCode",
                                  "publication did not commit")
                      : status;
    }

    if (auto token_status = CheckTokenLocked(token, "GuestAddressSpace::PublishCode");
        !token_status) {
        return token_status;
    }
    ++code_generation;
    ClearPoisonIfRepairedLocked(range);
    return Ok();
}

// Requires lock. Poison clears only when the range that failed has itself been republished or
// invalidated successfully. Clearing on any successful publication anywhere would let an unrelated
// range re-enable execution of the still-stale one.
void GuestAddressSpace::ClearPoisonIfRepairedLocked(GuestRange repaired) {
    if (!code_poisoned) {
        return;
    }
    const std::uint64_t poisoned_end = poisoned_range.base.value + poisoned_range.size;
    const std::uint64_t repaired_end = repaired.base.value + repaired.size;
    const bool covers = repaired.base.value <= poisoned_range.base.value &&
                        poisoned_end <= repaired_end;
    if (covers) {
        code_poisoned = false;
        poisoned_range = GuestRange{};
    }
}

Status GuestAddressSpace::RegisterAlias(GuestRange primary, GuestAddress alias_base) {
    std::lock_guard guard{lock};
    const bool known = std::any_of(mappings.begin(), mappings.end(), [&](const Mapping& m) {
        return m.range.base.value <= primary.base.value &&
               primary.base.value + primary.size <= m.range.base.value + m.range.size;
    });
    if (!known) {
        return MakeError(ErrorCategory::InvalidArgument, "GuestAddressSpace::RegisterAlias",
                         "primary range is not mapped");
    }
    aliases.push_back(Alias{primary, alias_base});
    ++mapping_generation;
    return Ok();
}

void GuestAddressSpace::AddObserver(MemoryObserver* observer) {
    if (observer == nullptr) {
        return;
    }
    std::lock_guard guard{lock};
    if (std::find(observers.begin(), observers.end(), observer) == observers.end()) {
        observers.push_back(observer);
    }
}

void GuestAddressSpace::RemoveObserver(MemoryObserver* observer) {
    std::lock_guard guard{lock};
    std::erase(observers, observer);
}

void GuestAddressSpace::NotifyObservers(GuestRange range) {
    // Widen to whole host pages before reporting. Over-reporting a dirty range
    // is acceptable; missing a notification is not (acceptance M12).
    GuestRange widened{};
    widened.base = GuestAddress{AlignDownToHostPage(range.base.value)};
    const std::uint64_t end = AlignUpToHostPage(range.base.value + range.size);
    widened.size = end > widened.base.value ? end - widened.base.value : range.size;

    std::vector<MemoryObserver*> snapshot;
    {
        std::lock_guard guard{lock};
        snapshot = observers;
    }
    // Called outside the lock: an observer must never be able to deadlock the
    // address space by calling back into it.
    for (auto* observer : snapshot) {
        observer->OnGuestWrite(widened);
    }
}

GuestAddressSpace::ResourceCounts GuestAddressSpace::Counts() const {
    std::lock_guard guard{lock};
    return ResourceCounts{mappings.size(), aliases.size(), pins.size(), observers.size()};
}

} // namespace Core::GuestCpu
