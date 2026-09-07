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
    void* reservation = ::mmap(nullptr, static_cast<std::size_t>(size), PROT_NONE,
                               MAP_PRIVATE | MAP_ANONYMOUS | MAP_NORESERVE, -1, 0);
    if (reservation == MAP_FAILED) {
        const int saved = errno;
        auto error = MakeError(CategoriseMapFailure(saved), "GuestAddressSpace::Create",
                               "reservation mmap failed");
        error.system_error = saved;
        return error;
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
    if (range.size == 0) {
        return MakeError(ErrorCategory::InvalidArgument, "GuestAddressSpace::ValidateRange",
                         "zero-length range");
    }
    if (range.base.value > std::numeric_limits<std::uint64_t>::max() - range.size) {
        return MakeError(ErrorCategory::InvalidArgument, "GuestAddressSpace::ValidateRange",
                         "base + size overflows");
    }

    std::lock_guard guard{lock};
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
    if (auto status = ValidateRange(range.Value(), GuestPermission::Read); !status) {
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
    if (auto status = ValidateRange(range.Value(), GuestPermission::Write); !status) {
        return status;
    }
    std::memcpy(HostPointer(to), from.data(), from.size());
    NotifyObservers(range.Value());
    return Ok();
}

Result<PinnedSpan> GuestAddressSpace::AcquirePinnedSpan(GuestRange range, bool writable) {
    const auto required = writable ? (GuestPermission::Read | GuestPermission::Write)
                                   : GuestPermission::Read;
    if (auto status = ValidateRange(range, required); !status) {
        return status.GetError();
    }

    std::lock_guard guard{lock};
    const std::uint64_t lease = next_lease_id++;
    pins.push_back(Pin{lease, range, writable});
    return PinnedSpan{liveness, range.base, HostPointer(range.base),
                      static_cast<std::size_t>(range.size), writable, lease};
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

Status GuestAddressSpace::InvalidateCode(const QuiescenceToken& token, GuestRange range,
                                         InvalidationReason reason) {
    if (!token.IsValid()) {
        return MakeError(ErrorCategory::InvalidArgument, "GuestAddressSpace::InvalidateCode",
                         "an invalid quiescence token cannot authorise invalidation");
    }

    std::lock_guard guard{lock};
    if (token.Epoch() != active_quiescence) {
        return MakeError(ErrorCategory::StaleEpoch, "GuestAddressSpace::InvalidateCode",
                         "token epoch does not match the active transaction");
    }
    // Range and reason are accepted but not yet used to narrow the
    // invalidation: V0 invalidates space-wide, which is conservative and
    // correct. They stay in the signature because a per-range backend
    // invalidation is the next step, and callers should already be passing
    // accurate values.
    (void)range;
    (void)reason;

    // Bumping the generation is what makes "success" mean stale decode can no
    // longer be entered. Every executable alias of this backing is covered
    // because the generation is space-wide, not per-VA (acceptance M10).
    ++code_generation;
    return Ok();
}

Status GuestAddressSpace::PublishCode(const QuiescenceToken& token, GuestRange range,
                                      std::span<const std::byte> code) {
    if (!token.IsValid()) {
        return MakeError(ErrorCategory::InvalidArgument, "GuestAddressSpace::PublishCode",
                         "an invalid quiescence token cannot authorise publication");
    }
    if (code.size() != range.size) {
        return MakeError(ErrorCategory::InvalidArgument, "GuestAddressSpace::PublishCode",
                         "code length does not match the target range");
    }

    {
        std::lock_guard guard{lock};
        if (token.Epoch() != active_quiescence) {
            return MakeError(ErrorCategory::StaleEpoch, "GuestAddressSpace::PublishCode",
                             "token epoch does not match the active transaction");
        }
    }

    if (auto status = ValidateRange(range, GuestPermission::Write); !status) {
        return status;
    }
    std::memcpy(HostPointer(range.base), code.data(), code.size());

    std::lock_guard guard{lock};
    ++code_generation;
    return Ok();
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
