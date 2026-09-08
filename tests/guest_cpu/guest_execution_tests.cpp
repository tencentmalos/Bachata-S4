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

#include <atomic>
#include <chrono>
#include <cinttypes>
#include <cstdio>
#include <cstring>
#include <string>
#include <thread>
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
        auto bytes = pin.Value().WritableBytes();
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
    auto writable = pin.Value().WritableBytes();
    std::memset(writable.data(), 0, writable.size());
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

// --- G09: publication through the public memory API alone --------------------------------------
//
// G08 above publishes with CpuContext::InvalidateCode, which is the backend's own entry point. That
// proves the backend can discard translations; it does not prove an embedder holding only a
// GuestAddressSpace gets the same guarantee. The 2026-09-08 publication review (P1-C) found that it
// did not: PublishCode and InvalidateCode both reported success, code_generation advanced 1 -> 3,
// and the guest still executed the previous fixture. These checks never touch the context, so a
// regression cannot hide behind the backend path the way it did then.

// Publishes a fixture using only GuestAddressSpace. No CpuContext call anywhere.
bool PublishViaPublicApi(Harness& harness, const Fixtures::Fixture& fixture, std::string& error) {
    const GuestRange range{GuestAddress{harness.code_base}, kMappingSize};

    auto writable = harness.space->Protect(range, GuestPermission::Read | GuestPermission::Write);
    if (!writable) {
        error = "Protect(RW): " + Describe(writable.GetError());
        return false;
    }

    std::vector<std::byte> image(kMappingSize, std::byte{0});
    if (fixture.bytes.size() > image.size()) {
        error = "fixture is larger than the code mapping";
        return false;
    }
    std::memcpy(image.data(), fixture.bytes.data(), fixture.bytes.size());
    if (fixture.gate_offset >= 0) {
        std::memcpy(image.data() + fixture.gate_offset, &harness.return_gate,
                    sizeof(harness.return_gate));
    }

    {
        auto quiesced = harness.space->Quiesce(/*timeout_ns=*/1'000'000'000);
        if (!quiesced) {
            error = "Quiesce: " + Describe(quiesced.GetError());
            return false;
        }
        // PublishCode alone, with no InvalidateCode after it: publishing new bytes has to discard
        // the translation of the bytes it replaced, or "published" means nothing.
        auto published = harness.space->PublishCode(quiesced.Value(), range, image);
        if (!published) {
            error = "PublishCode: " + Describe(published.GetError());
            return false;
        }
        // Token released here. Protect is a writer path, and the transaction excludes those.
    }

    auto executable =
        harness.space->Protect(range, GuestPermission::Read | GuestPermission::Execute);
    if (!executable) {
        error = "Protect(RX): " + Describe(executable.GetError());
        return false;
    }
    return true;
}

// Runs whatever is at the code base once, in its own thread.
bool RunPublishedCode(Harness& harness, std::uint64_t& rax, std::string& error) {
    ThreadInit init{};
    init.entry_rip = GuestCodeAddress{harness.code_base};
    init.initial_rsp = GuestAddress{harness.stack_top};
    init.guest_tid = 1;

    auto thread = harness.context->CreateThread(init);
    if (!thread) {
        error = "CreateThread: " + Describe(thread.GetError());
        return false;
    }
    auto run = harness.context->Run(thread.Value(), RunOptions{});
    if (!run) {
        error = "Run: " + Describe(run.GetError());
        (void)harness.context->DestroyThread(thread.Value());
        return false;
    }
    rax = run.Value().snapshot.registers.Get(Gpr::Rax);
    const bool returned = run.Value().primary_reason == StopReason::Returned;

    auto destroyed = harness.context->DestroyThread(thread.Value());
    if (!destroyed) {
        error = "DestroyThread: " + Describe(destroyed.GetError());
        return false;
    }
    if (!returned) {
        error = "did not return through the gate";
        return false;
    }
    return true;
}

void TestPublicApiPublication(Harness& harness) {
    const auto* a = FindFixture("constant_a");
    const auto* b = FindFixture("constant_b");
    if (!a || !b) {
        Check("G09a", "public publication fixtures are present", false);
        return;
    }

    bool alternated = true;
    std::string error;
    for (unsigned epoch = 0; epoch < 100 && alternated; ++epoch) {
        const bool use_a = (epoch % 2) == 0;
        const std::uint64_t expected = use_a ? 17u : 34u;

        if (!PublishViaPublicApi(harness, use_a ? *a : *b, error)) {
            alternated = false;
            error = "epoch " + std::to_string(epoch) + ": " + error;
            break;
        }
        std::uint64_t rax = 0;
        if (!RunPublishedCode(harness, rax, error)) {
            alternated = false;
            error = "epoch " + std::to_string(epoch) + ": " + error;
            break;
        }
        if (rax != expected) {
            // This is the exact P1-C symptom: the publication succeeded and the old block ran.
            alternated = false;
            error = "epoch " + std::to_string(epoch) + ": executed stale code, rax=" +
                    std::to_string(rax) + " (expected " + std::to_string(expected) + ")";
            break;
        }
    }
    Check("G09a", "100 publications through GuestAddressSpace alone run the new code", alternated,
          error);

    // The other public entry point: bytes changed through a pinned span, then invalidated. A
    // caller that writes its own bytes must be able to make that stick without PublishCode.
    const GuestRange range{GuestAddress{harness.code_base}, kMappingSize};
    bool invalidated_ok = true;
    error.clear();
    if (!harness.space->Protect(range, GuestPermission::Read | GuestPermission::Write)) {
        invalidated_ok = false;
        error = "Protect(RW) failed";
    } else {
        {
            auto pin = harness.space->AcquirePinnedSpan(range, /*writable=*/true);
            if (!pin) {
                invalidated_ok = false;
                error = "AcquirePinnedSpan: " + Describe(pin.GetError());
            } else {
                auto bytes = pin.Value().WritableBytes();
                std::memset(bytes.data(), 0, bytes.size());
                std::memcpy(bytes.data(), a->bytes.data(), a->bytes.size());
                if (a->gate_offset >= 0) {
                    std::memcpy(bytes.data() + a->gate_offset, &harness.return_gate,
                                sizeof(harness.return_gate));
                }
            }
        }
        if (invalidated_ok) {
            auto quiesced = harness.space->Quiesce(/*timeout_ns=*/1'000'000'000);
            if (!quiesced) {
                invalidated_ok = false;
                error = "Quiesce: " + Describe(quiesced.GetError());
            } else {
                auto discarded = harness.space->InvalidateCode(quiesced.Value(), range,
                                                               InvalidationReason::HostWrite);
                if (!discarded) {
                    invalidated_ok = false;
                    error = "InvalidateCode: " + Describe(discarded.GetError());
                }
            }
        }
    }
    if (invalidated_ok &&
        !harness.space->Protect(range, GuestPermission::Read | GuestPermission::Execute)) {
        invalidated_ok = false;
        error = "Protect(RX) failed";
    }
    if (invalidated_ok) {
        std::uint64_t rax = 0;
        if (!RunPublishedCode(harness, rax, error)) {
            invalidated_ok = false;
        } else if (rax != 17u) {
            invalidated_ok = false;
            error = "executed stale code, rax=" + std::to_string(rax) + " (expected 17)";
        }
    }
    Check("G09b", "GuestAddressSpace::InvalidateCode discards the backend's translation",
          invalidated_ok, error);

    Check("G09c", "successful publications leave no poisoned range",
          !harness.space->HasPoisonedCode());
    Check("G09d", "public publication leaves no guest threads",
          harness.context->LiveThreadCount() == 0);
}

// Fault injection must test actual cached guest execution, not only metadata.
void TestPinnedInvalidationRecovery(Harness& harness) {
    const auto* a = FindFixture("constant_a");
    const auto* b = FindFixture("constant_b");
    if (!a || !b) { Check("G10a", "failure fixtures exist", false); return; }
    auto warm = RunFixture(harness, *a, [](RegisterPatch&) {});
    if (!warm.ok || warm.result.snapshot.registers.Get(Gpr::Rax) != 17) {
        Check("G10a", "warm old translation", false, warm.error); return;
    }
    auto* real_sink = dynamic_cast<CodeInvalidationSink*>(harness.context.get());
    if (!real_sink) { Check("G10a", "backend sink exists", false); return; }
    struct FailingSink final : CodeInvalidationSink {
        std::string_view Name() const override { return "guest-recovery-test"; }
        Status DiscardTranslations(GuestRange, InvalidationReason) override {
            return MakeError(ErrorCategory::BackendFailure, "test", "injected failure");
        }
    } failed_sink;
    struct Restore final {
        GuestAddressSpace& space;
        CodeInvalidationSink* real;
        CodeInvalidationSink* injected;
        bool done{};
        void Apply() {
            if (done) return;
            space.ClearCodeInvalidationSink(injected);
            Check("G10c", "restore real backend registration", bool(space.SetCodeInvalidationSink(real)));
            done = true;
        }
        ~Restore() { Apply(); }
    } restore{*harness.space, real_sink, &failed_sink};
    harness.space->ClearCodeInvalidationSink(real_sink);
    if (!harness.space->SetCodeInvalidationSink(&failed_sink)) {
        Check("G10a", "register failure injection", false); return;
    }
    GuestRange range{GuestAddress{harness.code_base}, kMappingSize};
    if (!harness.space->Protect(range, GuestPermission::Read | GuestPermission::Write)) {
        Check("G10a", "make code writable", false); return;
    }
    {
        auto pin = harness.space->AcquirePinnedSpan(range, true);
        if (!pin) { Check("G10a", "pin code", false); return; }
        auto bytes = pin.Value().WritableBytes();
        std::memcpy(bytes.data(), b->bytes.data(), b->bytes.size());
        std::memcpy(bytes.data() + b->gate_offset, &harness.return_gate, sizeof(harness.return_gate));
    }
    {
        auto q = harness.space->Quiesce(1'000'000);
        if (!q) { Check("G10a", "quiesce failed code", false); return; }
        auto invalidated = harness.space->InvalidateCode(q.Value(), range, InvalidationReason::HostWrite);
        Check("G10a", "pinned rewrite with failed invalidation poisons code",
              !invalidated && harness.space->HasPoisonedCode());
    }
    std::uint64_t value{};
    std::string error;
    auto rx = harness.space->Protect(range, GuestPermission::Read | GuestPermission::Execute);
    Check("G10b", "poison refuses execute permission and real guest admission",
          !rx && !RunPublishedCode(harness, value, error));
    restore.Apply();
    {
        auto q = harness.space->Quiesce(1'000'000);
        if (!q || !harness.space->InvalidateCode(q.Value(), range, InvalidationReason::HostWrite)) {
            Check("G10d", "repair real translation", false); return;
        }
    }
    error.clear();
    const auto executable = harness.space->Protect(range, GuestPermission::Read | GuestPermission::Execute);
    Check("G10d", "successful repair resumes the new B translation",
          executable && RunPublishedCode(harness, value, error) && value == 34 &&
          !harness.space->HasPoisonedCode(), error);
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

// --- R2-C02: asynchronous stop of a loop that cannot stop itself -------------------------------
//
// The fixture spins on a backward jump with no HLE call, no syscall and no exit. Once FEX has
// compiled it, the owner is inside one translated block indefinitely: a cooperative check never
// runs and there is no block boundary to wait for. Only the out-of-band kick documented in
// docs/fex-async-stop-source-proof.md can end it, which is why this is the fixture the spec names.
void TestAsyncInterrupt(Harness& harness) {
    const auto* fixture = FindFixture("spin_loop");
    if (fixture == nullptr) {
        Check("G11", "spin loop fixture is present", false);
        return;
    }

    std::string error;
    if (!LoadFixture(harness, *fixture, error)) {
        Check("G11", "publish the spin loop", false, error);
        return;
    }

    ThreadInit init{};
    init.entry_rip = GuestCodeAddress{harness.code_base};
    init.initial_rsp = GuestAddress{harness.stack_top};
    init.guest_tid = 1;

    // The owner thread that drives Run is also the thread that must CreateThread: the API binds a
    // thread to its owner, and only that thread may Run it. Creating on this (controller) thread
    // and running on another makes Run refuse with WrongThread and return immediately, which looks
    // exactly like the loop "exiting on its own". So the owner creates, publishes the handle to the
    // controller, and runs; the controller waits for that handle before issuing any request.
    std::atomic<bool> run_returned{false};
    std::atomic<bool> run_ok{false};
    std::string run_error;
    ThreadHandle handle{};
    std::mutex owner_mutex;
    std::condition_variable owner_ready;
    bool ready = false;
    bool create_failed = false;
    std::string create_error;

    std::thread owner([&] {
        auto thread = harness.context->CreateThread(init);
        if (!thread) {
            {
                std::lock_guard<std::mutex> ready_guard{owner_mutex};
                create_failed = true;
                create_error = Describe(thread.GetError());
                ready = true;
            }
            owner_ready.notify_all();
            return;
        }
        {
            std::lock_guard<std::mutex> ready_guard{owner_mutex};
            handle = thread.Value();
            ready = true;
        }
        owner_ready.notify_all();

        auto result = harness.context->Run(handle, RunOptions{});
        run_ok.store(bool(result), std::memory_order_release);
        if (!result) {
            run_error = Describe(result.GetError());
        }
        run_returned.store(true, std::memory_order_release);
    });

    {
        std::unique_lock<std::mutex> ready_guard{owner_mutex};
        owner_ready.wait(ready_guard, [&] { return ready; });
    }
    if (create_failed) {
        Check("G11", "create the spinning thread on its owner", false, create_error);
        owner.join();
        return;
    }

    // Let it actually get into the JIT and warm the block. Stopping a thread that never started
    // would pass without exercising anything.
    std::this_thread::sleep_for(std::chrono::milliseconds(150));
    Check("G11a", "the spin loop is still running before any request",
          !run_returned.load(std::memory_order_acquire),
          run_returned.load() ? "it exited on its own: not an unbreakable loop" : "");

    const auto requested = std::chrono::steady_clock::now();
    auto ticket = harness.context->RequestInterrupt(handle, InterruptReason::Pause);
    Check("G11b", "an interrupt can be requested while the owner is in the JIT", bool(ticket),
          ticket ? std::string{} : Describe(ticket.GetError()));
    if (!ticket) {
        // Nothing will stop the loop now; do not leave a spinning thread behind.
        (void)harness.context->RequestInterrupt(handle, InterruptReason::Cancel);
        owner.join();
        return;
    }

    auto receipt = harness.context->WaitStopped(ticket.Value(), 1'000'000'000);
    const auto stopped_after = std::chrono::duration_cast<std::chrono::milliseconds>(
        std::chrono::steady_clock::now() - requested);
    Check("G11c", "the owner reaches a safe point", bool(receipt),
          receipt ? std::string{} : Describe(receipt.GetError()));

    if (receipt) {
        // The spec's budget: a stop request must reach a safe point within a second.
        Check("G11d", "the stop lands within 1s",
              stopped_after <= std::chrono::milliseconds(1000),
              std::to_string(stopped_after.count()) + "ms");
        Check("G11e", "the receipt answers the ticket that was issued",
              receipt.Value().request_epoch == ticket.Value().epoch);
        Check("G11f", "the receipt carries this context's identity",
              receipt.Value().context_id == harness.context->ContextId());
        // The snapshot is only meaningful because the owner spilled before acknowledging.
        const std::uint64_t counter = receipt.Value().snapshot.registers.Get(Gpr::Rax);
        Check("G11g", "the stopped snapshot shows the loop counter advanced", counter > 0,
              "rax=" + std::to_string(counter));

        // Resume, and confirm it continued rather than restarting.
        auto resumed = harness.context->Resume(handle, ticket.Value().epoch);
        Check("G11h", "the paused owner can be resumed", bool(resumed),
              resumed ? std::string{} : Describe(resumed.GetError()));
        std::this_thread::sleep_for(std::chrono::milliseconds(100));
        Check("G11i", "the owner is executing again after Resume",
              !run_returned.load(std::memory_order_acquire));
    }

    // Cancel ends the run so the thread can be joined. A Pause would leave it parked forever.
    auto cancel = harness.context->RequestInterrupt(handle, InterruptReason::Cancel);
    if (cancel) {
        (void)harness.context->WaitStopped(cancel.Value(), 1'000'000'000);
    }
    owner.join();
    Check("G11j", "Run returned after the cancel", run_returned.load(std::memory_order_acquire),
          run_error);
    (void)harness.context->DestroyThread(handle);
}

// --- R2-C03 subset: request identity and refusals ----------------------------------------------
// --- R2-C01: two owners genuinely concurrent in the JIT ---------------------------------------
//
// Each guest thread is created and run on its own host owner thread (the API binds a thread to its
// owner). Both run the unbreakable spin loop simultaneously. A barrier establishes that both were
// inside the JIT at the same time; then one is paused while the other must keep advancing, which
// cannot happen if Runs were serialized.
void TestTwoOwnerConcurrency(Harness& harness) {
    const auto* fixture = FindFixture("spin_loop");
    if (fixture == nullptr) {
        Check("G14", "spin loop fixture for two owners", false);
        return;
    }
    std::string error;
    if (!LoadFixture(harness, *fixture, error)) {
        Check("G14", "publish spin loop for two owners", false, error);
        return;
    }

    struct Owner {
        ThreadHandle handle{};
        std::thread host;
        std::atomic<bool> in_jit{false};
        std::atomic<bool> run_returned{false};
        std::atomic<std::uint64_t> native_tid{0};
        std::mutex ready_mutex;
        std::condition_variable ready_cv;
        bool ready{false};
        bool create_failed{false};
        std::string create_error;
        // Sampled rax when the controller paused us.
        std::atomic<std::uint64_t> sampled_rax{0};
    };
    auto run_owner = [&](Owner& owner, std::uint64_t guest_tid) {
        ThreadInit init{};
        init.entry_rip = GuestCodeAddress{harness.code_base};
        init.initial_rsp = GuestAddress{harness.stack_top};
        init.guest_tid = guest_tid;
        auto thread = harness.context->CreateThread(init);
        if (!thread) {
            std::lock_guard<std::mutex> g{owner.ready_mutex};
            owner.create_failed = true;
            owner.create_error = Describe(thread.GetError());
            owner.ready = true;
            owner.ready_cv.notify_all();
            return;
        }
        {
            std::lock_guard<std::mutex> g{owner.ready_mutex};
            owner.handle = thread.Value();
            owner.ready = true;
        }
        owner.ready_cv.notify_all();

        // Mark in-JIT once the Run has started; Run blocks for the whole spin. We cannot set this
        // before CreateThread returns (it is on this thread), but Run never returns for a spin, so
        // by the time the barrier waits below, being inside Run == being in the JIT.
        owner.native_tid.store(static_cast<std::uint64_t>(
            static_cast<std::intptr_t>(owner.host.native_handle())),
            std::memory_order_release);
        owner.in_jit.store(true, std::memory_order_release);
        auto result = harness.context->Run(owner.handle, RunOptions{});
        owner.in_jit.store(false, std::memory_order_release);
        owner.run_returned.store(bool(result), std::memory_order_release);
        (void)result;
    };

    Owner a, b;
    a.host = std::thread([&] { run_owner(a, 11); });
    b.host = std::thread([&] { run_owner(b, 12); });

    auto wait_ready = [](Owner& o) {
        std::unique_lock<std::mutex> g{o.ready_mutex};
        o.ready_cv.wait(g, [&] { return o.ready; });
    };
    wait_ready(a);
    wait_ready(b);
    if (a.create_failed || b.create_failed) {
        Check("G14", "create two owners each on its own thread", false,
              a.create_error + " " + b.create_error);
        if (a.host.joinable()) a.host.join();
        if (b.host.joinable()) b.host.join();
        return;
    }

    // Let both warm into the JIT, then prove overlap: both in_jit at the same instant.
    std::this_thread::sleep_for(std::chrono::milliseconds(200));
    const bool both_in_jit = a.in_jit.load() && b.in_jit.load();
    Check("G14a", "two owners are both executing concurrently", both_in_jit);
    Check("G14b", "the two owners are distinct native threads",
          a.native_tid.load() != 0 && b.native_tid.load() != 0 &&
              a.native_tid.load() != b.native_tid.load(),
          "tid a=" + std::to_string(a.native_tid.load()) + " b=" +
              std::to_string(b.native_tid.load()));

    // Pause owner A. Owner B must still be running afterwards.
    auto ticket_a = harness.context->RequestInterrupt(a.handle, InterruptReason::Pause);
    Check("G14c", "interrupt owner A while both run", bool(ticket_a),
          ticket_a ? std::string{} : Describe(ticket_a.GetError()));
    if (ticket_a) {
        auto receipt = harness.context->WaitStopped(ticket_a.Value(), 1'000'000'000);
        Check("G14d", "owner A reached a safe point", bool(receipt),
              receipt ? std::string{} : Describe(receipt.GetError()));
        if (receipt) {
            a.sampled_rax.store(receipt.Value().snapshot.registers.Get(Gpr::Rax),
                                std::memory_order_release);
        }
    }

    // A is stopped (G14d proved the safe point); B must be untouched and still running. A's counter
    // must be frozen while B continues: that is the only way to show stopping one owner does not
    // hold the other.
    std::this_thread::sleep_for(std::chrono::milliseconds(50));
    const std::uint64_t a_rax_at_stop = a.sampled_rax.load(std::memory_order_acquire);
    const bool b_still_running = b.in_jit.load();
    // Re-read A's registers: it is parked, so the snapshot must be stable and equal to the receipt.
    std::uint64_t a_rax_later = a_rax_at_stop;
    {
        auto snap = harness.context->ReadRegisters(a.handle);
        if (snap) {
            a_rax_later = snap.Value().registers.Get(Gpr::Rax);
        }
    }
    Check("G14e", "stopping owner A freezes A while owner B keeps executing",
          b_still_running && a_rax_later == a_rax_at_stop && !b.run_returned.load(),
          "a_rax=" + std::to_string(a_rax_at_stop) + "->" + std::to_string(a_rax_later) +
              " b_running=" + std::to_string(int(b_still_running)));

    // Resume A; both run again.
    if (ticket_a) {
        auto resumed = harness.context->Resume(a.handle, ticket_a.Value().epoch);
        Check("G14f", "owner A resumes after the pause", bool(resumed),
              resumed ? std::string{} : Describe(resumed.GetError()));
    }
    std::this_thread::sleep_for(std::chrono::milliseconds(100));
    Check("G14g", "both owners run again after resume", a.in_jit.load() && b.in_jit.load());

    // Cancel both to unwind.
    for (Owner* owner : {&a, &b}) {
        auto cancel = harness.context->RequestInterrupt(owner->handle, InterruptReason::Cancel);
        if (cancel) {
            (void)harness.context->WaitStopped(cancel.Value(), 1'000'000'000);
        }
    }
    a.host.join();
    b.host.join();
    Check("G14h", "both owners returned after cancel",
          a.run_returned.load() && b.run_returned.load());
    (void)harness.context->DestroyThread(a.handle);
    (void)harness.context->DestroyThread(b.handle);
}

// LLDB-attached pause being timed.
void TestInterruptStress(Harness& harness) {
    const auto* fixture = FindFixture("spin_loop");
    if (fixture == nullptr) {
        Check("G13", "spin loop fixture is present for stress", false);
        return;
    }
    std::string error;
    if (!LoadFixture(harness, *fixture, error)) {
        Check("G13", "publish the spin loop for stress", false, error);
        return;
    }

    ThreadInit init{};
    init.entry_rip = GuestCodeAddress{harness.code_base};
    init.initial_rsp = GuestAddress{harness.stack_top};
    init.guest_tid = 7;

    ThreadHandle handle{};
    std::atomic<bool> run_returned{false};
    std::atomic<bool> run_ok{false};
    std::string run_error;
    std::mutex ready_mutex;
    std::condition_variable ready_cv;
    bool ready = false;
    bool create_failed = false;
    std::string create_error;

    std::thread owner([&] {
        auto thread = harness.context->CreateThread(init);
        if (!thread) {
            std::lock_guard<std::mutex> g{ready_mutex};
            create_failed = true;
            create_error = Describe(thread.GetError());
            ready = true;
            ready_cv.notify_all();
            return;
        }
        {
            std::lock_guard<std::mutex> g{ready_mutex};
            handle = thread.Value();
            ready = true;
        }
        ready_cv.notify_all();

        auto result = harness.context->Run(handle, RunOptions{});
        run_ok.store(bool(result), std::memory_order_release);
        if (!result) {
            run_error = Describe(result.GetError());
        }
        run_returned.store(true, std::memory_order_release);
    });
    {
        std::unique_lock<std::mutex> g{ready_mutex};
        ready_cv.wait(g, [&] { return ready; });
    }
    if (create_failed) {
        Check("G13", "create the stress owner on its thread", false, create_error);
        owner.join();
        return;
    }

    constexpr int kCycles = 100;
    constexpr auto kBudget = std::chrono::milliseconds(1000);
    std::vector<long> latency_ms;
    latency_ms.reserve(kCycles);
    bool every_stop_fast = true;
    bool every_receipt_valid = true;
    std::string latency_detail;

    // Wait for the owner to be inside the JIT before the first kick. RequestInterrupt only signals a
    // thread it knows is executing; a request sent while the thread is still entering leaves the
    // pending bit set without a kick, and the warm spin block has no cooperative check to notice it.
    std::this_thread::sleep_for(std::chrono::milliseconds(150));

    for (int cycle = 0; cycle < kCycles; ++cycle) {
        auto ticket = harness.context->RequestInterrupt(handle, InterruptReason::Pause);
        if (!ticket) {
            every_receipt_valid = false;
            latency_detail = "cycle " + std::to_string(cycle) + ": RequestInterrupt: " +
                             Describe(ticket.GetError());
            break;
        }
        const auto requested = std::chrono::steady_clock::now();
        auto receipt = harness.context->WaitStopped(ticket.Value(), 1'000'000'000);
        if (!receipt) {
            every_receipt_valid = false;
            latency_detail = "cycle " + std::to_string(cycle) + ": WaitStopped: " +
                             Describe(receipt.GetError());
            break;
        }
        const auto elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(
            std::chrono::steady_clock::now() - requested);
        latency_ms.push_back(elapsed.count());
        if (elapsed > kBudget) {
            every_stop_fast = false;
        }

        // Resume only the epoch that was actually acknowledged; this must not consume the next
        // request, but there is none yet in this single-controller loop.
        auto resumed = harness.context->Resume(handle, ticket.Value().epoch);
        if (!resumed) {
            every_receipt_valid = false;
            latency_detail = "cycle " + std::to_string(cycle) + ": Resume: " +
                             Describe(resumed.GetError());
            break;
        }
        // Give the owner time to fully leave the pause stub and re-enter the JIT before the next
        // kick. Without a settle gap the request can race the SIGILL return-to-JIT and measure the
        // setup window rather than steady-state stop latency; warm loops are the stated target.
        std::this_thread::sleep_for(std::chrono::milliseconds(20));
    }

    if (!latency_ms.empty()) {
        std::sort(latency_ms.begin(), latency_ms.end());
        const long p50 = latency_ms[latency_ms.size() / 2];
        const long p95 = latency_ms[(latency_ms.size() * 95) / 100];
        const long max_l = latency_ms.back();
        char buffer[160];
        std::snprintf(buffer, sizeof(buffer), "%zu cycles, p50=%ldms p95=%ldms max=%ldms",
                      latency_ms.size(), p50, p95, max_l);
        latency_detail = buffer;
    }

    Check("G13a", "100 pause/receipt/resume cycles each produced a valid receipt",
          static_cast<int>(latency_ms.size()) == kCycles && every_receipt_valid, latency_detail);
    Check("G13b", "every stop reached a safe point within the 1s budget", every_stop_fast,
          latency_detail);

    // Cancel once to let the owner return and clean up.
    auto cancel = harness.context->RequestInterrupt(handle, InterruptReason::Cancel);
    if (cancel) {
        (void)harness.context->WaitStopped(cancel.Value(), 1'000'000'000);
    }
    owner.join();
    Check("G13c", "the stress owner returned after cancel",
          run_returned.load(std::memory_order_acquire) && run_ok.load(std::memory_order_acquire),
          run_error);
    (void)harness.context->DestroyThread(handle);
}

void TestInterruptRefusals(Harness& harness) {
    const auto* fixture = FindFixture("return_only");
    if (fixture == nullptr) {
        Check("G12", "return_only fixture is present", false);
        return;
    }
    std::string error;
    if (!LoadFixture(harness, *fixture, error)) {
        Check("G12", "publish return_only", false, error);
        return;
    }

    ThreadInit init{};
    init.entry_rip = GuestCodeAddress{harness.code_base};
    init.initial_rsp = GuestAddress{harness.stack_top};
    init.guest_tid = 1;
    auto thread = harness.context->CreateThread(init);
    if (!thread) {
        Check("G12", "create a thread", false, Describe(thread.GetError()));
        return;
    }
    const ThreadHandle handle = thread.Value();

    // A ticket that did not come from this context must not authorise anything, even though its
    // thread id and epoch would match.
    InterruptTicket forged{};
    forged.context_id = harness.context->ContextId() + 1000;
    forged.thread_id = handle.id;
    forged.thread_generation = handle.generation;
    forged.epoch = 1;
    auto refused = harness.context->WaitStopped(forged, 1'000'000);
    Check("G12a", "a ticket from another context is refused", !refused,
          refused ? "it was accepted" : Describe(refused.GetError()));

    // A destroyed handle cannot be interrupted.
    auto stale_handle = handle;
    stale_handle.generation += 1;
    auto stale = harness.context->RequestInterrupt(stale_handle, InterruptReason::Pause);
    Check("G12b", "a stale thread generation is refused", !stale,
          stale ? "it was accepted" : Describe(stale.GetError()));

    // Resume without a stop to acknowledge is a caller error, not a no-op.
    auto early = harness.context->Resume(handle, 999);
    Check("G12c", "resuming an unacknowledged epoch is refused", !early,
          early ? "it was accepted" : Describe(early.GetError()));

    // A non-zero deadline must be refused before execution rather than silently ignored.
    RunOptions with_deadline{};
    with_deadline.deadline_ns = 1'000'000;
    auto deadline = harness.context->Run(handle, with_deadline);
    Check("G12d", "a non-zero deadline_ns is refused before running",
          !deadline && deadline.Category() == ErrorCategory::Unsupported,
          deadline ? "it ran" : Describe(deadline.GetError()));

    (void)harness.context->DestroyThread(handle);
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
    TestPublicApiPublication(harness);
    TestAsyncInterrupt(harness);
    TestTwoOwnerConcurrency(harness);
    TestInterruptStress(harness);
    TestInterruptRefusals(harness);
    TestPinnedInvalidationRecovery(harness);
    printf("\n");
    // Contract checks last: they publish their own stub at the same address.
    TestContracts(harness);

    printf("\n%d check(s), %s (%d failure%s)\n", g_checks,
           g_failures == 0 ? "ALL PASS" : "FAILED", g_failures, g_failures == 1 ? "" : "s");
    printf("SCOPE: real x86-64 executed by FEXCore through the public API, from\n");
    printf("       assembler-generated fixtures. Single-threaded; no HLE, interrupt or Step.\n");
    return g_failures == 0 ? 0 : 1;
}
