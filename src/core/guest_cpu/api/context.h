// SPDX-FileCopyrightText: Copyright 2026 shadPS4 Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

// Context and thread handles: the backend-free half of the execution API.
//
// Nothing here names FEX. A consumer compiles this with -I src alone (B05), and
// the backend is selected at link time through CreateContext. That boundary is
// the point: a second backend, or a test double, must not require changes here.

#pragma once

#include <cstdint>
#include <memory>
#include <string>
#include <vector>

#include "core/guest_cpu/api/address_space.h"
#include "core/guest_cpu/api/execution.h"
#include "core/guest_cpu/api/registers.h"
#include "core/guest_cpu/api/result.h"

namespace Core::GuestCpu {

// Which guest features the backend will actually execute. Declared rather than
// assumed: C03 requires that asking for something outside this set is refused
// with a defined error instead of faulting somewhere inside the JIT.
enum class GuestFeature : std::uint32_t {
    None = 0,
    // Integer arithmetic, compare, shift, carry and branches. Always present.
    BaseInteger = 1u << 0,
    Sse2 = 1u << 1,
    // Not implemented in V0. Present so a caller can ask and be refused.
    Avx = 1u << 2,
};

[[nodiscard]] constexpr GuestFeature operator|(GuestFeature a, GuestFeature b) noexcept {
    return static_cast<GuestFeature>(static_cast<std::uint32_t>(a) |
                                     static_cast<std::uint32_t>(b));
}
[[nodiscard]] constexpr bool Contains(GuestFeature set, GuestFeature wanted) noexcept {
    return (static_cast<std::uint32_t>(set) & static_cast<std::uint32_t>(wanted)) ==
           static_cast<std::uint32_t>(wanted);
}

// What the linked backend actually is and can do. Reported, never inferred by
// the caller from a version string.
struct BackendCapabilities final {
    std::string backend_name;
    // Upstream revision the backend was built from, and the downstream branch
    // point when the build is a fork. Both recorded: a fork's own hash does not
    // tell a reader which upstream it descends from.
    std::string upstream_revision;
    std::string downstream_revision;

    GuestFeature features{GuestFeature::None};
    StepScope step_scope{StepScope::None};
    MemoryMode memory_mode{MemoryMode::DirectMapped};
    SmcMode smc_mode{SmcMode::ExplicitPublication};

    // The host page size the backend was actually told to use. Reported so a
    // caller can check it against its own sysconf value rather than trusting
    // that the injection happened.
    std::uint64_t host_page_size{};
    std::uint32_t snapshot_schema_version{CpuSnapshot::kSchemaVersion};

    // Address guest code must reach for Run to report StopReason::Returned.
    //
    // Exposed because a caller assembling guest code has to encode it, and
    // because it is what makes a normal return distinguishable from a guest
    // HLT anywhere else (acceptance D05). The backend chooses the address, so
    // a caller must read it rather than assume a fixed location.
    std::uint64_t return_gate_address{};

    // Highest guest address the backend can execute from, or 0 for no limit.
    //
    // Not a host limit: FEXCore's block lookup masks the guest RIP with its own
    // virtual memory size, so code above this aliases onto unrelated cache
    // entries and the wrong block runs with no error reported. Pass this to
    // AddressSpaceConfig::max_address so the reservation lands somewhere the
    // backend can actually address.
    std::uint64_t max_guest_address{};
};

struct CpuConfig final {
    static constexpr std::uint32_t kApiVersion = 1;

    std::uint32_t api_version{kApiVersion};
    GuestFeature requested_features{GuestFeature::BaseInteger | GuestFeature::Sse2};
    MemoryMode memory_mode{MemoryMode::DirectMapped};
    SmcMode smc_mode{SmcMode::ExplicitPublication};
};

// Initial architectural state for a guest thread. Deliberately not a host
// function pointer: entry_rip and initial_rsp must be addresses the caller has
// already mapped in the address space this context was created against.
struct ThreadInit final {
    GuestCodeAddress entry_rip{};
    GuestAddress initial_rsp{};
    std::uint64_t guest_tid{};
    // Applied before the first instruction. Only the fields in the patch mask
    // are written, so an unset field is the architectural default rather than
    // an accidental zero.
    RegisterPatch initial_state{};
};

// Opaque identity for a guest thread: id plus generation, so a handle to a
// destroyed thread is distinguishable from a live one that reused its slot
// rather than silently addressing the wrong thread.
struct ThreadHandle final {
    std::uint64_t id{};
    std::uint64_t generation{};

    [[nodiscard]] bool IsValid() const noexcept {
        return id != 0 && generation != 0;
    }
    [[nodiscard]] friend bool operator==(ThreadHandle a, ThreadHandle b) noexcept {
        return a.id == b.id && a.generation == b.generation;
    }
};

// Owns the backend, its code cache and its association with one address space.
//
// V0 permits one live context per process (API contract §2). A second
// concurrent CreateContext returns AlreadyActive; destroying the first must
// make a rebuild possible, so this is a live-instance rule, not a once-ever
// latch.
class CpuContext {
public:
    virtual ~CpuContext() = default;

    CpuContext(const CpuContext&) = delete;
    CpuContext& operator=(const CpuContext&) = delete;

    [[nodiscard]] virtual BackendCapabilities Capabilities() const = 0;

    // Owner-thread operations. The thread that calls CreateThread becomes that
    // guest thread's owner and is the only thread permitted to Run, Step or
    // destroy it; other threads get WrongThread rather than a data race.
    [[nodiscard]] virtual Result<ThreadHandle> CreateThread(const ThreadInit& init) = 0;
    [[nodiscard]] virtual Result<RunResult> Run(ThreadHandle thread, const RunOptions& options) = 0;
    [[nodiscard]] virtual Result<RunResult> Step(ThreadHandle thread,
                                                 const StepOptions& options) = 0;
    [[nodiscard]] virtual Result<void> DestroyThread(ThreadHandle thread) = 0;

    // Initial stopped-thread guest-call subset (NOT a re-entrant HLE callback).
    // Enters `entry` as a normal SysV integer-only function on
    // an already-owned guest thread: integer arguments go to rdi/rsi/rdx/rcx/r8/r9
    // (spill to the guest stack past six), the backend's return gate is pushed as
    // the return address, and the call returns when the thread lands back on that
    // gate. The thread's guest stack, FS/TLS and heap persist, so a sequence of
    // boot calls (DT_INIT, `_malloc_init`, ...) share state as the PS4 kernel
    // intends. A fault, unresolved import or cancellation during the call is
    // reported in the result, never folded into a zero return value.
    //
    // Successful return restores caller RIP/RSP and any per-call FS override.
    // A failed preflight changes neither registers nor stack bytes. Nested calls
    // are refused until HleScope/WaitingHle is implemented and validated.
    // Fault/cancel results preserve the actual stopped callee state; they do not
    // pretend the caller continuation was reached. Resume/recovery follows Run.
    // Owner-thread only, same as Run: the caller must own `thread` and it must be
    // stopped (not mid-Run). This is the primitive the Orbis linker/module start
    // path uses instead of casting a guest VA to a native function pointer.
    [[nodiscard]] virtual Result<GuestCallResult> InvokeGuest(ThreadHandle thread,
                                                              GuestCodeAddress entry,
                                                              const GuestCallArgs& args,
                                                              const GuestCallOptions& options) = 0;

    // Readable at a stopped boundary. The returned snapshot's kind says how
    // much of it is authoritative; it is never silently upgraded to SafePoint.
    [[nodiscard]] virtual Result<CpuSnapshot> ReadRegisters(ThreadHandle thread) const = 0;

    // Owner + stopped + matching epoch. A stale epoch is refused so a write
    // computed against an older stop cannot land on newer state.
    [[nodiscard]] virtual Result<void> WriteRegisters(ThreadHandle thread,
                                                      const RegisterPatch& patch,
                                                      std::uint64_t stop_epoch) = 0;

    [[nodiscard]] virtual std::size_t LiveThreadCount() const = 0;

    // Discards translated code for `range` so the next execution re-decodes it.
    //
    // GuestAddressSpace::InvalidateCode records the generation change, but only
    // the backend owns the translated code, so the invalidation has to reach
    // here too. Requires a QuiescenceToken: dropping a block while a thread is
    // executing inside it is a use-after-free, and the token is the proof that
    // no thread is (API contract §7.2).
    //
    // Success means execution after this point cannot enter stale decode, not
    // that an invalidation was queued.
    [[nodiscard]] virtual Result<void> InvalidateCode(const QuiescenceToken& token,
                                                      GuestRange range,
                                                      InvalidationReason reason) = 0;

    // --- coordinated quiescence (Round 2 G2) -------------------------------
    //
    // QuiesceContext stops the whole context for a code/mapping transaction. It is the controller
    // side that backs GuestAddressSpace::Quiesce: the space token alone can only refuse new work,
    // it cannot stop owners that are already inside Run. This does.
    //
    // It atomically closes admission for new Run/CreateThread/Resume, pauses every running owner,
    // and waits for each Run to return (which is when the execution lease is released), then takes
    // the address-space QuiescenceToken. Only then is the transaction allowed to change backing,
    // publish bytes or discard translations. Commit/End reopens admission; on failure the context
    // stays closed and any owner whose stop faulted is left faulted -- an owner is never reported
    // stopped when its ack was not received. Retry QuiesceContext after a timeout to finish the
    // same drain; there is no implicit abort/resume. Only this transaction's Pause tickets are
    // retired. External Pause/Cancel/Shutdown remain pending until an explicit Resume.
    // timeout_ns=0 selects one second; all owners share one bounded deadline (maximum 60s).
    [[nodiscard]] virtual Result<QuiescenceToken> QuiesceContext(std::uint64_t timeout_ns) = 0;

    // Discards every translation the backend holds, for a whole-context remap or a clean-code-cache
    // request. Requires a current token from this address space (a memory-only Quiesce token
    // is also sufficient when no execution leases exist). Covers the owned guest reservation,
    // including removed/RW mappings, and the backend return gate. Preserves guest register state.
    [[nodiscard]] virtual Result<void> ClearCodeCache(const QuiescenceToken& token) = 0;

    // --- asynchronous control (Round 2 G1) ---------------------------------
    //
    // The FEX backend enables fault-page checks at JIT entry boundaries. A
    // requested stop spills state and returns Run to its owner; no native host
    // stack remains parked in the JIT. See
    // docs/validation/round2/g1-control-decision.md for the pinned source proof.
    // Resume changes admission only: the owner explicitly calls Run again.

    // Callable from any thread, including while the target is executing.
    //
    // Returns a ticket identifying this request. Requesting again before the
    // first is acknowledged is allowed and does not lose the earlier reason:
    // precedence is Fault/BackendFailure > Cancel > Pause.
    [[nodiscard]] virtual Result<InterruptTicket> RequestInterrupt(ThreadHandle thread,
                                                                   InterruptReason reason) = 0;

    // Waits until the ticket's thread has left the JIT, spilled, and published a
    // snapshot. An already stopped thread reuses its frozen snapshot. Repeated
    // reads never advance stop_epoch; retired tickets are refused.
    //
    // A timeout returns Timeout and leaves the request pending; it must never
    // report a stop that did not happen, and must not destroy a thread that is
    // still running. A running owner cannot wait for itself.
    [[nodiscard]] virtual Result<StopReceipt> WaitStopped(const InterruptTicket& ticket,
                                                          std::uint64_t timeout_ns) = 0;

    // Clears a pause so the owner's next Run may proceed. Consumes exactly the
    // named epoch: a newer request stays pending, so a Resume racing a second
    // Pause cannot swallow it.
    [[nodiscard]] virtual Result<void> Resume(ThreadHandle thread,
                                              std::uint64_t acknowledged_epoch) = 0;

    // Identity of this context instance. Non-zero, and different for a context
    // created after this one is destroyed, so a stale ticket cannot match.
    [[nodiscard]] virtual std::uint64_t ContextId() const noexcept = 0;

protected:
    CpuContext() = default;
};

// Creates the context using whichever backend is linked in.
//
// `space` must outlive the returned context: the backend holds guest mappings
// from it for the lifetime of every thread. Ownership is not taken, because the
// address space is shared with host services that are not part of this API.
[[nodiscard]] Result<std::unique_ptr<CpuContext>> CreateContext(const CpuConfig& config,
                                                                GuestAddressSpace& space);

// Describes the backend without creating one, for a caller deciding whether the
// build it has can run what it needs.
[[nodiscard]] BackendCapabilities QueryBackendCapabilities();

} // namespace Core::GuestCpu
