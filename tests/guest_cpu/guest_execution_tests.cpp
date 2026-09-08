// SPDX-FileCopyrightText: Copyright 2026 shadPS4 Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later
//
// Guest execution through the public CPU API, using assembler-generated fixtures.
//
// The 2026-09-08 review found that the previous version hand-encoded its fixtures and got one
// wrong: `49 01 C8` is `add r8, rcx`, not `add r8, r9`, so C01 could not have passed against a
// perfect backend and the failure was misread as a backend defect. Fixtures now come from
// tests/guest_cpu/fixtures/guest_fixtures.S via scripts/android/generate-guest-fixtures, which
// disassembles the extracted bytes back and records the listing next to them.
//
// Each fixture runs in its own thread with only the registers it needs seeded, and the harness
// reports the guest bytes and the recorded disassembly on failure so a wrong result can be told
// apart from a wrong fixture without re-deriving the encoding.

#include <cinttypes>
#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

#include <unistd.h>

#include "core/guest_cpu/api/address_space.h"
#include "core/guest_cpu/api/context.h"

#include "guest_fixtures.h"

namespace {

using namespace Core::GuestCpu;

int g_failures = 0;
int g_checks = 0;

void Check(const char* id, const char* name, bool condition, const std::string& detail = {}) {
    ++g_checks;
    printf("[%-6s] %-58s %s", id, name, condition ? "PASS" : "FAIL");
    if (!detail.empty()) {
        printf(" -- %s", detail.c_str());
    }
    printf("\n");
    fflush(stdout);
    if (!condition) {
        ++g_failures;
    }
}

std::string Hex(std::uint64_t value) {
    char buffer[32];
    std::snprintf(buffer, sizeof(buffer), "0x%" PRIx64, value);
    return buffer;
}

void CheckU64(const char* id, const char* name, std::uint64_t actual, std::uint64_t expected) {
    const bool ok = actual == expected;
    Check(id, name, ok, ok ? std::string{} : "expected " + Hex(expected) + ", got " + Hex(actual));
}

// Layout inside the guest reservation. Code and stack are separate mappings so an overrun of one
// cannot silently land in the other.
constexpr std::uint64_t kCodeOffset = 0x10000;
constexpr std::uint64_t kStackOffset = 0x20000;
constexpr std::uint64_t kMappingSize = 0x4000;

struct Harness final {
    std::unique_ptr<GuestAddressSpace> space;
    std::unique_ptr<CpuContext> context;
    std::uint64_t code_base{};
    std::uint64_t stack_base{};
    std::uint64_t stack_top{};
    std::uint64_t return_gate{};
};

const Fixtures::Fixture* FindFixture(std::string_view name) {
    for (const auto& fixture : Fixtures::kAll) {
        if (fixture.name == name) {
            return &fixture;
        }
    }
    return nullptr;
}

// Writes a fixture into guest memory, patches the return-gate address, publishes RX, and discards
// any translation of whatever was there before.
bool LoadFixture(Harness& harness, const Fixtures::Fixture& fixture, std::string& error) {
    const GuestRange range{GuestAddress{harness.code_base}, kMappingSize};

    auto writable = harness.space->Protect(range, GuestPermission::Read | GuestPermission::Write);
    if (!writable) {
        error = "Protect(RW): " + Describe(writable.GetError());
        return false;
    }

    {
        auto pin = harness.space->AcquirePinnedSpan(range, /*writable=*/true);
        if (!pin) {
            error = "AcquirePinnedSpan: " + Describe(pin.GetError());
            return false;
        }
        auto bytes = pin.Value().Bytes();
        if (fixture.bytes.size() > bytes.size()) {
            error = "fixture is larger than the code mapping";
            return false;
        }
        // Zero the rest so a shorter fixture cannot run into the previous one's tail.
        std::memset(bytes.data(), 0, bytes.size());
        std::memcpy(bytes.data(), fixture.bytes.data(), fixture.bytes.size());

        if (fixture.gate_offset >= 0) {
            // Patch the placeholder the assembler emitted with the gate the backend actually chose.
            std::memcpy(bytes.data() + fixture.gate_offset, &harness.return_gate,
                        sizeof(harness.return_gate));
        }
        // Release the pin before changing permissions: Protect must not run while a writer holds a
        // span, and holding it here would mask that ordering requirement.
    }

    auto executable =
        harness.space->Protect(range, GuestPermission::Read | GuestPermission::Execute);
    if (!executable) {
        error = "Protect(RX): " + Describe(executable.GetError());
        return false;
    }

    auto quiesced = harness.space->Quiesce(/*timeout_ns=*/1'000'000'000);
    if (!quiesced) {
        error = "Quiesce: " + Describe(quiesced.GetError());
        return false;
    }
    auto invalidated = harness.context->InvalidateCode(quiesced.Value(), range,
                                                       InvalidationReason::HostWrite);
    if (!invalidated) {
        error = "InvalidateCode: " + Describe(invalidated.GetError());
        return false;
    }
    return true;
}

// Zeroes the guest stack so a stale value cannot look like a fresh store.
bool ResetStack(Harness& harness, std::string& error) {
    auto pin = harness.space->AcquirePinnedSpan(
        GuestRange{GuestAddress{harness.stack_base}, kMappingSize}, /*writable=*/true);
    if (!pin) {
        error = "pin stack: " + Describe(pin.GetError());
        return false;
    }
    std::memset(pin.Value().Bytes().data(), 0, pin.Value().Bytes().size());
    return true;
}

// Reads one 8-byte guest slot. Returns false when the read itself failed, so "could not read" is
// distinguishable from "read zero".
bool ReadGuestU64(Harness& harness, std::uint64_t address, std::uint64_t& out) {
    auto pin = harness.space->AcquirePinnedSpan(GuestRange{GuestAddress{address}, 8},
                                                /*writable=*/false);
    if (!pin) {
        return false;
    }
    std::memcpy(&out, pin.Value().Bytes().data(), sizeof(out));
    return true;
}

struct RunOutcome final {
    bool ok{false};
    RunResult result{};
    std::string error;
};

template <typename SetupFn>
RunOutcome RunFixture(Harness& harness, const Fixtures::Fixture& fixture, SetupFn&& setup) {
    RunOutcome outcome{};

    if (!LoadFixture(harness, fixture, outcome.error)) {
        return outcome;
    }
    if (!ResetStack(harness, outcome.error)) {
        return outcome;
    }

    ThreadInit init{};
    init.entry_rip = GuestCodeAddress{harness.code_base};
    init.initial_rsp = GuestAddress{harness.stack_top};
    init.guest_tid = 1;
    setup(init.initial_state);

    auto thread = harness.context->CreateThread(init);
    if (!thread) {
        outcome.error = "CreateThread: " + Describe(thread.GetError());
        return outcome;
    }

    auto result = harness.context->Run(thread.Value(), RunOptions{});
    if (!result) {
        outcome.error = "Run: " + Describe(result.GetError());
        (void)harness.context->DestroyThread(thread.Value());
        return outcome;
    }
    outcome.result = result.Value();

    auto destroyed = harness.context->DestroyThread(thread.Value());
    if (!destroyed) {
        outcome.error = "DestroyThread: " + Describe(destroyed.GetError());
        return outcome;
    }

    outcome.ok = true;
    return outcome;
}

// Prints what the fixture actually is, so a failure can be attributed to the backend or to the
// fixture without anyone re-deriving the encoding by hand.
void ReportFixture(const Fixtures::Fixture& fixture) {
    printf("  fixture %.*s (%zu bytes):\n", static_cast<int>(fixture.name.size()),
           fixture.name.data(), fixture.bytes.size());
    printf("    %.*s\n", static_cast<int>(fixture.disassembly.size()), fixture.disassembly.data());
    fflush(stdout);
}

bool Require(const char* id, const RunOutcome& outcome, const Fixtures::Fixture& fixture) {
    if (!outcome.ok) {
        Check(id, "run fixture", false, outcome.error);
        ReportFixture(fixture);
        return false;
    }
    return true;
}

// --- C01: integer arithmetic -------------------------------------------------------------------
void TestIntegerArithmetic(Harness& harness) {
    const auto* fixture = FindFixture("integer");
    if (fixture == nullptr) {
        Check("G01", "integer fixture is present", false);
        return;
    }

    auto outcome = RunFixture(harness, *fixture, [](RegisterPatch& patch) {
        patch.fields = RegisterValidity::Gpr;
        patch.gpr_mask = (1u << Index(Gpr::R8)) | (1u << Index(Gpr::R9));
        patch.values.Set(Gpr::R8, 0x1000);
        patch.values.Set(Gpr::R9, 0x234);
    });
    if (!Require("G01", outcome, *fixture)) {
        return;
    }

    const auto& regs = outcome.result.snapshot.registers;
    const bool returned = outcome.result.primary_reason == StopReason::Returned;
    Check("G01a", "integer fixture returned through the gate", returned,
          returned ? std::string{} : std::string{ToString(outcome.result.primary_reason)});
    CheckU64("G01b", "add r8, r9 produced the sum", regs.Get(Gpr::R8), 0x1234);
    CheckU64("G01c", "mov r10, r8 then sub 5", regs.Get(Gpr::R10), 0x1234 - 5);
    CheckU64("G01d", "xor r11, r11 cleared the register", regs.Get(Gpr::R11), 0);
    Check("G01e", "snapshot reports GPRs as valid", HasAll(regs.validity, RegisterValidity::Gpr));
    Check("G01f", "snapshot kind is a safe point",
          outcome.result.snapshot.kind == SnapshotKind::SafePoint);

    if (g_failures != 0) {
        ReportFixture(*fixture);
    }
}

// --- C01: branches, both directions ------------------------------------------------------------
void TestBranches(Harness& harness) {
    const auto* fixture = FindFixture("branch");
    if (fixture == nullptr) {
        Check("G02", "branch fixture is present", false);
        return;
    }

    auto equal = RunFixture(harness, *fixture, [](RegisterPatch& patch) {
        patch.fields = RegisterValidity::Gpr;
        patch.gpr_mask = (1u << Index(Gpr::Rdi)) | (1u << Index(Gpr::Rsi));
        patch.values.Set(Gpr::Rdi, 42);
        patch.values.Set(Gpr::Rsi, 42);
    });
    if (Require("G02", equal, *fixture)) {
        CheckU64("G02a", "not-taken branch fell through to the mov",
                 equal.result.snapshot.registers.Get(Gpr::Rax), 1);
    }

    auto unequal = RunFixture(harness, *fixture, [](RegisterPatch& patch) {
        patch.fields = RegisterValidity::Gpr;
        patch.gpr_mask = (1u << Index(Gpr::Rdi)) | (1u << Index(Gpr::Rsi));
        patch.values.Set(Gpr::Rdi, 42);
        patch.values.Set(Gpr::Rsi, 43);
    });
    if (Require("G02", unequal, *fixture)) {
        CheckU64("G02b", "taken branch skipped the mov",
                 unequal.result.snapshot.registers.Get(Gpr::Rax), 0);
    }
}

// --- C01: load/store through the guest stack -----------------------------------------------------
void TestLoadStore(Harness& harness) {
    const auto* fixture = FindFixture("loadstore");
    if (fixture == nullptr) {
        Check("G03", "loadstore fixture is present", false);
        return;
    }

    constexpr std::uint64_t kFirst = 0xAAAA'BBBB'CCCC'DDDDull;
    constexpr std::uint64_t kSecond = 0x1111'2222'3333'4444ull;

    auto outcome = RunFixture(harness, *fixture, [](RegisterPatch& patch) {
        patch.fields = RegisterValidity::Gpr;
        patch.gpr_mask = (1u << Index(Gpr::Rdi)) | (1u << Index(Gpr::Rsi));
        patch.values.Set(Gpr::Rdi, kFirst);
        patch.values.Set(Gpr::Rsi, kSecond);
    });
    if (!Require("G03", outcome, *fixture)) {
        return;
    }

    const auto& regs = outcome.result.snapshot.registers;
    CheckU64("G03a", "pop rax read back the second push", regs.Get(Gpr::Rax), kSecond);
    CheckU64("G03b", "pop rcx read back the first push", regs.Get(Gpr::Rcx), kFirst);
    CheckU64("G03c", "stack pointer is balanced after push/pop", regs.Rsp(), harness.stack_top);
}

// --- C01: a store that must remain visible in guest memory ---------------------------------------
void TestStoreMemory(Harness& harness) {
    const auto* fixture = FindFixture("store_memory");
    if (fixture == nullptr) {
        Check("G07", "store_memory fixture is present", false);
        return;
    }

    constexpr std::uint64_t kLow = 0x0BAD'C0DE'0BAD'C0DEull;
    constexpr std::uint64_t kHigh = 0xFEED'FACE'FEED'FACEull;

    auto outcome = RunFixture(harness, *fixture, [](RegisterPatch& patch) {
        patch.fields = RegisterValidity::Gpr;
        patch.gpr_mask = (1u << Index(Gpr::Rdi)) | (1u << Index(Gpr::Rsi));
        patch.values.Set(Gpr::Rdi, kLow);
        patch.values.Set(Gpr::Rsi, kHigh);
    });
    if (!Require("G07", outcome, *fixture)) {
        return;
    }

    // Checked per slot rather than as an OR across both, so a partial write cannot pass. The review
    // called this out on the previous version.
    std::uint64_t slot_minus_16 = 0;
    std::uint64_t slot_minus_8 = 0;
    const bool read_low = ReadGuestU64(harness, harness.stack_top - 16, slot_minus_16);
    const bool read_high = ReadGuestU64(harness, harness.stack_top - 8, slot_minus_8);

    Check("G07a", "guest memory below rsp is readable", read_low && read_high);
    if (read_low) {
        CheckU64("G07b", "store to [rsp-16] landed in guest memory", slot_minus_16, kLow);
    }
    if (read_high) {
        CheckU64("G07c", "store to [rsp-8] landed in guest memory", slot_minus_8, kHigh);
    }

    // Read the same slots straight through the host pointer, bypassing the address space entirely.
    // This separates "the guest never wrote" from "the guest wrote somewhere the API does not read
    // back", which the pinned-span read alone cannot distinguish.
    const auto* raw = reinterpret_cast<const volatile std::uint64_t*>(harness.stack_top);
    const std::uint64_t direct_low = raw[-2];
    const std::uint64_t direct_high = raw[-1];
    char detail[192];
    std::snprintf(detail, sizeof(detail),
                  "direct[rsp-16]=%s direct[rsp-8]=%s (pinned read gave %s / %s)",
                  Hex(direct_low).c_str(), Hex(direct_high).c_str(),
                  Hex(slot_minus_16).c_str(), Hex(slot_minus_8).c_str());
    Check("G07d", "direct host read agrees with the pinned-span read",
          direct_low == slot_minus_16 && direct_high == slot_minus_8, detail);

    // What the guest itself read back from those slots. If these are correct while the host reads
    // are zero, the store and load are consistent with each other but invisible outside the guest;
    // if these are also zero, the store never happened at all.
    const auto& regs = outcome.result.snapshot.registers;
    CheckU64("G07e", "guest read back its own store to [rsp-16]", regs.Get(Gpr::Rax), kLow);
    CheckU64("G07f", "guest read back its own store to [rsp-8]", regs.Get(Gpr::Rcx), kHigh);
}

// --- C02: SSE2 -----------------------------------------------------------------------------------
void TestSse2(Harness& harness) {
    const auto* fixture = FindFixture("sse2");
    if (fixture == nullptr) {
        Check("G04", "sse2 fixture is present", false);
        return;
    }

    constexpr std::uint64_t kLeft = 0x0000'0001'0000'0002ull;
    constexpr std::uint64_t kRight = 0x0000'0010'0000'0020ull;
    // paddq adds the full 64-bit lane, and neither lane carries into the other here.
    constexpr std::uint64_t kExpected = kLeft + kRight;

    auto outcome = RunFixture(harness, *fixture, [](RegisterPatch& patch) {
        patch.fields = RegisterValidity::Gpr;
        patch.gpr_mask = (1u << Index(Gpr::Rdi)) | (1u << Index(Gpr::Rsi));
        patch.values.Set(Gpr::Rdi, kLeft);
        patch.values.Set(Gpr::Rsi, kRight);
    });
    if (!Require("G04", outcome, *fixture)) {
        return;
    }

    const auto& regs = outcome.result.snapshot.registers;
    CheckU64("G04a", "paddq result moved back to a GPR", regs.Get(Gpr::Rax), kExpected);
    CheckU64("G04b", "xmm0 low half holds the packed sum", regs.xmm[0].low, kExpected);
    Check("G04c", "snapshot reports XMM as valid", HasAll(regs.validity, RegisterValidity::Xmm));
}

// --- D05: a HLT outside the gate is not a normal return -------------------------------------------
void TestUnregisteredHlt(Harness& harness) {
    const auto* fixture = FindFixture("bare_hlt");
    if (fixture == nullptr) {
        Check("G05", "bare_hlt fixture is present", false);
        return;
    }

    auto outcome = RunFixture(harness, *fixture, [](RegisterPatch&) {});
    if (!Require("G05", outcome, *fixture)) {
        return;
    }

    const auto reason = outcome.result.primary_reason;
    Check("G05a", "a HLT outside the gate is not reported as Returned",
          reason != StopReason::Returned, std::string{ToString(reason)});
    Check("G05b", "the stop is reported as a guest fault", reason == StopReason::GuestFault);
    Check("G05c", "fault carries a guest RIP",
          outcome.result.fault.has_value() && outcome.result.fault->guest_rip.has_value());
    Check("G05d", "snapshot kind is Faulted, not SafePoint",
          outcome.result.snapshot.kind == SnapshotKind::Faulted);
}

// Exercise the two cache lifetimes separately: FEX's shared code buffers outlive
// all threads, while each live thread also has its own lookup/call-return cache.
void TestCodeInvalidation(Harness& harness) {
    const auto* a = FindFixture("constant_a");
    const auto* b = FindFixture("constant_b");
    if (!a || !b) {
        Check("G08a", "invalidation fixtures are present", false);
        return;
    }

    bool recreated_ok = true;
    std::string error;
    for (unsigned epoch = 0; epoch < 100; ++epoch) {
        const bool use_a = (epoch % 2) == 0;
        auto outcome = RunFixture(harness, use_a ? *a : *b, [](RegisterPatch&) {});
        const auto expected = use_a ? 17u : 34u;
        if (!outcome.ok || outcome.result.primary_reason != StopReason::Returned ||
            outcome.result.snapshot.registers.Get(Gpr::Rax) != expected ||
            harness.context->LiveThreadCount() != 0) {
            recreated_ok = false;
            error = "epoch " + std::to_string(epoch) + ": " + outcome.error;
            break;
        }
    }
    Check("G08a", "100 same-VA publications with zero live threads between runs",
          recreated_ok, error);

    if (!LoadFixture(harness, *a, error)) {
        Check("G08b", "publish code for two stopped cache owners", false, error);
        return;
    }
    ThreadInit init{};
    init.entry_rip = GuestCodeAddress{harness.code_base};
    init.initial_rsp = GuestAddress{harness.stack_top};
    auto first = harness.context->CreateThread(init);
    auto second = harness.context->CreateThread(init);
    if (!first || !second) {
        Check("G08b", "create two cache owners", false);
        if (first) (void)harness.context->DestroyThread(first.Value());
        if (second) (void)harness.context->DestroyThread(second.Value());
        return;
    }
    bool live_ok = true;
    const ThreadHandle handles[]{first.Value(), second.Value()};
    for (unsigned epoch = 0; epoch < 100 && live_ok; ++epoch) {
        const bool use_a = (epoch % 2) == 0;
        if (!LoadFixture(harness, use_a ? *a : *b, error)) {
            live_ok = false;
            break;
        }
        for (auto handle : handles) {
            auto stopped = harness.context->ReadRegisters(handle);
            if (!stopped || (epoch != 0 &&
                stopped.Value().registers.Get(Gpr::Rax) != (use_a ? 34u : 17u))) {
                live_ok = false;
                error = "invalidation changed stopped registers";
                break;
            }
            RegisterPatch patch{};
            patch.fields = RegisterValidity::Rip;
            patch.values.rip = harness.code_base;
            auto reset = harness.context->WriteRegisters(handle, patch, stopped.Value().stop_epoch);
            if (!reset) {
                live_ok = false;
                error = Describe(reset.GetError());
                break;
            }
            auto run = harness.context->Run(handle, RunOptions{});
            if (!run || run.Value().primary_reason != StopReason::Returned ||
                run.Value().snapshot.registers.Get(Gpr::Rax) != (use_a ? 17u : 34u)) {
                live_ok = false;
                error = "stale translation at epoch " + std::to_string(epoch);
                break;
            }
        }
    }
    Check("G08b", "100 publications invalidate both stopped threads without changing state",
          live_ok, error);
    auto destroy_first = harness.context->DestroyThread(first.Value());
    auto destroy_second = harness.context->DestroyThread(second.Value());
    Check("G08c", "cache regression leaves no guest threads",
          destroy_first && destroy_second && harness.context->LiveThreadCount() == 0);
}

// --- contract checks that need no execution -------------------------------------------------------
void TestContracts(Harness& harness) {
    const auto caps = harness.context->Capabilities();
    Check("G06a", "capabilities declare base integer support",
          Contains(caps.features, GuestFeature::BaseInteger));
    Check("G06b", "capabilities declare SSE2 support", Contains(caps.features, GuestFeature::Sse2));
    Check("G06c", "capabilities do not claim AVX", !Contains(caps.features, GuestFeature::Avx));
    Check("G06d", "capabilities report the injected host page size",
          caps.host_page_size == static_cast<std::uint64_t>(::sysconf(_SC_PAGESIZE)));

    auto second = CreateContext(CpuConfig{}, *harness.space);
    Check("G06e", "a second concurrent context is refused with AlreadyActive",
          !second && second.Category() == ErrorCategory::AlreadyActive);

    ThreadInit bad{};
    bad.entry_rip = GuestCodeAddress{harness.space->ReservationBase().value + 0x7F00'0000};
    bad.initial_rsp = GuestAddress{harness.stack_top};
    auto rejected = harness.context->CreateThread(bad);
    Check("G06f", "an unmapped entry_rip is refused",
          !rejected && rejected.Category() == ErrorCategory::InvalidArgument);

    const auto* stub = FindFixture("return_only");
    if (stub == nullptr) {
        Check("G06", "return_only fixture is present", false);
        return;
    }
    std::string error;
    if (!LoadFixture(harness, *stub, error)) {
        Check("G06g", "publish a stub fixture for the handle checks", false, error);
        return;
    }

    ThreadInit ok{};
    ok.entry_rip = GuestCodeAddress{harness.code_base};
    ok.initial_rsp = GuestAddress{harness.stack_top};
    auto thread = harness.context->CreateThread(ok);
    if (!thread) {
        Check("G06g", "create a thread for the handle checks", false,
              Describe(thread.GetError()));
        return;
    }

    auto stepped = harness.context->Step(thread.Value(), StepOptions{});
    Check("G06g", "Step is refused with Unsupported, not silently run as a block",
          !stepped && stepped.Category() == ErrorCategory::Unsupported);

    RegisterPatch patch{};
    patch.fields = RegisterValidity::Gpr;
    patch.gpr_mask = 1u << Index(Gpr::Rax);
    patch.values.Set(Gpr::Rax, 1);
    auto stale = harness.context->WriteRegisters(thread.Value(), patch, /*stop_epoch=*/9999);
    Check("G06h", "WriteRegisters refuses a stale stop epoch",
          !stale && stale.Category() == ErrorCategory::StaleEpoch);

    (void)harness.context->DestroyThread(thread.Value());

    auto after = harness.context->ReadRegisters(thread.Value());
    Check("G06i", "a destroyed handle is refused with InvalidHandle",
          !after && after.Category() == ErrorCategory::InvalidHandle);

    // A token from a different address space is structurally valid, so IsValid() alone would
    // accept it as authorisation to discard translations for a space it says nothing about.
    {
        AddressSpaceConfig other_config{};
        other_config.reservation_size = std::uint64_t{1} << 24;
        other_config.max_address = QueryBackendCapabilities().max_guest_address;
        auto other = GuestAddressSpace::Create(other_config);
        if (!other) {
            Check("G06j", "create a second address space for the token identity check", false,
                  Describe(other.GetError()));
            return;
        }
        auto foreign = other.Value()->Quiesce(/*timeout_ns=*/1'000'000);
        if (!foreign) {
            Check("G06j", "quiesce the second address space", false,
                  Describe(foreign.GetError()));
            return;
        }
        auto refused = harness.context->InvalidateCode(
            foreign.Value(), GuestRange{GuestAddress{harness.code_base}, kMappingSize},
            InvalidationReason::HostWrite);
        Check("G06j", "InvalidateCode refuses a token from another address space",
              !refused && refused.Category() == ErrorCategory::InvalidArgument,
              refused ? std::string{"it was accepted"} : Describe(refused.GetError()));
    }
}

} // namespace

int main() {
    printf("guest execution through the public CPU API\n");
    printf("host page size: %ld\n", ::sysconf(_SC_PAGESIZE));
    printf("fixtures: %zu, generated from tests/guest_cpu/fixtures/guest_fixtures.S\n\n",
           std::size(Fixtures::kAll));

    Harness harness{};

    AddressSpaceConfig space_config{};
    space_config.reservation_size = std::uint64_t{1} << 28;
    space_config.max_address = QueryBackendCapabilities().max_guest_address;
    auto space = GuestAddressSpace::Create(space_config);
    if (!space) {
        printf("FAILED: could not create the guest address space: %s\n",
               Describe(space.GetError()).c_str());
        return 1;
    }
    harness.space = std::move(space).Value();

    const std::uint64_t base = harness.space->ReservationBase().value;
    harness.code_base = base + kCodeOffset;
    harness.stack_base = base + kStackOffset;
    harness.stack_top = harness.stack_base + kMappingSize - 16;

    auto code_map = harness.space->Map(GuestRange{GuestAddress{harness.code_base}, kMappingSize},
                                       GuestPermission::Read | GuestPermission::Write);
    if (!code_map) {
        printf("FAILED: could not map guest code: %s\n", Describe(code_map.GetError()).c_str());
        return 1;
    }
    auto stack_map = harness.space->Map(GuestRange{GuestAddress{harness.stack_base}, kMappingSize},
                                        GuestPermission::Read | GuestPermission::Write);
    if (!stack_map) {
        printf("FAILED: could not map guest stack: %s\n", Describe(stack_map.GetError()).c_str());
        return 1;
    }

    auto context = CreateContext(CpuConfig{}, *harness.space);
    if (!context) {
        printf("FAILED: could not create the CPU context: %s\n",
               Describe(context.GetError()).c_str());
        return 1;
    }
    harness.context = std::move(context).Value();

    harness.return_gate = harness.context->Capabilities().return_gate_address;
    if (harness.return_gate == 0) {
        printf("FAILED: the backend did not report a return gate address\n");
        return 1;
    }

    printf("guest reservation base: 0x%" PRIx64 "\n", base);
    printf("code at 0x%" PRIx64 ", stack top 0x%" PRIx64 "\n", harness.code_base,
           harness.stack_top);
    printf("return gate at 0x%" PRIx64 "\n\n", harness.return_gate);

    TestIntegerArithmetic(harness);
    TestBranches(harness);
    TestLoadStore(harness);
    TestStoreMemory(harness);
    TestSse2(harness);
    TestUnregisteredHlt(harness);
    TestCodeInvalidation(harness);
    printf("\n");
    // Contract checks last: they publish their own stub at the same address.
    TestContracts(harness);

    printf("\n%d check(s), %s (%d failure%s)\n", g_checks,
           g_failures == 0 ? "ALL PASS" : "FAILED", g_failures, g_failures == 1 ? "" : "s");
    printf("SCOPE: real x86-64 executed by FEXCore through the public API, from\n");
    printf("       assembler-generated fixtures. Single-threaded; no HLE, interrupt or Step.\n");
    return g_failures == 0 ? 0 : 1;
}
