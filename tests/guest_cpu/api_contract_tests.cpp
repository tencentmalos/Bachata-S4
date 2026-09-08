// SPDX-FileCopyrightText: Copyright 2026 shadPS4 Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

// Host-side contract tests for the public guest CPU API.
//
// Scope discipline (decision DEC-07): these run on the build host, so they
// prove page-size arithmetic and POSIX mapping semantics against the *real*
// host page size -- which is 16384 on this machine. They prove nothing about
// bionic, ART, or FEX execution. Anything requiring those is NOT_RUN until a
// device is attached.
//
// Deliberately dependency-free: no GTest, so this builds and runs anywhere the
// API itself builds.

#include <algorithm>
#include <array>
#include <atomic>
#include <cstdio>
#include <cstring>
#include <string>
#include <thread>
#include <vector>

#include <unistd.h>

#include "core/guest_cpu/api/address_space.h"
#include "core/guest_cpu/api/execution.h"
#include "core/guest_cpu/api/memory.h"
#include "core/guest_cpu/api/registers.h"

namespace {

using namespace Core::GuestCpu;

struct CaseResult final {
    std::string id;
    std::string name;
    bool passed{};
    std::string detail;
};

std::vector<CaseResult> g_results;
std::string g_current_id;
std::string g_current_name;
bool g_current_ok = true;
std::string g_current_detail;

void Check(bool condition, const char* what) {
    if (!condition && g_current_ok) {
        g_current_ok = false;
        g_current_detail = what;
    }
}

template <typename Fn>
void RunCase(const char* id, const char* name, Fn&& body) {
    g_current_id = id;
    g_current_name = name;
    g_current_ok = true;
    g_current_detail.clear();
    body();
    g_results.push_back(CaseResult{g_current_id, g_current_name, g_current_ok, g_current_detail});
    std::printf("[%-5s] %-52s %s%s%s\n", id, name, g_current_ok ? "PASS" : "FAIL",
                g_current_ok ? "" : " -- ", g_current_detail.c_str());
}

std::unique_ptr<GuestAddressSpace> MakeSpace(std::uint64_t size = 64ull * 1024 * 1024) {
    AddressSpaceConfig config{};
    config.reservation_size = size;
    auto space = GuestAddressSpace::Create(config);
    if (!space) {
        std::printf("fatal: could not create address space: %s\n",
                    Describe(space.GetError()).c_str());
        std::abort();
    }
    return std::move(space).Value();
}

// A stand-in for the GPU dirty tracker. It is a test double for the observer
// *protocol* only; it is not a model of the real page manager (spec §5.1).
class RecordingObserver final : public MemoryObserver {
public:
    std::string_view Name() const override {
        return "test.gpu-dirty-double";
    }
    void OnGuestWrite(GuestRange range) override {
        ranges.push_back(range);
    }
    std::vector<GuestRange> ranges;
};

// --- page size --------------------------------------------------------------

void TestHostPageDiscovery() {
    RunCase("M01a", "host page size is discovered, power of two", [] {
        const std::uint64_t page = HostPageSize();
        Check(page >= 4096, "host page smaller than 4096");
        Check((page & (page - 1)) == 0, "host page is not a power of two");
        // The value must come from the platform. If this were a compile-time
        // constant, one APK could not serve both 4 KiB and 16 KiB (B04).
        Check(page == static_cast<std::uint64_t>(::sysconf(_SC_PAGESIZE)),
              "HostPageSize disagrees with sysconf");
    });

    RunCase("M01b", "three page quantities stay distinct", [] {
        // DEC-04: collapsing these is the failure mode the spec forbids.
        Check(kGuestAbiPageSize == 4096, "guest ABI page must stay 4096");
        Check(kCodeIndexGranularity == 4096, "code index granularity must stay 4096");
        if (HostPageSize() != 4096) {
            Check(HostPageSize() != kGuestAbiPageSize,
                  "host page must not be conflated with the guest ABI page");
        }
    });

    RunCase("M04a", "align-up reports overflow instead of wrapping", [] {
        const std::uint64_t huge = ~std::uint64_t{0} - 8;
        Check(AlignUpToHostPage(huge) == 0, "overflow must yield 0, not a small value");
        Check(AlignUpToHostPage(1) == HostPageSize(), "align-up of 1 is one page");
        Check(AlignUpToHostPage(0) == 0, "align-up of 0 is 0");
        Check(AlignUpToHostPage(HostPageSize()) == HostPageSize(), "exact page is unchanged");
    });
}

// --- mapping ----------------------------------------------------------------

void TestMappingLifecycle() {
    RunCase("M01c", "map, write, read back, unmap three host pages", [] {
        auto space = MakeSpace();
        const std::uint64_t page = HostPageSize();
        auto range = GuestRange::Checked(space->ReservationBase(), page * 3);
        Check(range.HasValue(), "range construction failed");
        if (!range) return;

        auto mapped = space->Map(range.Value(), GuestPermission::Read | GuestPermission::Write);
        Check(mapped.HasValue(), "map of three host pages failed");
        if (!mapped) return;
        Check(mapped.Value().range.size == page * 3, "mapped size mismatch");

        const std::string payload = "shadps4-v0-page-probe";
        std::vector<std::byte> out(payload.size());
        std::memcpy(out.data(), payload.data(), payload.size());
        Check(space->Write(range.Value().base, out).HasValue(), "write failed");

        std::vector<std::byte> back(payload.size());
        Check(space->Read(range.Value().base, back).HasValue(), "read failed");
        Check(std::memcmp(back.data(), payload.data(), payload.size()) == 0,
              "read-back content mismatch");

        Check(space->Counts().mappings == 1, "mapping count should be 1");
        Check(space->Unmap(range.Value()).HasValue(), "unmap failed");
        Check(space->Counts().mappings == 0, "mapping count should return to 0");
    });

    RunCase("M02", "four 4 KiB logical slices inside one host page", [] {
        auto space = MakeSpace();
        const std::uint64_t page = HostPageSize();
        auto range = GuestRange::Checked(space->ReservationBase(), page).Value();
        Check(space->Map(range, GuestPermission::Read | GuestPermission::Write).HasValue(),
              "map failed");

        // Logical segmentation only. These slices share one host page and so
        // cannot have distinct host permissions -- that is M03's job.
        const std::uint64_t slices = page / kGuestAbiPageSize;
        for (std::uint64_t i = 0; i < slices; ++i) {
            const auto marker = static_cast<std::uint8_t>(0xA0 + i);
            std::vector<std::byte> data(kGuestAbiPageSize, static_cast<std::byte>(marker));
            const GuestAddress at{range.base.value + i * kGuestAbiPageSize};
            Check(space->Write(at, data).HasValue(), "slice write failed");
        }
        for (std::uint64_t i = 0; i < slices; ++i) {
            const auto expected = static_cast<std::byte>(0xA0 + i);
            std::vector<std::byte> back(kGuestAbiPageSize);
            const GuestAddress at{range.base.value + i * kGuestAbiPageSize};
            Check(space->Read(at, back).HasValue(), "slice read failed");
            const bool uniform = std::all_of(back.begin(), back.end(),
                                             [&](std::byte b) { return b == expected; });
            Check(uniform, "a slice write leaked into a neighbouring slice");
        }
    });

    RunCase("M03", "conflicting sub-page permission is refused, state unchanged", [] {
        auto space = MakeSpace();
        const std::uint64_t page = HostPageSize();
        auto range = GuestRange::Checked(space->ReservationBase(), page).Value();
        Check(space->Map(range, GuestPermission::Read | GuestPermission::Write).HasValue(),
              "map failed");

        const std::uint64_t generation_before = space->MappingGeneration();
        const auto permission_before = space->Query(range.base).Value().permission;

        // Ask for read-only on just the first guest-ABI page of a host page.
        // On a 16 KiB host this is impossible; on a 4 KiB host it is a whole
        // page and legal. Assert the right thing for the actual host.
        auto sub = GuestRange::Checked(range.base, kGuestAbiPageSize).Value();
        auto status = space->Protect(sub, GuestPermission::Read);

        if (page > kGuestAbiPageSize) {
            Check(!status.HasValue(), "sub-host-page protect should be refused");
            if (!status.HasValue()) {
                Check(status.GetError().category == ErrorCategory::Unsupported,
                      "refusal must be Unsupported");
            }
            // The refusal must be total: nothing may have changed.
            Check(space->MappingGeneration() == generation_before,
                  "a refused protect must not bump the generation");
            Check(space->Query(range.base).Value().permission == permission_before,
                  "a refused protect must not change permissions");
            // And the page must still actually be writable.
            std::vector<std::byte> probe(4, std::byte{0x5A});
            Check(space->Write(range.base, probe).HasValue(),
                  "memory must remain writable after a refused protect");
        } else {
            Check(status.HasValue(), "on a 4 KiB host this is a whole page and must succeed");
        }
    });

    RunCase("M04b", "malformed and conflicting requests are rejected", [] {
        auto space = MakeSpace();
        const std::uint64_t page = HostPageSize();

        Check(!GuestRange::Checked(space->ReservationBase(), 0).HasValue(),
              "zero length must be rejected");
        Check(!GuestRange::Checked(GuestAddress{~std::uint64_t{0} - 4}, 64).HasValue(),
              "base + size overflow must be rejected");

        // Misaligned base, using an offset that is misaligned on any host.
        auto misaligned = GuestRange::Checked(GuestAddress{space->ReservationBase().value + 1},
                                              page).Value();
        Check(!space->Map(misaligned, GuestPermission::Read).HasValue(),
              "misaligned base must be rejected");

        // Outside the reservation.
        auto outside = GuestRange::Checked(
            GuestAddress{space->ReservationBase().value + space->ReservationSize()}, page).Value();
        Check(!space->Map(outside, GuestPermission::Read).HasValue(),
              "range outside the reservation must be rejected");

        // Overlap with an existing mapping.
        auto first = GuestRange::Checked(space->ReservationBase(), page * 2).Value();
        Check(space->Map(first, GuestPermission::Read).HasValue(), "first map failed");
        auto overlapping = GuestRange::Checked(
            GuestAddress{space->ReservationBase().value + page}, page * 2).Value();
        Check(!space->Map(overlapping, GuestPermission::Read).HasValue(),
              "overlapping map must be rejected");
        Check(space->Counts().mappings == 1, "a rejected map must not be recorded");
    });

    RunCase("M05", "permission is enforced on the explicit access path", [] {
        auto space = MakeSpace();
        const std::uint64_t page = HostPageSize();
        auto range = GuestRange::Checked(space->ReservationBase(), page).Value();
        Check(space->Map(range, GuestPermission::Read).HasValue(), "map read-only failed");

        std::vector<std::byte> data(8, std::byte{0x11});
        auto write = space->Write(range.base, data);
        Check(!write.HasValue(), "write to a read-only mapping must fail");
        if (!write.HasValue()) {
            Check(write.GetError().category == ErrorCategory::PermissionDenied,
                  "refusal must be PermissionDenied");
        }

        // Unmapped address.
        const GuestAddress unmapped{space->ReservationBase().value + page * 8};
        std::vector<std::byte> sink(8);
        Check(!space->Read(unmapped, sink).HasValue(), "read of unmapped memory must fail");

        // A span crossing out of its mapping is rejected rather than truncated.
        std::vector<std::byte> oversized(static_cast<std::size_t>(page) + 16);
        Check(!space->Read(range.base, oversized).HasValue(),
              "a span crossing the mapping end must be rejected");
    });
}

// --- transactions -----------------------------------------------------------

void TestTransactions() {
    RunCase("M13", "a pinned HLE span blocks unmap until released", [] {
        auto space = MakeSpace();
        const std::uint64_t page = HostPageSize();
        auto range = GuestRange::Checked(space->ReservationBase(), page).Value();
        Check(space->Map(range, GuestPermission::Read | GuestPermission::Write).HasValue(),
              "map failed");

        {
            auto pin = space->AcquirePinnedSpan(range, true);
            Check(pin.HasValue(), "acquiring a pin failed");
            if (!pin) return;
            Check(space->Counts().live_pins == 1, "pin should be counted");

            auto unmap = space->Unmap(range);
            Check(!unmap.HasValue(), "unmap must not succeed while a span is pinned");
            if (!unmap.HasValue()) {
                Check(unmap.GetError().category == ErrorCategory::Busy, "refusal must be Busy");
            }

            // The span must still be usable after the refused unmap.
            auto bytes = pin.Value().WritableBytes();
            Check(bytes.size() == page, "pinned span size mismatch");
            bytes[0] = std::byte{0x7E};
            Check(bytes[0] == std::byte{0x7E}, "pinned span must remain writable");
        }

        Check(space->Counts().live_pins == 0, "pin must be released on scope exit");
        Check(space->Unmap(range).HasValue(), "unmap must succeed after release");
    });

    RunCase("M07", "publication needs a live token and bumps the generation", [] {
        auto space = MakeSpace();
        const std::uint64_t page = HostPageSize();
        auto range = GuestRange::Checked(space->ReservationBase(), page).Value();
        // RW first, then flip to RX after publishing. Mapping RWX and leaving
        // it there is what spec §5.1 forbids, and macOS W^X rejects it outright
        // (mmap returns EACCES), so the correct flow is also the only portable
        // one.
        Check(space->Map(range, GuestPermission::Read | GuestPermission::Write).HasValue(),
              "map failed");

        auto code = GuestRange::Checked(range.base, 16).Value();
        std::vector<std::byte> body(16, std::byte{0xCC});

        // An invalid token must not authorise anything.
        QuiescenceToken invalid;
        Check(!space->PublishCode(invalid, code, body).HasValue(),
              "an invalid token must not authorise publication");

        const std::uint64_t before = space->CodeGeneration();
        {
            auto token = space->Quiesce(1'000'000);
            Check(token.HasValue(), "quiesce failed");
            if (!token) return;
            Check(space->PublishCode(token.Value(), code, body).HasValue(), "publish failed");
            Check(space->CodeGeneration() > before, "publication must bump the code generation");

            // Length mismatch is caught rather than truncating the copy.
            std::vector<std::byte> wrong(8, std::byte{0x90});
            Check(!space->PublishCode(token.Value(), code, wrong).HasValue(),
                  "length mismatch must be rejected");
        }

        std::vector<std::byte> back(16);
        Check(space->Read(code.base, back).HasValue(), "read-back failed");
        const bool all_cc = std::all_of(back.begin(), back.end(),
                                        [](std::byte b) { return b == std::byte{0xCC}; });
        Check(all_cc, "published bytes were not observed");

        // Complete the RW -> RX transition. No window of RWX at any point.
        Check(space->Protect(range, GuestPermission::Read | GuestPermission::Execute).HasValue(),
              "RW to RX transition failed");
        Check(space->Query(range.base).Value().permission ==
                  (GuestPermission::Read | GuestPermission::Execute),
              "permission should read back as RX");
        std::vector<std::byte> reject(4, std::byte{0x00});
        Check(!space->Write(range.base, reject).HasValue(),
              "the published range must no longer be writable");
    });

    RunCase("M14a", "long-term RWX is not obtainable through this API on a W^X host", [] {
        // Not a claim that RWX is refused by policy in our code -- it is a
        // record of what the host actually allows, so the JIT publication
        // design cannot quietly depend on RWX being available.
        auto space = MakeSpace();
        const std::uint64_t page = HostPageSize();
        auto range = GuestRange::Checked(space->ReservationBase(), page).Value();
        auto rwx = space->Map(range, GuestPermission::Read | GuestPermission::Write |
                                         GuestPermission::Execute);
        if (rwx.HasValue()) {
            std::printf("       note: this host permits RWX mappings; "
                        "publication must still use RW->RX\n");
        } else {
            std::printf("       note: host refused RWX (%s); RW->RX publication is mandatory\n",
                        ToString(rwx.GetError().category).data());
        }
        // Either outcome is acceptable; the case exists to record which.
        Check(true, "");
    });

    RunCase("M13b", "quiesce reports Timeout when an HLE writer is still in flight", [] {
        auto space = MakeSpace();
        const std::uint64_t page = HostPageSize();
        auto range = GuestRange::Checked(space->ReservationBase(), page).Value();
        Check(space->Map(range, GuestPermission::Read | GuestPermission::Write).HasValue(),
              "map failed");

        auto pin = space->AcquirePinnedSpan(range, true);
        Check(pin.HasValue(), "pin failed");
        auto token = space->Quiesce(1'000);
        Check(!token.HasValue(), "quiesce must not succeed with a writer in flight");
        if (!token.HasValue()) {
            Check(token.GetError().category == ErrorCategory::Timeout, "must report Timeout");
        }
        // An incomplete stop must leave the space untouched and reusable.
        pin.Value().Release();
        Check(space->Quiesce(1'000).HasValue(), "quiesce must succeed once writers are gone");
    });

    RunCase("M13c", "an active transaction excludes every writer path, not just pinned ones", [] {
        auto space = MakeSpace();
        const std::uint64_t page = HostPageSize();
        auto range = GuestRange::Checked(space->ReservationBase(), page).Value();
        Check(space->Map(range, GuestPermission::Read | GuestPermission::Write).HasValue(),
              "map failed");

        const std::array<std::byte, 8> data{};
        Check(space->Write(range.base, data).HasValue(),
              "an explicit write must be allowed with no transaction in progress");

        auto token = space->Quiesce(1'000'000);
        Check(token.HasValue(), "quiesce failed");

        // Write() previously skipped this check while AcquirePinnedSpan enforced it, so a
        // transaction could be undercut through the explicit path. That made the token mean "no
        // pinned writers", which is only the same as "no writers" if every writer uses a pin.
        auto during = space->Write(range.base, data);
        Check(!during.HasValue(), "an explicit write during a transaction must be refused");
        if (!during.HasValue()) {
            Check(during.GetError().category == ErrorCategory::Busy, "must report Busy");
        }

        auto writable = space->AcquirePinnedSpan(range, true);
        Check(!writable.HasValue(), "a new writable pin during a transaction must be refused");

        // Readers stay admitted: the transaction owner needs them, and they cannot invalidate
        // what the token asserts.
        auto readable = space->AcquirePinnedSpan(range, false);
        Check(readable.HasValue(), "a read-only pin must still be granted during a transaction");

        // The owner is not locked out of its own transaction.
        std::vector<std::byte> code(static_cast<std::size_t>(page), std::byte{0x90});
        Check(space->PublishCode(token.Value(), range, code).HasValue(),
              "the token holder must still be able to publish");
    });

    RunCase("M13d", "a read-only lease cannot be used to write", [] {
        auto space = MakeSpace();
        const std::uint64_t page = HostPageSize();
        auto range = GuestRange::Checked(space->ReservationBase(), page).Value();
        Check(space->Map(range, GuestPermission::Read | GuestPermission::Write).HasValue(),
              "map failed");

        const std::array<std::byte, 1> marker{std::byte{0x11}};
        Check(space->Write(range.base, marker).HasValue(), "seeding write failed");

        auto reader = space->AcquirePinnedSpan(GuestRange{range.base, 1}, /*writable=*/false);
        Check(reader.HasValue(), "read-only pin failed");
        if (!reader) return;

        Check(!reader.Value().Writable(), "a read-only lease must not report itself writable");
        // Bytes() is a const span, so `Bytes()[0] = ...` does not compile at all -- that is the
        // actual fix for P1-A, and it cannot be expressed as a runtime assertion. What is checkable
        // is that the writable view stays empty, so a caller reaching for it writes nothing rather
        // than writing through a lease it was not granted.
        Check(reader.Value().WritableBytes().empty(),
              "a read-only lease must not hand out a writable view");
        Check(reader.Value().Bytes().size() == 1, "the read view must still be usable");

        // The byte is unchanged: nothing above could have modified it.
        std::array<std::byte, 1> observed{};
        Check(space->Read(range.base, observed).HasValue(), "read-back failed");
        Check(observed[0] == std::byte{0x11}, "guest memory changed through a read-only lease");
    });

    RunCase("M13e", "publication entry points reject a token from another address space", [] {
        // Each space counts epochs independently, so two freshly quiesced spaces both hold epoch 1.
        // Checking only IsValid() and the numeric epoch therefore accepted a foreign token, which
        // the 2026-09-08 publication review reproduced against a live second transaction.
        auto a = MakeSpace();
        auto b = MakeSpace();
        const std::uint64_t page = HostPageSize();
        auto range = GuestRange::Checked(b->ReservationBase(), page).Value();
        Check(b->Map(range, GuestPermission::Read | GuestPermission::Write).HasValue(),
              "map failed");

        const std::array<std::byte, 1> marker{std::byte{0x11}};
        Check(b->Write(range.base, marker).HasValue(), "seeding write failed");

        auto token_a = a->Quiesce(1'000'000);
        auto token_b = b->Quiesce(1'000'000);
        Check(token_a.HasValue() && token_b.HasValue(), "quiesce failed");
        if (!token_a || !token_b) return;
        Check(token_a.Value().Epoch() == token_b.Value().Epoch(),
              "the two tokens should collide on epoch, or this case proves nothing");

        const std::uint64_t generation_before = b->CodeGeneration();
        std::vector<std::byte> code(static_cast<std::size_t>(page), std::byte{0x42});

        auto published = b->PublishCode(token_a.Value(), range, code);
        Check(!published.HasValue(), "PublishCode must refuse a foreign token");

        auto invalidated = b->InvalidateCode(token_a.Value(), range, InvalidationReason::HostWrite);
        Check(!invalidated.HasValue(), "InvalidateCode must refuse a foreign token");

        // A refusal must change nothing: not the bytes, not the generation.
        std::array<std::byte, 1> observed{};
        Check(b->Read(range.base, observed).HasValue(), "read-back failed");
        Check(observed[0] == std::byte{0x11}, "a refused publication still wrote guest memory");
        Check(b->CodeGeneration() == generation_before,
              "a refused publication still advanced the code generation");

        // The space's own token still works, so this is a provenance check rather than a
        // blanket refusal.
        Check(b->PublishCode(token_b.Value(), range, code).HasValue(),
              "the space's own token must still authorise publication");
    });

    RunCase("M13f", "a publication whose backend invalidation fails leaves nothing executable", [] {
        // The review's requirement for a failed publication: "if invalidation fails after the write,
        // at least keep it non-executable/non-resumable and report the error clearly". Reporting
        // success here would be the worst outcome -- the bytes are already new, so the old
        // translation would run against code that no longer exists.
        //
        // A failing sink is the only way to exercise this deterministically; a real backend refuses
        // only under conditions this suite cannot force (a thread mid-execution).
        class FailingSink final : public CodeInvalidationSink {
        public:
            std::string_view Name() const override {
                return "always-fails";
            }
            Status DiscardTranslations(GuestRange, InvalidationReason) override {
                ++calls;
                return MakeError(ErrorCategory::BackendFailure, "DiscardTranslations",
                                 "simulated backend invalidation failure");
            }
            int calls{};
        };

        auto space = MakeSpace();
        const std::uint64_t page = HostPageSize();
        auto range = GuestRange::Checked(space->ReservationBase(), page).Value();
        Check(space->Map(range, GuestPermission::Read | GuestPermission::Write).HasValue(),
              "map failed");
        // Executable *and* writable: PublishCode validates for Write, so a plain RX mapping would
        // be refused before the sink is ever consulted, and this case would prove nothing about
        // what happens when the backend fails. RWX is what makes the execute-revocation visible.
        auto rwx = space->Protect(range, GuestPermission::Read | GuestPermission::Write |
                                             GuestPermission::Execute);
        if (!rwx) {
            // A W^X host refuses RWX outright (M14a covers that). There is then no way to hold a
            // mapping that is executable *and* acceptable to PublishCode, so the execute-revocation
            // half of this case cannot be observed here. Say so rather than reporting a bare PASS:
            // a case that quietly checks nothing is exactly what the review objected to. Swan does
            // permit RWX, so the device run is what actually exercises this.
            std::printf("       (skipped: host refuses RWX, so execute revocation is not "
                        "observable; run on device)\n");
            return;
        }

        FailingSink sink;
        Check(space->SetCodeInvalidationSink(&sink).HasValue(), "registering the sink failed");

        // A second backend must not silently displace the first: its translations would then be
        // unreachable by any future invalidation.
        FailingSink other;
        Check(!space->SetCodeInvalidationSink(&other).HasValue(),
              "a second sink must be refused, not swapped in");

        Check(!space->HasPoisonedCode(), "nothing should be poisoned before the failure");

        auto token = space->Quiesce(1'000'000);
        Check(token.HasValue(), "quiesce failed");
        if (!token) return;

        std::vector<std::byte> code(static_cast<std::size_t>(page), std::byte{0x90});
        auto published = space->PublishCode(token.Value(), range, code);
        Check(!published.HasValue(),
              "PublishCode must fail when the backend cannot discard translations");
        Check(sink.calls == 1, "the sink should have been consulted exactly once");
        Check(space->HasPoisonedCode(), "a failed publication must mark the range poisoned");

        // The mapping was executable; after the failure it must not be.
        auto info = space->Query(range.base);
        Check(info.HasValue(), "query failed");
        if (info) {
            Check(!HasPermission(info.Value().permission, GuestPermission::Execute),
                  "a failed publication left the range executable");
        }

        // The generation must still move: the bytes did change, so anything holding the previous
        // generation must not keep comparing equal to it.
        Check(space->CodeGeneration() > 1, "a failed publication must still advance the generation");

        space->ClearCodeInvalidationSink(&sink);
    });

    RunCase("M13g", "a pin held on one host thread races unmap/protect on another", [] {
        // M13's criterion is a *race* between a pinned HLE span and another thread's
        // unmap/protect: "wait/Busy or a defined transaction cancellation; the span does not become
        // invalid while in use, and succeeds once released". Every other M13 sub-case is
        // single-threaded, so none of them can show that -- the 2026-09-08 review listed this as
        // still missing. Two threads contending on the same range is the only way to test it.
        constexpr int kRounds = 200;
        const std::uint64_t page = HostPageSize();

        std::atomic<int> refused_while_pinned{0};
        std::atomic<int> succeeded_after_release{0};
        std::atomic<int> span_corrupted{0};
        std::atomic<int> unexpected_error{0};

        for (int round = 0; round < kRounds; ++round) {
            auto space = MakeSpace();
            auto range = GuestRange::Checked(space->ReservationBase(), page).Value();
            if (!space->Map(range, GuestPermission::Read | GuestPermission::Write)) {
                Check(false, "map failed");
                return;
            }

            std::atomic<bool> go{false};
            std::atomic<bool> pin_acquired{false};
            std::atomic<bool> racer_done{false};

            // Holder: takes the pin, writes through it, then releases.
            std::thread holder([&] {
                while (!go.load(std::memory_order_acquire)) {
                }
                auto pin = space->AcquirePinnedSpan(range, /*writable=*/true);
                if (!pin) {
                    // Losing the acquire to a concurrent protect is legitimate; it is not a
                    // corrupted span.
                    pin_acquired.store(true, std::memory_order_release);
                    return;
                }
                auto bytes = pin.Value().WritableBytes();
                bytes[0] = std::byte{0xA5};
                pin_acquired.store(true, std::memory_order_release);

                // Keep the pin until the racer has actually attempted its operations. Spinning a
                // fixed number of iterations instead made the overlap a scheduling accident: on
                // Swan only 3 of 200 rounds contended, so the case passed largely without racing.
                // The span must stay valid the entire time it is held.
                while (!racer_done.load(std::memory_order_acquire)) {
                    bytes[0] = static_cast<std::byte>(0xA5);
                    if (bytes.size() != page) {
                        span_corrupted.fetch_add(1, std::memory_order_relaxed);
                        break;
                    }
                }
                if (bytes.size() != page) {
                    span_corrupted.fetch_add(1, std::memory_order_relaxed);
                }
            });

            // Racer: tries to unmap and protect the same range while the pin is held.
            std::thread racer([&] {
                while (!go.load(std::memory_order_acquire)) {
                }
                // Wait for the pin to exist, so this really is a contended attempt.
                while (!pin_acquired.load(std::memory_order_acquire)) {
                }
                auto unmap = space->Unmap(range);
                if (!unmap) {
                    const auto category = unmap.GetError().category;
                    if (category == ErrorCategory::Busy) {
                        refused_while_pinned.fetch_add(1, std::memory_order_relaxed);
                    } else {
                        unexpected_error.fetch_add(1, std::memory_order_relaxed);
                    }
                }
                auto protect = space->Protect(range, GuestPermission::Read);
                if (!protect && protect.GetError().category != ErrorCategory::Busy &&
                    protect.GetError().category != ErrorCategory::InvalidArgument) {
                    unexpected_error.fetch_add(1, std::memory_order_relaxed);
                }
                racer_done.store(true, std::memory_order_release);
            });

            go.store(true, std::memory_order_release);
            holder.join();
            racer.join();

            // Once nothing is pinned, the operation the race may have refused must now succeed.
            if (space->Counts().live_pins == 0) {
                if (space->Query(range.base).HasValue()) {
                    if (space->Unmap(range)) {
                        succeeded_after_release.fetch_add(1, std::memory_order_relaxed);
                    }
                } else {
                    // The racer's unmap won; that is a legal outcome of the race.
                    succeeded_after_release.fetch_add(1, std::memory_order_relaxed);
                }
            }
        }

        Check(span_corrupted.load() == 0,
              "a pinned span was invalidated while its holder was still using it");
        Check(unexpected_error.load() == 0,
              "a contended operation failed with something other than the defined refusal");
        const std::string reoperable =
            "the range must be operable again once every pin is released; got " +
            std::to_string(succeeded_after_release.load()) + "/" + std::to_string(kRounds);
        Check(succeeded_after_release.load() == kRounds, reoperable.c_str());
        // The racer waits for the pin, so every round must contend. A low count here would mean
        // the case is not testing what it claims.
        const std::string contended =
            "every round must actually contend; only " +
            std::to_string(refused_while_pinned.load()) + "/" + std::to_string(kRounds) + " did";
        Check(refused_while_pinned.load() == kRounds, contended.c_str());
    });

    RunCase("M12", "observers get conservatively widened write notifications", [] {
        auto space = MakeSpace();
        const std::uint64_t page = HostPageSize();
        auto range = GuestRange::Checked(space->ReservationBase(), page * 2).Value();
        Check(space->Map(range, GuestPermission::Read | GuestPermission::Write).HasValue(),
              "map failed");

        RecordingObserver gpu;
        space->AddObserver(&gpu);
        Check(space->Counts().observers == 1, "observer should be registered");

        // A small write in the middle of a host page.
        std::vector<std::byte> data(4, std::byte{0x33});
        const GuestAddress at{range.base.value + 100};
        Check(space->Write(at, data).HasValue(), "write failed");
        Check(gpu.ranges.size() == 1, "observer should have been notified exactly once");
        if (!gpu.ranges.empty()) {
            const auto& reported = gpu.ranges.front();
            // Widening is required; missing coverage is not acceptable.
            Check(reported.base.value <= at.value, "reported range must cover the write start");
            Check(reported.base.value + reported.size >= at.value + 4,
                  "reported range must cover the write end");
            Check(IsHostPageAligned(reported.base.value),
                  "reported range should start on a host page");
        }

        space->RemoveObserver(&gpu);
        Check(space->Counts().observers == 0, "observer should be removed");
        Check(space->Write(at, data).HasValue(), "write after removal failed");
        Check(gpu.ranges.size() == 1, "a removed observer must not be notified");
    });

    RunCase("M10", "aliases require a mapped primary range", [] {
        auto space = MakeSpace();
        const std::uint64_t page = HostPageSize();
        auto range = GuestRange::Checked(space->ReservationBase(), page).Value();

        const GuestAddress alias{space->ReservationBase().value + page * 4};
        Check(!space->RegisterAlias(range, alias).HasValue(),
              "alias of an unmapped range must be rejected");

        Check(space->Map(range, GuestPermission::Read | GuestPermission::Write).HasValue(),
              "map failed");
        Check(space->RegisterAlias(range, alias).HasValue(), "alias registration failed");
        Check(space->Counts().aliases == 1, "alias should be counted");

        // Invalidating through the primary bumps the space-wide code
        // generation, which is what covers every executable alias.
        auto token = space->Quiesce(1'000'000);
        Check(token.HasValue(), "quiesce failed");
        if (!token) return;
        const std::uint64_t before = space->CodeGeneration();
        Check(space->InvalidateCode(token.Value(), range, InvalidationReason::AliasUpdate)
                  .HasValue(), "invalidate failed");
        Check(space->CodeGeneration() > before, "invalidate must bump the code generation");
    });
}

// --- handle lifetime --------------------------------------------------------

void TestHandleLifetime() {
    RunCase("L01a", "a token outliving its address space releases safely", [] {
        // Regression: the first version stored a raw owner pointer, so an
        // orphaned token called ReleaseQuiescence on freed memory and aborted
        // with "mutex lock failed". Releasing late must be a no-op instead.
        QuiescenceToken orphan;
        {
            auto space = MakeSpace();
            auto token = space->Quiesce(1'000'000);
            Check(token.HasValue(), "quiesce failed");
            if (!token) return;
            orphan = std::move(token).Value();
            Check(orphan.OwnerAlive(), "owner should be alive while the space exists");
        }
        Check(!orphan.OwnerAlive(), "owner must read as gone once the space is destroyed");
        Check(orphan.Epoch() != 0, "the epoch value itself is still readable");
        // The destructor runs at scope exit; reaching the next case is the
        // real assertion.
    });

    RunCase("L01b", "a pinned span outliving its address space releases safely", [] {
        PinnedSpan orphan;
        {
            auto space = MakeSpace();
            const std::uint64_t page = HostPageSize();
            auto range = GuestRange::Checked(space->ReservationBase(), page).Value();
            Check(space->Map(range, GuestPermission::Read | GuestPermission::Write).HasValue(),
                  "map failed");
            auto pin = space->AcquirePinnedSpan(range, true);
            Check(pin.HasValue(), "pin failed");
            if (!pin) return;
            orphan = std::move(pin).Value();
            Check(orphan.OwnerAlive(), "owner should be alive while the space exists");
        }
        Check(!orphan.OwnerAlive(), "owner must read as gone once the space is destroyed");
        // Deliberately not touching Bytes() here: the mapping is unmapped, so
        // reading it would be a genuine use-after-free rather than a test.
    });

    RunCase("L01c", "moved-from handles do not double release", [] {
        auto space = MakeSpace();
        auto first = space->Quiesce(1'000'000);
        Check(first.HasValue(), "quiesce failed");
        if (!first) return;

        QuiescenceToken moved = std::move(first).Value();
        Check(moved.IsValid(), "moved-to token should be valid");
        {
            QuiescenceToken second = std::move(moved);
            Check(second.IsValid(), "second move target should be valid");
            Check(!moved.IsValid(), "moved-from token must be invalidated");
        }
        // The single live token was released exactly once, so a new
        // transaction can start.
        Check(space->Quiesce(1'000'000).HasValue(),
              "a new quiesce must succeed after the previous token was released");
    });
}

// --- unsupported modes ------------------------------------------------------
void TestUnsupportedModes() {
    RunCase("M08b", "TransparentSMC and SoftwareCallbacks are refused at init", [] {
        AddressSpaceConfig transparent{};
        transparent.smc_mode = SmcMode::TransparentSmc;
        auto a = GuestAddressSpace::Create(transparent);
        Check(!a.HasValue(), "TransparentSMC must be refused (DEC-05)");
        if (!a.HasValue()) {
            Check(a.GetError().category == ErrorCategory::Unsupported,
                  "TransparentSMC refusal must be Unsupported");
        }

        AddressSpaceConfig callbacks{};
        callbacks.memory_mode = MemoryMode::SoftwareCallbacks;
        auto b = GuestAddressSpace::Create(callbacks);
        Check(!b.HasValue(), "SoftwareCallbacks must be refused");
        if (!b.HasValue()) {
            Check(b.GetError().category == ErrorCategory::UnsupportedMemoryMode,
                  "refusal must be UnsupportedMemoryMode");
        }
    });
}

// --- execution vocabulary ---------------------------------------------------

void TestStopSemantics() {
    RunCase("T03a", "a fault is never masked by a simultaneous pause", [] {
        // API contract §5. This is pure priority logic, so it is meaningful on
        // the host even with no backend attached.
        auto both = BitOf(StopReason::GuestFault) | BitOf(StopReason::PauseRequested);
        Check(SelectPrimaryReason(both) == StopReason::GuestFault,
              "fault must outrank a concurrent pause");

        auto cancel_pause = BitOf(StopReason::Cancelled) | BitOf(StopReason::PauseRequested);
        Check(SelectPrimaryReason(cancel_pause) == StopReason::Cancelled,
              "cancel must outrank pause");

        auto failure = BitOf(StopReason::BackendFailure) | BitOf(StopReason::Cancelled);
        Check(SelectPrimaryReason(failure) == StopReason::BackendFailure,
              "backend failure must outrank cancel");

        // Pending bits must survive selection so nothing is silently dropped.
        Check(Contains(both, StopReason::PauseRequested),
              "the non-primary reason must remain visible in the pending set");

        Check(SelectPrimaryReason(BitOf(StopReason::Returned)) == StopReason::Returned,
              "a lone Returned stays Returned");
        Check(SelectPrimaryReason(StopReasonBits::None) == StopReason::Returned,
              "an empty set defaults to Returned");
    });

    RunCase("D03a", "snapshot validity distinguishes unknown from zero", [] {
        CpuSnapshot snapshot{};
        snapshot.kind = SnapshotKind::AsyncJitStop;
        // An async JIT stop leaves guest values in host registers. Reporting
        // an all-zero RegisterFile as valid is exactly what D03 forbids.
        Check(snapshot.registers.validity == RegisterValidity::None,
              "an uncaptured snapshot must claim no valid fields");
        Check(!HasAll(snapshot.registers.validity, RegisterValidity::Gpr),
              "GPRs must not read as valid when they were never captured");

        snapshot.registers.validity |= RegisterValidity::Gpr;
        snapshot.registers.Set(Gpr::Rax, 0);
        Check(HasAll(snapshot.registers.validity, RegisterValidity::Gpr),
              "an explicitly captured zero must read as valid");
        Check(snapshot.registers.Get(Gpr::Rax) == 0, "captured value mismatch");

        // RSP has exactly one authoritative slot.
        snapshot.registers.Set(Gpr::Rsp, 0xDEAD0000);
        Check(snapshot.registers.Rsp() == 0xDEAD0000, "Rsp() must read the single RSP slot");
    });

    RunCase("D03b", "public GPR numbering matches the x86-64 encoding", [] {
        // A consumer cross-checking a disassembly must not have to translate.
        Check(Index(Gpr::Rax) == 0 && Index(Gpr::Rcx) == 1 && Index(Gpr::Rdx) == 2 &&
                  Index(Gpr::Rbx) == 3,
              "low GPR numbering mismatch");
        Check(Index(Gpr::Rsp) == 4 && Index(Gpr::Rbp) == 5 && Index(Gpr::Rsi) == 6 &&
                  Index(Gpr::Rdi) == 7,
              "mid GPR numbering mismatch");
        Check(Index(Gpr::R15) == 15, "R15 must be 15");
        Check(ToString(Gpr::Rsp) == "rsp", "GPR name mismatch");
    });
}

} // namespace

int main() {
    std::printf("shadPS4 guest CPU API contract tests\n");
    std::printf("host page size: %llu bytes\n",
                static_cast<unsigned long long>(HostPageSize()));
    std::printf("guest ABI page: %llu, code index granularity: %llu\n\n",
                static_cast<unsigned long long>(kGuestAbiPageSize),
                static_cast<unsigned long long>(kCodeIndexGranularity));

    TestHostPageDiscovery();
    TestMappingLifecycle();
    TestTransactions();
    TestHandleLifetime();
    TestUnsupportedModes();
    TestStopSemantics();

    const auto failed = static_cast<std::size_t>(
        std::count_if(g_results.begin(), g_results.end(),
                      [](const CaseResult& r) { return !r.passed; }));
    std::printf("\n%zu/%zu passed\n", g_results.size() - failed, g_results.size());

    if (failed != 0) {
        std::printf("\nFAILURES:\n");
        for (const auto& result : g_results) {
            if (!result.passed) {
                std::printf("  %s %s: %s\n", result.id.c_str(), result.name.c_str(),
                            result.detail.c_str());
            }
        }
    }
    // Scope reminder so a passing run is never mistaken for device validation.
    std::printf("\nSCOPE: host-only. Proves page arithmetic and POSIX mapping semantics\n");
    std::printf("       at the real host page size. Proves nothing about bionic, ART or FEX.\n");
    return failed == 0 ? 0 : 1;
}
