// SPDX-FileCopyrightText: Copyright 2026 shadPS4 Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

// Owned guest address space: reservation, mapping, protection, aliases, code
// publication.
//
// "Owned" is the operative word (API contract §2). This object holds its own
// reservation and only ever uses MAP_FIXED inside it. It never reads
// /proc/self/maps looking for a hole and then races to claim it, because that
// hole can be taken by ART or a driver between the read and the mmap.

#pragma once

#include <condition_variable>
#include <cstddef>
#include <memory>
#include <mutex>
#include <optional>
#include <span>
#include <string_view>
#include <vector>

#include "core/guest_cpu/api/memory.h"
#include "core/guest_cpu/api/result.h"

namespace Core::GuestCpu {

// Callback for a component that needs to know when guest memory it watches was
// written. The GPU dirty tracker is the real consumer; V0 exercises the
// contract with a test double (acceptance M12).
class MemoryObserver {
public:
    virtual ~MemoryObserver() = default;
    virtual std::string_view Name() const = 0;
    // Called with the *conservatively widened* range. Over-reporting is
    // acceptable, missing a notification is not.
    virtual void OnGuestWrite(GuestRange range) = 0;
};

// The backend's half of a publication transaction.
//
// Only the backend owns translated code, so bumping this object's
// code_generation cannot by itself make stale decode unreachable. The
// 2026-09-08 publication review (P1-C) reproduced the consequence: a caller
// used PublishCode + InvalidateCode through the public API, both succeeded, the
// generation advanced 1 -> 3, and the guest still executed the *previous*
// fixture. The two same-named entry points meant different things.
//
// This is deliberately not a MemoryObserver. An observer returns void and is
// best-effort by contract; discarding translations is neither. A sink that
// fails must fail the publication, because the alternative is reporting success
// while the old block is still reachable.
//
// Declared here rather than in the backend so guest_cpu_api keeps building with
// no FEX include path (acceptance B05). The backend registers itself.
class CodeInvalidationSink {
public:
    virtual ~CodeInvalidationSink() = default;
    virtual std::string_view Name() const = 0;

    // Discard any translation covering `range`. Success must mean execution
    // after this point cannot enter stale decode -- not that work was queued.
    //
    // Called with the address space lock NOT held. The lock order in this API
    // is context -> space (the backend reads CodeGeneration() while holding its
    // own lock to build a snapshot), so calling a sink from under the space
    // lock would invert it.
    //
    // Must not call ClearCodeInvalidationSink on the space that is calling it:
    // that waits for this callback to finish and would deadlock on itself.
    [[nodiscard]] virtual Status DiscardTranslations(GuestRange range,
                                                     InvalidationReason reason) = 0;
};

// Permission to execute guest code, taken by the backend for the duration of
// one Run or Step.
//
// The 2026-09-08 poison review (R3) found that a quiescence token did not stop
// new execution from starting: PublishCode copied bytes first and the backend
// only checked for running threads afterwards, so a Run could enter the JIT on
// either side of that check. A token has to exclude execution for the whole
// transaction, not just for the instant the backend looks.
//
// The backend asks the space (context -> space, the established order). The
// space never calls into the backend to ask whether threads are running, which
// would invert it.
class ExecutionLease final {
public:
    ExecutionLease() = default;
    ExecutionLease(const ExecutionLease&) = delete;
    ExecutionLease& operator=(const ExecutionLease&) = delete;
    ExecutionLease(ExecutionLease&& other) noexcept;
    ExecutionLease& operator=(ExecutionLease&& other) noexcept;
    ~ExecutionLease();

    [[nodiscard]] bool IsValid() const noexcept {
        return space != nullptr;
    }

private:
    friend class GuestAddressSpace;
    explicit ExecutionLease(GuestAddressSpace* owner) : space(owner) {}
    void ReleaseIfOwned() noexcept;

    GuestAddressSpace* space{};
};

struct AddressSpaceConfig final {
    // Total guest reservation. Rounded up to a host page.
    std::uint64_t reservation_size{std::uint64_t{1} << 32};
    MemoryMode memory_mode{MemoryMode::DirectMapped};
    SmcMode smc_mode{SmcMode::ExplicitPublication};

    // Upper bound on guest addresses, or 0 for no constraint.
    //
    // This can express an embedder's placement policy as well as a backend
    // limit. FEX's masked lookup index still compares the full guest address;
    // an index collision alone does not cause execution of an unrelated block.
    //
    // Callers should take this from BackendCapabilities::max_guest_address.
    std::uint64_t max_address{};
};

class GuestAddressSpace final {
public:
    // Fails with UnsupportedMemoryMode / Unsupported when the caller asks for
    // a mode V0 does not implement, rather than quietly downgrading.
    [[nodiscard]] static Result<std::unique_ptr<GuestAddressSpace>> Create(
        const AddressSpaceConfig& config);

    GuestAddressSpace(const GuestAddressSpace&) = delete;
    GuestAddressSpace& operator=(const GuestAddressSpace&) = delete;
    ~GuestAddressSpace();

    [[nodiscard]] GuestAddress ReservationBase() const noexcept {
        return reservation_base;
    }
    [[nodiscard]] std::uint64_t ReservationSize() const noexcept {
        return reservation_size;
    }
    [[nodiscard]] MemoryMode Mode() const noexcept {
        return memory_mode;
    }
    [[nodiscard]] SmcMode Smc() const noexcept {
        return smc_mode;
    }
    [[nodiscard]] std::uint64_t MappingGeneration() const;
    [[nodiscard]] std::uint64_t CodeGeneration() const;

    // --- mapping -----------------------------------------------------------

    // Commits a host-page-aligned range inside the reservation. A request that
    // is not host-page aligned is rejected outright; V0 does not silently
    // widen, because the caller would then get permissions on memory it did
    // not ask for.
    [[nodiscard]] Result<MappingInfo> Map(GuestRange range, GuestPermission permission);
    [[nodiscard]] Status Unmap(GuestRange range);

    // Refuses with Unsupported when the request would need two different
    // permissions inside one host page, leaving everything untouched
    // (DEC-06, acceptance M03).
    [[nodiscard]] Status Protect(GuestRange range, GuestPermission permission);

    [[nodiscard]] Result<MappingInfo> Query(GuestAddress address) const;
    [[nodiscard]] std::vector<MappingInfo> Mappings() const;

    // Explicit access path for HLE / loader / debugger. DirectMapped JIT loads
    // and stores do NOT come through here (API contract §7.1).
    [[nodiscard]] Status Read(GuestAddress from, std::span<std::byte> into) const;
    [[nodiscard]] Status Write(GuestAddress to, std::span<const std::byte> from);

    [[nodiscard]] Status ValidateRange(GuestRange range, GuestPermission required) const;

    // Lease a host view for the duration of an HLE call.
    [[nodiscard]] Result<PinnedSpan> AcquirePinnedSpan(GuestRange range, bool writable);

    // --- transactions ------------------------------------------------------

    // Reserves an already-idle address space for publication. This V0 subset
    // does not interrupt guest owners or wait for HLE pins; a refusal leaves
    // state unchanged. Running-thread stop/acknowledgement is still pending.
    //
    // Refuses with Busy while any ExecutionLease is outstanding, and blocks new
    // leases for as long as the returned token lives. That admission gate is
    // required even when every thread is already stopped: without it a Run can
    // start between PublishCode's memcpy and the backend's running-thread check
    // (2026-09-08 poison review, R3).
    [[nodiscard]] Result<QuiescenceToken> Quiesce(std::uint64_t timeout_ns);

    // Taken by the backend around Run/Step. Refused while a transaction holds
    // quiescence, or while code is poisoned.
    [[nodiscard]] Result<ExecutionLease> AcquireExecutionLease();

    // Success means execution after this point cannot enter stale decode --
    // not merely that an invalidation was queued (API contract §7.2).
    //
    // Both reach the registered CodeInvalidationSink. Without one, a space with
    // no backend attached is a memory-only transaction and the generation bump
    // is the whole of it; once a backend is attached, its failure fails these.
    [[nodiscard]] Status InvalidateCode(const QuiescenceToken& token, GuestRange range,
                                        InvalidationReason reason);
    [[nodiscard]] Status PublishCode(const QuiescenceToken& token, GuestRange range,
                                     std::span<const std::byte> code);

    // Registers the backend that owns translated code. One at a time: a second
    // registration is refused rather than silently replacing the first, since
    // that would leave the displaced backend's caches unreachable by any
    // invalidation. Passing nullptr to Set is rejected; use Clear.
    [[nodiscard]] Status SetCodeInvalidationSink(CodeInvalidationSink* sink);

    // Unregisters and waits for any in-flight DiscardTranslations call on that
    // sink to return before doing so.
    //
    // The wait is the point. Publications copy the sink pointer under the lock
    // and call it unlocked, so clearing the slot alone left callers holding a
    // pointer the backend was about to destroy; the review reproduced a Clear
    // returning while a callback was still running, a replacement registering
    // immediately, and the old callback then reporting success (R2). Safe to
    // call when not registered, and safe to call twice.
    void ClearCodeInvalidationSink(CodeInvalidationSink* sink);

    // True when a publication failed to reach the backend. While set, execution
    // leases and execute permission are refused: the bytes already changed, so
    // the backend's translation of the previous bytes must never run again.
    // Cleared only by a successful publication or invalidation over the
    // poisoned range.
    [[nodiscard]] bool HasPoisonedCode() const;

    // Second VA for the same backing. Invalidating through one alias must
    // invalidate every executable alias (acceptance M10).
    [[nodiscard]] Status RegisterAlias(GuestRange primary, GuestAddress alias_base);

    // --- observers ---------------------------------------------------------

    void AddObserver(MemoryObserver* observer);
    void RemoveObserver(MemoryObserver* observer);

    // --- diagnostics -------------------------------------------------------

    struct ResourceCounts final {
        std::size_t mappings{};
        std::size_t aliases{};
        std::size_t live_pins{};
        std::size_t observers{};
    };
    [[nodiscard]] ResourceCounts Counts() const;

private:
    friend class QuiescenceToken;
    friend class PinnedSpan;
    friend class ExecutionLease;

    GuestAddressSpace(void* reservation, std::uint64_t size, const AddressSpaceConfig& config);

    void ReleaseQuiescence(std::uint64_t epoch);
    void ReleasePin(std::uint64_t lease_id);
    void ReleaseExecutionLease();
    [[nodiscard]] std::byte* HostPointer(GuestAddress address) const;
    void NotifyObservers(GuestRange range);

    // Both require `lock` to be held. Kept separate from their locking wrappers so a caller that
    // must validate and then act without a gap -- AcquirePinnedSpan, Read, Write, Protect -- can do
    // so under one lock hold rather than revalidating stale state.
    [[nodiscard]] Status ValidateRangeLocked(GuestRange range, GuestPermission required) const;
    [[nodiscard]] bool AnyPinOverlapsLocked(GuestRange range) const;

    // Requires `lock`. Shared by every token-consuming entry point so provenance, liveness and
    // epoch are checked the same way in each; having them check independently is how one of them
    // ended up accepting a token from another space.
    [[nodiscard]] Status CheckTokenLocked(const QuiescenceToken& token,
                                          std::string_view operation) const;

    // Requires `lock`. Drops execute from every mapping overlapping `range`, so a publication that
    // could not reach the backend leaves the old translation unreachable instead of running.
    void RevokeExecuteLocked(GuestRange range);

    // Requires `lock` via `guard`, which is unlocked around the callback and re-locked before
    // return. Counts the call so ClearCodeInvalidationSink can drain it.
    [[nodiscard]] Status CallSinkUnlocked(std::unique_lock<std::mutex>& guard, GuestRange range,
                                          InvalidationReason reason, bool& committed);

    // Requires `lock`. Clears only the failed intervals fully covered by this repair.
    void ClearPoisonIfRepairedLocked(GuestRange repaired);
    void PoisonCodeLocked(GuestRange range);
    [[nodiscard]] Status CheckMappingMutationLocked(std::string_view operation) const;

    struct Mapping final {
        GuestRange range{};
        GuestPermission permission{GuestPermission::None};
        ProtectionReason reasons{ProtectionReason::GuestPermission};
        std::uint64_t generation{};
    };
    struct Alias final {
        GuestRange primary{};
        GuestAddress alias_base{};
    };
    struct Pin final {
        std::uint64_t lease_id{};
        GuestRange range{};
        bool writable{};
    };

    mutable std::mutex lock;
    void* reservation_host{};
    GuestAddress reservation_base{};
    std::uint64_t reservation_size{};
    MemoryMode memory_mode{MemoryMode::DirectMapped};
    SmcMode smc_mode{SmcMode::ExplicitPublication};

    std::vector<Mapping> mappings;
    std::vector<Alias> aliases;
    std::vector<Pin> pins;
    std::vector<MemoryObserver*> observers;

    std::uint64_t mapping_generation{1};
    std::uint64_t code_generation{1};
    std::uint64_t next_quiescence_epoch{1};
    std::uint64_t active_quiescence{};
    std::uint64_t next_lease_id{1};
    CodeInvalidationSink* code_sink{};
    // Number of DiscardTranslations calls on the current or draining registration.
    // Clear waits for this to reach zero before returning, so a backend can
    // destroy itself immediately afterwards.
    std::size_t sink_calls_in_flight{};
    // Bumped on every registration change. A callback that started under an
    // earlier registration cannot commit its result: the sink it ran against is
    // no longer the one that owns translated code.
    std::uint64_t sink_generation{1};
    // Outstanding execution leases. A transaction cannot start while any is
    // held, and no new one may be taken while a transaction is active.
    std::size_t execution_leases{};
    // Set when a publication modified bytes but could not discard the matching
    // translations. Blocks execution leases and execute permission until a
    // publication or invalidation over the poisoned range succeeds; reporting
    // success without that would let stale translated code run against bytes
    // that no longer exist.
    std::vector<GuestRange> poisoned_ranges;
    bool sink_draining{};
    std::condition_variable sink_idle;
    std::condition_variable leases_idle;
    // Handed to tokens and pins as a weak reference so they can tell whether
    // this object still exists when they are released.
    std::shared_ptr<AddressSpaceLiveness> liveness;
};

} // namespace Core::GuestCpu
