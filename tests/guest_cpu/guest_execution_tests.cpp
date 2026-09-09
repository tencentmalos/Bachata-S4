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
#include <barrier>
#include <cfenv>
#include <chrono>
#include <cinttypes>
#include <condition_variable>
#include <csignal>
#include <cstdio>
#include <cstring>
#include <functional>
#include <future>
#include <mutex>
#include <queue>
#include <string>
#include <sys/syscall.h>
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

void Check(const char *id, const char *name, bool condition, const std::string &detail = {}) {
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

void CheckU64(const char *id, const char *name, std::uint64_t actual, std::uint64_t expected) {
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

const Fixtures::Fixture *FindFixture(std::string_view name) {
    for (const auto &fixture : Fixtures::kAll) {
        if (fixture.name == name) {
            return &fixture;
        }
    }
    return nullptr;
}

// Writes a fixture into guest memory, patches the return-gate address, publishes RX, and discards
// any translation of whatever was there before.
bool LoadFixture(Harness &harness, const Fixtures::Fixture &fixture, std::string &error) {
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
    auto invalidated =
        harness.context->InvalidateCode(quiesced.Value(), range, InvalidationReason::HostWrite);
    if (!invalidated) {
        error = "InvalidateCode: " + Describe(invalidated.GetError());
        return false;
    }
    return true;
}

// Zeroes the guest stack so a stale value cannot look like a fresh store.
bool ResetStack(Harness &harness, std::string &error) {
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
bool ReadGuestU64(Harness &harness, std::uint64_t address, std::uint64_t &out) {
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
RunOutcome RunFixture(Harness &harness, const Fixtures::Fixture &fixture, SetupFn &&setup) {
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
void ReportFixture(const Fixtures::Fixture &fixture) {
    printf("  fixture %.*s (%zu bytes):\n", static_cast<int>(fixture.name.size()),
           fixture.name.data(), fixture.bytes.size());
    printf("    %.*s\n", static_cast<int>(fixture.disassembly.size()), fixture.disassembly.data());
    fflush(stdout);
}

bool Require(const char *id, const RunOutcome &outcome, const Fixtures::Fixture &fixture) {
    if (!outcome.ok) {
        Check(id, "run fixture", false, outcome.error);
        ReportFixture(fixture);
        return false;
    }
    return true;
}

// --- C01: integer arithmetic -------------------------------------------------------------------
void TestIntegerArithmetic(Harness &harness) {
    const auto *fixture = FindFixture("integer");
    if (fixture == nullptr) {
        Check("G01", "integer fixture is present", false);
        return;
    }

    auto outcome = RunFixture(harness, *fixture, [](RegisterPatch &patch) {
        patch.fields = RegisterValidity::Gpr;
        patch.gpr_mask = (1u << Index(Gpr::R8)) | (1u << Index(Gpr::R9));
        patch.values.Set(Gpr::R8, 0x1000);
        patch.values.Set(Gpr::R9, 0x234);
    });
    if (!Require("G01", outcome, *fixture)) {
        return;
    }

    const auto &regs = outcome.result.snapshot.registers;
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
void TestBranches(Harness &harness) {
    const auto *fixture = FindFixture("branch");
    if (fixture == nullptr) {
        Check("G02", "branch fixture is present", false);
        return;
    }

    auto equal = RunFixture(harness, *fixture, [](RegisterPatch &patch) {
        patch.fields = RegisterValidity::Gpr;
        patch.gpr_mask = (1u << Index(Gpr::Rdi)) | (1u << Index(Gpr::Rsi));
        patch.values.Set(Gpr::Rdi, 42);
        patch.values.Set(Gpr::Rsi, 42);
    });
    if (Require("G02", equal, *fixture)) {
        CheckU64("G02a", "not-taken branch fell through to the mov",
                 equal.result.snapshot.registers.Get(Gpr::Rax), 1);
    }

    auto unequal = RunFixture(harness, *fixture, [](RegisterPatch &patch) {
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
void TestLoadStore(Harness &harness) {
    const auto *fixture = FindFixture("loadstore");
    if (fixture == nullptr) {
        Check("G03", "loadstore fixture is present", false);
        return;
    }

    constexpr std::uint64_t kFirst = 0xAAAA'BBBB'CCCC'DDDDull;
    constexpr std::uint64_t kSecond = 0x1111'2222'3333'4444ull;

    auto outcome = RunFixture(harness, *fixture, [](RegisterPatch &patch) {
        patch.fields = RegisterValidity::Gpr;
        patch.gpr_mask = (1u << Index(Gpr::Rdi)) | (1u << Index(Gpr::Rsi));
        patch.values.Set(Gpr::Rdi, kFirst);
        patch.values.Set(Gpr::Rsi, kSecond);
    });
    if (!Require("G03", outcome, *fixture)) {
        return;
    }

    const auto &regs = outcome.result.snapshot.registers;
    CheckU64("G03a", "pop rax read back the second push", regs.Get(Gpr::Rax), kSecond);
    CheckU64("G03b", "pop rcx read back the first push", regs.Get(Gpr::Rcx), kFirst);
    CheckU64("G03c", "stack pointer is balanced after push/pop", regs.Rsp(), harness.stack_top);
}

// --- C01: a store that must remain visible in guest memory ---------------------------------------
void TestStoreMemory(Harness &harness) {
    const auto *fixture = FindFixture("store_memory");
    if (fixture == nullptr) {
        Check("G07", "store_memory fixture is present", false);
        return;
    }

    constexpr std::uint64_t kLow = 0x0BAD'C0DE'0BAD'C0DEull;
    constexpr std::uint64_t kHigh = 0xFEED'FACE'FEED'FACEull;

    auto outcome = RunFixture(harness, *fixture, [](RegisterPatch &patch) {
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
    const auto *raw = reinterpret_cast<const volatile std::uint64_t *>(harness.stack_top);
    const std::uint64_t direct_low = raw[-2];
    const std::uint64_t direct_high = raw[-1];
    char detail[192];
    std::snprintf(detail, sizeof(detail),
                  "direct[rsp-16]=%s direct[rsp-8]=%s (pinned read gave %s / %s)",
                  Hex(direct_low).c_str(), Hex(direct_high).c_str(), Hex(slot_minus_16).c_str(),
                  Hex(slot_minus_8).c_str());
    Check("G07d", "direct host read agrees with the pinned-span read",
          direct_low == slot_minus_16 && direct_high == slot_minus_8, detail);

    // What the guest itself read back from those slots. If these are correct while the host reads
    // are zero, the store and load are consistent with each other but invisible outside the guest;
    // if these are also zero, the store never happened at all.
    const auto &regs = outcome.result.snapshot.registers;
    CheckU64("G07e", "guest read back its own store to [rsp-16]", regs.Get(Gpr::Rax), kLow);
    CheckU64("G07f", "guest read back its own store to [rsp-8]", regs.Get(Gpr::Rcx), kHigh);
}

// --- C02: SSE2 -----------------------------------------------------------------------------------
void TestSse2(Harness &harness) {
    const auto *fixture = FindFixture("sse2");
    if (fixture == nullptr) {
        Check("G04", "sse2 fixture is present", false);
        return;
    }

    constexpr std::uint64_t kLeft = 0x0000'0001'0000'0002ull;
    constexpr std::uint64_t kRight = 0x0000'0010'0000'0020ull;
    // paddq adds the full 64-bit lane, and neither lane carries into the other here.
    constexpr std::uint64_t kExpected = kLeft + kRight;

    auto outcome = RunFixture(harness, *fixture, [](RegisterPatch &patch) {
        patch.fields = RegisterValidity::Gpr;
        patch.gpr_mask = (1u << Index(Gpr::Rdi)) | (1u << Index(Gpr::Rsi));
        patch.values.Set(Gpr::Rdi, kLeft);
        patch.values.Set(Gpr::Rsi, kRight);
    });
    if (!Require("G04", outcome, *fixture)) {
        return;
    }

    const auto &regs = outcome.result.snapshot.registers;
    CheckU64("G04a", "paddq result moved back to a GPR", regs.Get(Gpr::Rax), kExpected);
    CheckU64("G04b", "xmm0 low half holds the packed sum", regs.xmm[0].low, kExpected);
    Check("G04c", "snapshot reports XMM as valid", HasAll(regs.validity, RegisterValidity::Xmm));
}

// --- D05: a HLT outside the gate is not a normal return
// -------------------------------------------
void TestUnregisteredHlt(Harness &harness) {
    const auto *fixture = FindFixture("bare_hlt");
    if (fixture == nullptr) {
        Check("G05", "bare_hlt fixture is present", false);
        return;
    }

    auto outcome = RunFixture(harness, *fixture, [](RegisterPatch &) {});
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
void TestCodeInvalidation(Harness &harness) {
    const auto *a = FindFixture("constant_a");
    const auto *b = FindFixture("constant_b");
    if (!a || !b) {
        Check("G08a", "invalidation fixtures are present", false);
        return;
    }

    bool recreated_ok = true;
    std::string error;
    for (unsigned epoch = 0; epoch < 100; ++epoch) {
        const bool use_a = (epoch % 2) == 0;
        auto outcome = RunFixture(harness, use_a ? *a : *b, [](RegisterPatch &) {});
        const auto expected = use_a ? 17u : 34u;
        if (!outcome.ok || outcome.result.primary_reason != StopReason::Returned ||
            outcome.result.snapshot.registers.Get(Gpr::Rax) != expected ||
            harness.context->LiveThreadCount() != 0) {
            recreated_ok = false;
            error = "epoch " + std::to_string(epoch) + ": " + outcome.error;
            break;
        }
    }
    Check("G08a", "100 same-VA publications with zero live threads between runs", recreated_ok,
          error);

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
        if (first)
            (void)harness.context->DestroyThread(first.Value());
        if (second)
            (void)harness.context->DestroyThread(second.Value());
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
            if (!stopped ||
                (epoch != 0 && stopped.Value().registers.Get(Gpr::Rax) != (use_a ? 34u : 17u))) {
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
bool PublishViaPublicApi(Harness &harness, const Fixtures::Fixture &fixture, std::string &error) {
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
bool RunPublishedCode(Harness &harness, std::uint64_t &rax, std::string &error) {
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

void TestPublicApiPublication(Harness &harness) {
    const auto *a = FindFixture("constant_a");
    const auto *b = FindFixture("constant_b");
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
            error = "epoch " + std::to_string(epoch) +
                    ": executed stale code, rax=" + std::to_string(rax) + " (expected " +
                    std::to_string(expected) + ")";
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
void TestPinnedInvalidationRecovery(Harness &harness) {
    const auto *a = FindFixture("constant_a");
    const auto *b = FindFixture("constant_b");
    if (!a || !b) {
        Check("G10a", "failure fixtures exist", false);
        return;
    }
    auto warm = RunFixture(harness, *a, [](RegisterPatch &) {});
    if (!warm.ok || warm.result.snapshot.registers.Get(Gpr::Rax) != 17) {
        Check("G10a", "warm old translation", false, warm.error);
        return;
    }
    auto *real_sink = dynamic_cast<CodeInvalidationSink *>(harness.context.get());
    if (!real_sink) {
        Check("G10a", "backend sink exists", false);
        return;
    }
    struct FailingSink final : CodeInvalidationSink {
        std::string_view Name() const override { return "guest-recovery-test"; }
        Status DiscardTranslations(GuestRange, InvalidationReason) override {
            return MakeError(ErrorCategory::BackendFailure, "test", "injected failure");
        }
    } failed_sink;
    struct Restore final {
        GuestAddressSpace &space;
        CodeInvalidationSink *real;
        CodeInvalidationSink *injected;
        bool done{};
        void Apply() {
            if (done)
                return;
            space.ClearCodeInvalidationSink(injected);
            Check("G10c", "restore real backend registration",
                  bool(space.SetCodeInvalidationSink(real)));
            done = true;
        }
        ~Restore() { Apply(); }
    } restore{*harness.space, real_sink, &failed_sink};
    harness.space->ClearCodeInvalidationSink(real_sink);
    if (!harness.space->SetCodeInvalidationSink(&failed_sink)) {
        Check("G10a", "register failure injection", false);
        return;
    }
    GuestRange range{GuestAddress{harness.code_base}, kMappingSize};
    if (!harness.space->Protect(range, GuestPermission::Read | GuestPermission::Write)) {
        Check("G10a", "make code writable", false);
        return;
    }
    {
        auto pin = harness.space->AcquirePinnedSpan(range, true);
        if (!pin) {
            Check("G10a", "pin code", false);
            return;
        }
        auto bytes = pin.Value().WritableBytes();
        std::memcpy(bytes.data(), b->bytes.data(), b->bytes.size());
        std::memcpy(bytes.data() + b->gate_offset, &harness.return_gate,
                    sizeof(harness.return_gate));
    }
    {
        auto q = harness.space->Quiesce(1'000'000);
        if (!q) {
            Check("G10a", "quiesce failed code", false);
            return;
        }
        auto invalidated =
            harness.space->InvalidateCode(q.Value(), range, InvalidationReason::HostWrite);
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
            Check("G10d", "repair real translation", false);
            return;
        }
    }
    error.clear();
    const auto executable =
        harness.space->Protect(range, GuestPermission::Read | GuestPermission::Execute);
    Check("G10d", "successful repair resumes the new B translation",
          executable && RunPublishedCode(harness, value, error) && value == 34 &&
              !harness.space->HasPoisonedCode(),
          error);
}

// --- contract checks that need no execution
// -------------------------------------------------------
void TestContracts(Harness &harness) {
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

    const auto *stub = FindFixture("return_only");
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
        Check("G06g", "create a thread for the handle checks", false, Describe(thread.GetError()));
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
            Check("G06j", "quiesce the second address space", false, Describe(foreign.GetError()));
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

// --- Round 2 control regression ------------------------------------------------
// Test owners have a real command queue. Pause returns Run to this queue; Resume
// changes admission only. No flag set before Run is used as evidence of JIT work.
template <typename T> T Await(std::future<T> &result) {
    if (result.wait_for(std::chrono::seconds(2)) != std::future_status::ready) {
        std::fprintf(stderr, "FAIL: owner command exceeded 2s; supervisor cleanup required\n");
        std::fflush(stderr);
        std::_Exit(4);
    }
    return result.get();
}

class TestOwner final {
  public:
    TestOwner(Harness &h, std::uint64_t progress, std::uint64_t stack_delta = 0) : h_{h} {
        std::promise<ThreadHandle> ready;
        auto future = ready.get_future();
        host_ = std::thread([&, progress, stack_delta] {
            tid_ = static_cast<std::uint64_t>(::gettid());
            ThreadInit init{};
            init.entry_rip = GuestCodeAddress{h_.code_base};
            init.initial_rsp = GuestAddress{h_.stack_top - stack_delta};
            init.initial_state.fields = RegisterValidity::Gpr;
            init.initial_state.gpr_mask = (1u << Index(Gpr::Rdi));
            init.initial_state.values.Set(Gpr::Rdi, progress);
            auto created = h_.context->CreateThread(init);
            if (!created) {
                ready.set_value({});
                return;
            }
            ready.set_value(created.Value());
            for (;;) {
                std::function<void()> task;
                {
                    std::unique_lock guard{mutex_};
                    changed_.wait(guard, [&] { return quit_ || !tasks_.empty(); });
                    if (quit_ && tasks_.empty())
                        return;
                    task = std::move(tasks_.front());
                    tasks_.pop();
                }
                task();
            }
        });
        handle = Await(future);
        if (!handle.IsValid()) {
            host_.join();
            std::fprintf(stderr, "FAIL: CreateThread\n");
            std::_Exit(4);
        }
    }
    template <typename F> auto Submit(F fn) {
        using R = decltype(fn());
        auto task = std::make_shared<std::packaged_task<R()>>(std::move(fn));
        auto result = task->get_future();
        {
            std::lock_guard guard{mutex_};
            tasks_.emplace([task] { (*task)(); });
        }
        changed_.notify_one();
        return result;
    }
    auto Run(RunOptions options = {}) {
        return Submit([this, options] { return h_.context->Run(handle, options); });
    }
    std::uint64_t Tid() const { return tid_; }
    ~TestOwner() {
        auto requested = h_.context->RequestInterrupt(handle, InterruptReason::Shutdown);
        if (requested)
            (void)h_.context->WaitStopped(requested.Value(), 1'000'000'000);
        auto destroyed = Submit([this] { return h_.context->DestroyThread(handle); });
        auto status = Await(destroyed);
        if (!status) {
            std::fprintf(stderr, "FAIL: owner DestroyThread: %s\n",
                         Describe(status.GetError()).c_str());
            std::_Exit(4);
        }
        {
            std::lock_guard guard{mutex_};
            quit_ = true;
        }
        changed_.notify_one();
        host_.join();
    }
    ThreadHandle handle{};

  private:
    Harness &h_;
    std::thread host_;
    std::uint64_t tid_{};
    std::mutex mutex_;
    std::condition_variable changed_;
    std::queue<std::function<void()>> tasks_;
    bool quit_{};
};

std::uint64_t Progress(std::uint64_t address) {
    // The fixture uses aligned x86-64 qword stores, which this backend emits as
    // aligned ARM64 stores. A host atomic load observes actual guest memory work.
    return std::atomic_ref<std::uint64_t>(*reinterpret_cast<std::uint64_t *>(address)).load();
}
bool WaitProgress(std::uint64_t address, std::uint64_t before) {
    const auto until = std::chrono::steady_clock::now() + std::chrono::seconds(1);
    while (std::chrono::steady_clock::now() < until) {
        if (Progress(address) > before)
            return true;
        std::this_thread::yield();
    }
    return false;
}
std::uint64_t PrepareProgress(Harness &h, std::size_t slot = 0) {
    const auto address = h.stack_base + 0x100 + slot * 64;
    new (reinterpret_cast<void *>(address)) std::uint64_t{0};
    return address;
}
bool LoadProgress(Harness &h) {
    std::string error;
    const auto *fixture = FindFixture("progress_loop");
    const bool ok = fixture && LoadFixture(h, *fixture, error);
    if (!ok)
        Check("G11setup", "load progress fixture", false, error);
    return ok;
}

void TestAsyncInterrupt(Harness &h) {
    if (!LoadProgress(h))
        return;
    const auto progress = PrepareProgress(h);
    TestOwner owner(h, progress);
    auto run = owner.Run();
    Check("G11a", "guest store proves execution before pause", WaitProgress(progress, 0));
    auto ticket = h.context->RequestInterrupt(owner.handle, InterruptReason::Pause);
    Check("G11b", "request Pause", bool(ticket));
    auto receipt = h.context->WaitStopped(ticket.Value(), 1'000'000'000);
    auto paused = Await(run);
    Check("G11c", "owner Run returns PauseRequested and acknowledges",
          receipt && paused && paused.Value().primary_reason == StopReason::PauseRequested);
    auto read = h.context->ReadRegisters(owner.handle);
    Check("G11d", "paused registers readable and equal frozen guest store",
          receipt && read && read.Value().registers.Get(Gpr::Rax) == Progress(progress));
    auto again = h.context->WaitStopped(ticket.Value(), 1'000'000);
    Check("G11e", "repeated WaitStopped preserves the same stop epoch",
          receipt && again && receipt.Value().stop_epoch == again.Value().stop_epoch);
    Check("G11f", "receipt carries context and thread generation",
          receipt && receipt.Value().context_id == h.context->ContextId() &&
              receipt.Value().snapshot.thread_generation == owner.handle.generation);
    auto resumed = h.context->Resume(owner.handle, ticket.Value().epoch);
    Check("G11g", "Resume consumes exactly the acknowledged ticket", bool(resumed));
    auto stale = h.context->WaitStopped(ticket.Value(), 1'000'000);
    Check("G11h", "retired ticket cannot create a fresh SafePoint", !stale);
    const auto before = Progress(progress);
    auto rerun = owner.Run();
    Check("G11i", "resumed guest really advances its counter", WaitProgress(progress, before));
    auto cancel = h.context->RequestInterrupt(owner.handle, InterruptReason::Cancel);
    auto stopped = h.context->WaitStopped(cancel.Value(), 1'000'000'000);
    auto result = Await(rerun);
    Check("G11j", "Cancel returns and its ticket receives a Cancelled receipt",
          stopped && result && stopped.Value().reason == StopReason::Cancelled &&
              result.Value().primary_reason == StopReason::Cancelled);
}

void TestInterruptStress(Harness &h) {
    if (!LoadProgress(h))
        return;
    const auto progress = PrepareProgress(h);
    TestOwner owner(h, progress);
    std::vector<std::uint64_t> delays;
    bool valid = true, fast = true;
    for (int cycle = 0; cycle < 100; ++cycle) {
        auto before = Progress(progress);
        auto run = owner.Run();
        valid &= WaitProgress(progress, before);
        const auto start = std::chrono::steady_clock::now();
        auto ticket = h.context->RequestInterrupt(owner.handle, InterruptReason::Pause);
        auto receipt = h.context->WaitStopped(ticket.Value(), 1'000'000'000);
        const auto us = std::chrono::duration_cast<std::chrono::microseconds>(
                            std::chrono::steady_clock::now() - start)
                            .count();
        auto result = Await(run);
        valid &= bool(receipt) && bool(result) &&
                 result.Value().primary_reason == StopReason::PauseRequested;
        fast &= us <= 1'000'000;
        if (receipt) {
            valid &= receipt.Value().snapshot.registers.Get(Gpr::Rax) == Progress(progress);
            std::printf(
                "TRACE G13 iteration=%d request=%llu stop=%llu progress=%llu latency_us=%lld\n",
                cycle, (unsigned long long)ticket.Value().epoch,
                (unsigned long long)receipt.Value().stop_epoch,
                (unsigned long long)Progress(progress), (long long)us);
        }
        delays.push_back(us);
        valid &= bool(h.context->Resume(owner.handle, ticket.Value().epoch));
        // No settle sleep. The next iteration observes actual guest progress.
    }
    std::sort(delays.begin(), delays.end());
    Check("G13a", "100 cycles each execute, pause and publish fresh state", valid);
    Check("G13b", "request-to-ack budget includes RequestInterrupt itself", fast,
          "p50_us=" + std::to_string(delays[50]) + " p95_us=" + std::to_string(delays[95]) +
              " max_us=" + std::to_string(delays.back()));
    auto run = owner.Run();
    auto cancel = h.context->RequestInterrupt(owner.handle, InterruptReason::Cancel);
    auto receipt = h.context->WaitStopped(cancel.Value(), 1'000'000'000);
    auto result = Await(run);
    Check("G13c", "immediate-entry Cancel is acknowledged without a warmup gap",
          receipt && result && result.Value().primary_reason == StopReason::Cancelled);
}

void TestTwoOwnerConcurrency(Harness &h) {
    if (!LoadProgress(h))
        return;
    auto pa = PrepareProgress(h, 0), pb = PrepareProgress(h, 1);
    TestOwner a(h, pa), b(h, pb, 4096);
    auto ra = a.Run(), rb = b.Run();
    Check("G14a", "both guests perform memory stores before either is stopped",
          WaitProgress(pa, 0) && WaitProgress(pb, 0));
    Check("G14b", "owners have distinct actual gettid values",
          a.Tid() != b.Tid() && a.Tid() != 0 && b.Tid() != 0,
          "tid_a=" + std::to_string(a.Tid()) + " tid_b=" + std::to_string(b.Tid()));
    auto ta = h.context->RequestInterrupt(a.handle, InterruptReason::Pause);
    Check("G14c", "request owner A pause", bool(ta));
    auto sa = h.context->WaitStopped(ta.Value(), 1'000'000'000);
    auto stopped_a = Await(ra);
    Check("G14d", "A has a stopped receipt and returned to its command queue",
          sa && stopped_a && stopped_a.Value().primary_reason == StopReason::PauseRequested);
    const auto a0 = Progress(pa), b0 = Progress(pb);
    const bool b_advanced = WaitProgress(pb, b0 + 100);
    auto snapshot = h.context->ReadRegisters(a.handle);
    Check("G14e", "while A is frozen B performs new guest stores",
          b_advanced && Progress(pa) == a0 && snapshot &&
              snapshot.Value().registers.Get(Gpr::Rax) == a0,
          "A=" + std::to_string(a0) + " B=" + std::to_string(b0) + "->" +
              std::to_string(Progress(pb)));
    Check("G14f", "resume A", bool(h.context->Resume(a.handle, ta.Value().epoch)));
    ra = a.Run();
    const auto b1 = Progress(pb);
    Check("G14g", "both guests show new progress after A resumes",
          WaitProgress(pa, a0) && WaitProgress(pb, b1));
    auto ca = h.context->RequestInterrupt(a.handle, InterruptReason::Cancel);
    auto cb = h.context->RequestInterrupt(b.handle, InterruptReason::Cancel);
    auto aa = h.context->WaitStopped(ca.Value(), 1'000'000'000);
    auto ab = h.context->WaitStopped(cb.Value(), 1'000'000'000);
    auto xa = Await(ra), xb = Await(rb);
    Check("G14h", "both Cancels acknowledge and return normally",
          aa && ab && xa && xb && xa.Value().primary_reason == StopReason::Cancelled &&
              xb.Value().primary_reason == StopReason::Cancelled);
}

std::atomic<bool> hold_entered{false}, hold_release{false};
void HoldOwnerSignal(int) {
    hold_entered.store(true, std::memory_order_release);
    while (!hold_release.load(std::memory_order_acquire)) {
    }
}
void TestInterruptInterleavings(Harness &h) {
    if (!LoadProgress(h))
        return;
    auto progress = PrepareProgress(h);
    TestOwner owner(h, progress);
    bool priority = true, stale = true, pending = true;
    for (int i = 0; i < 100; ++i) {
        auto before = Progress(progress);
        auto run = owner.Run();
        pending &= WaitProgress(progress, before);
        std::barrier barrier(3);
        std::optional<InterruptTicket> pause, cancel;
        std::thread p([&] {
            barrier.arrive_and_wait();
            auto t = h.context->RequestInterrupt(owner.handle, InterruptReason::Pause);
            if (t)
                pause = t.Value();
        });
        std::thread c([&] {
            barrier.arrive_and_wait();
            auto t = h.context->RequestInterrupt(owner.handle, InterruptReason::Cancel);
            if (t)
                cancel = t.Value();
        });
        barrier.arrive_and_wait();
        p.join();
        c.join();
        if (!pause || !cancel) {
            priority = false;
            std::_Exit(4);
        }
        auto pa = h.context->WaitStopped(*pause, 1'000'000'000);
        auto ca = h.context->WaitStopped(*cancel, 1'000'000'000);
        auto result = Await(run);
        priority &= pa && ca && result && ca.Value().reason == StopReason::Cancelled &&
                    pa.Value().reason == StopReason::Cancelled;
        const auto low = std::min(pause->epoch, cancel->epoch),
                   high = std::max(pause->epoch, cancel->epoch);
        stale &= !h.context->Resume(owner.handle, low);
        auto still = h.context->WaitStopped(*cancel, 1'000'000);
        pending &= still && still.Value().reason == StopReason::Cancelled;
        pending &= bool(h.context->Resume(owner.handle, high));
    }
    Check("G15a", "100 concurrent Pause/Cancel pairs preserve Cancel priority", priority);
    Check("G15b", "older Resume never consumes a newer stopped request", stale && pending);

    // Force a genuine late ack without backend test hooks: a bounded unrelated
    // host signal temporarily holds the owner outside JIT entry checks.
    bool timeout_kept = true;
    struct sigaction action {
    }, previous{};
    action.sa_handler = HoldOwnerSignal;
    action.sa_flags = SA_ONSTACK;
    sigemptyset(&action.sa_mask);
    if (sigaction(SIGUSR1, &action, &previous) != 0) {
        Check("G15c", "install bounded host hold", false);
        return;
    }
    for (int i = 0; i < 100; ++i) {
        hold_entered.store(false);
        hold_release.store(false);
        auto before = Progress(progress);
        auto run = owner.Run();
        timeout_kept &= WaitProgress(progress, before);
        ::syscall(SYS_tgkill, ::getpid(), static_cast<pid_t>(owner.Tid()), SIGUSR1);
        auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(1);
        while (!hold_entered.load() && std::chrono::steady_clock::now() < deadline)
            std::this_thread::yield();
        if (!hold_entered.load()) {
            hold_release.store(true);
            std::_Exit(4);
        }
        auto t = h.context->RequestInterrupt(owner.handle, InterruptReason::Pause);
        auto early = h.context->WaitStopped(t.Value(), 1'000'000);
        timeout_kept &= !early && early.GetError().category == ErrorCategory::Timeout;
        hold_release.store(true, std::memory_order_release);
        auto late = h.context->WaitStopped(t.Value(), 1'000'000'000);
        auto result = Await(run);
        timeout_kept &= late && result && late.Value().request_epoch == t.Value().epoch;
        timeout_kept &= bool(h.context->Resume(owner.handle, t.Value().epoch));
    }
    sigaction(SIGUSR1, &previous, nullptr);
    Check("G15c", "100 timed-out waits retain requests and receive late owner acks", timeout_kept);

    // Ready-state request vs Run entry and immediate Resume/new Pause interleavings.
    bool entry_ok = true;
    for (int i = 0; i < 100; ++i) {
        auto p = h.context->RequestInterrupt(owner.handle, InterruptReason::Pause);
        auto a = h.context->WaitStopped(p.Value(), 1'000'000'000);
        auto run = owner.Run();
        auto result = Await(run);
        entry_ok &= a && result && result.Value().primary_reason == StopReason::PauseRequested;
        auto old_epoch = p.Value().epoch;
        std::barrier race(3);
        auto resume_race = std::async(std::launch::async, [&] {
            race.arrive_and_wait();
            return h.context->Resume(owner.handle, old_epoch);
        });
        auto pause_race = std::async(std::launch::async, [&] {
            race.arrive_and_wait();
            return h.context->RequestInterrupt(owner.handle, InterruptReason::Pause);
        });
        race.arrive_and_wait();
        auto resumed = Await(resume_race);
        auto next = Await(pause_race);
        entry_ok &= resumed || resumed.GetError().category == ErrorCategory::StaleEpoch;
        if (!next)
            std::_Exit(4);
        entry_ok &= !h.context->Resume(owner.handle, old_epoch);
        auto newer = h.context->WaitStopped(next.Value(), 1'000'000'000);
        entry_ok &= bool(newer) && bool(h.context->Resume(owner.handle, next.Value().epoch));
    }
    Check("G15d", "100 pre-entry stops and concurrent Resume/Pause races retain new requests",
          entry_ok);
    // Cancel after the owner has already returned from Pause must acknowledge too.
    auto p = h.context->RequestInterrupt(owner.handle, InterruptReason::Pause);
    auto pa = h.context->WaitStopped(p.Value(), 1'000'000'000);
    auto c = h.context->RequestInterrupt(owner.handle, InterruptReason::Cancel);
    auto ca = h.context->WaitStopped(c.Value(), 1'000'000'000);
    Check("G15e", "Cancel while paused has an immediate frozen-state receipt",
          pa && ca && ca.Value().reason == StopReason::Cancelled);
}

void TestStoppedStateExecution(Harness &h) {
    if (!LoadProgress(h))
        return;
    // Add the echo entry before any owner starts, preserving the spin entry.
    const auto *echo = FindFixture("state_echo");
    const auto entry = h.code_base + 0x1000;
    GuestRange all{GuestAddress{h.code_base}, kMappingSize};
    auto writable = h.space->Protect(all, GuestPermission::Read | GuestPermission::Write);
    if (!writable || !echo) {
        Check("G16setup", "prepare echo entry", false);
        return;
    }
    {
        auto pin = h.space->AcquirePinnedSpan({GuestAddress{entry}, echo->bytes.size()}, true);
        if (!pin)
            std::_Exit(4);
        auto bytes = pin.Value().WritableBytes();
        std::memcpy(bytes.data(), echo->bytes.data(), echo->bytes.size());
        if (echo->gate_offset >= 0)
            std::memcpy(bytes.data() + echo->gate_offset, &h.return_gate, 8);
    }
    (void)h.space->Protect(all, GuestPermission::Read | GuestPermission::Execute);
    {
        auto q = h.space->Quiesce(1'000'000'000);
        if (!q || !h.context->InvalidateCode(q.Value(), all, InvalidationReason::HostWrite))
            std::_Exit(4);
    }
    bool state_ok = true, refused = true, fp_ok = true;
    for (int i = 0; i < 10; ++i) {
        auto progress = PrepareProgress(h);
        const auto output = h.stack_base + 0x800, fs = h.stack_base + 0x600,
                   gs = h.stack_base + 0x608;
        *reinterpret_cast<std::uint64_t *>(fs) = 0x12345678 + i;
        *reinterpret_cast<std::uint64_t *>(gs) = 0x87654321 + i;
        TestOwner owner(h, progress);
        const int guest_round = i % 4;
        const int host_round = guest_round < 2 ? FE_UPWARD : FE_DOWNWARD;
        auto rounding = owner.Submit([host_round] {
            ::feraiseexcept(FE_INVALID);
            return ::fesetround(host_round);
        });
        (void)Await(rounding);
        auto run = owner.Run();
        state_ok &= WaitProgress(progress, 0);
        auto running_read = h.context->ReadRegisters(owner.handle);
        refused &=
            !running_read && running_read.GetError().category == ErrorCategory::AlreadyRunning;
        auto running_write = h.context->WriteRegisters(owner.handle, RegisterPatch{}, 1);
        refused &=
            !running_write && running_write.GetError().category == ErrorCategory::WrongThread;
        auto ticket = h.context->RequestInterrupt(owner.handle, InterruptReason::Pause);
        auto ack = h.context->WaitStopped(ticket.Value(), 1'000'000'000);
        auto paused = Await(run);
        if (!ack || !paused)
            std::_Exit(4);
        RegisterPatch patch{};
        patch.fields = RegisterValidity::Gpr | RegisterValidity::Rip | RegisterValidity::Xmm |
                       RegisterValidity::Mxcsr | RegisterValidity::SegmentBases;
        patch.gpr_mask = (1u << Index(Gpr::Rax)) | (1u << Index(Gpr::Rdi));
        patch.values.Set(Gpr::Rax, 0x1122334455667788ULL + i);
        patch.values.Set(Gpr::Rdi, output);
        patch.values.rip = entry;
        patch.xmm_mask = 3;
        patch.values.xmm[0] = {0x1020304050607080ULL, 0x8877665544332211ULL};
        double x = -1.75; // toward-zero (-1) differs from host FE_DOWNWARD (-2)
        std::memcpy(&patch.values.xmm[1].low, &x, 8);
        patch.values.mxcsr = 0x1f80 | (guest_round << 13);
        patch.values.fs_base = fs;
        patch.values.gs_base = gs;
        auto wrong = h.context->WriteRegisters(owner.handle, patch, ack.Value().stop_epoch);
        refused &= !wrong && wrong.GetError().category == ErrorCategory::WrongThread;
        auto stale = owner.Submit([&] {
            return h.context->WriteRegisters(owner.handle, patch, ack.Value().stop_epoch - 1);
        });
        auto st = Await(stale);
        refused &= !st && st.GetError().category == ErrorCategory::StaleEpoch;
        auto write = owner.Submit(
            [&] { return h.context->WriteRegisters(owner.handle, patch, ack.Value().stop_epoch); });
        state_ok &= bool(Await(write));
        auto resume = h.context->Resume(owner.handle, ticket.Value().epoch);
        state_ok &= bool(resume);
        run = owner.Run();
        auto result = Await(run);
        auto env = owner.Submit([host_round] {
            return ::fegetround() == host_round && (::fetestexcept(FE_INVALID) & FE_INVALID);
        });
        fp_ok &= Await(env);
        auto *bytes = reinterpret_cast<const std::byte *>(output);
        std::uint64_t rax, xlo, xhi, fsv, gsv, rounded;
        std::uint32_t mxcsr;
        std::memcpy(&rax, bytes, 8);
        std::memcpy(&xlo, bytes + 8, 8);
        std::memcpy(&xhi, bytes + 16, 8);
        std::memcpy(&mxcsr, bytes + 24, 4);
        std::memcpy(&fsv, bytes + 32, 8);
        std::memcpy(&gsv, bytes + 40, 8);
        std::memcpy(&rounded, bytes + 48, 8);
        const bool ok = result && result.Value().primary_reason == StopReason::Returned &&
                        rax == patch.values.Get(Gpr::Rax) && xlo == patch.values.xmm[0].low &&
                        xhi == patch.values.xmm[0].high &&
                        (mxcsr & 0x6000) == (patch.values.mxcsr & 0x6000) &&
                        fsv == 0x12345678ULL + i && gsv == 0x87654321ULL + i &&
                        rounded == static_cast<std::uint64_t>(guest_round < 2 ? -2LL : -1LL);
        if (!ok)
            std::printf(
                "TRACE G16 i=%d rax=%llx xmm=%llx:%llx csr=%x fs=%llx gs=%llx round=%llu "
                "reason=%s\n",
                i, (unsigned long long)rax, (unsigned long long)xhi, (unsigned long long)xlo, mxcsr,
                (unsigned long long)fsv, (unsigned long long)gsv, (unsigned long long)rounded,
                result ? std::string(ToString(result.Value().primary_reason)).c_str() : "error");
        state_ok &= ok;
    }
    Check("G16a", "10 paused owner patches affect actual GPR/RIP/XMM/MXCSR/FS/GS execution",
          state_ok);
    Check("G16b", "running reads and wrong-owner/stale writes are rejected", refused);
    Check("G16c", "all four guest rounding modes preserve host rounding and exception flags",
          fp_ok);
}

void TestRetiredContextsAndFaultPriority(Harness &h) {
    bool retired = true, fault_priority = true;
    for (int i = 0; i < 100; ++i) {
        std::string error;
        if (!LoadFixture(h, *FindFixture("bare_hlt"), error))
            std::_Exit(4);
        ThreadInit init{};
        init.entry_rip = GuestCodeAddress{h.code_base};
        init.initial_rsp = GuestAddress{h.stack_top};
        auto old = h.context->CreateThread(init);
        if (!old)
            std::_Exit(4);
        auto fault = h.context->Run(old.Value(), {});
        auto cancel = h.context->RequestInterrupt(old.Value(), InterruptReason::Cancel);
        auto receipt = h.context->WaitStopped(cancel.Value(), 1'000'000'000);
        fault_priority &= fault && receipt && receipt.Value().reason == StopReason::GuestFault &&
                          receipt.Value().snapshot.kind == SnapshotKind::Faulted &&
                          !h.context->Resume(old.Value(), cancel.Value().epoch) &&
                          !h.context->Run(old.Value(), {});
        const auto old_context = h.context->ContextId();
        if (!h.context->DestroyThread(old.Value()))
            std::_Exit(4);
        retired &= !h.context->WaitStopped(cancel.Value(), 1);
        h.context.reset();
        auto fresh = CreateContext(CpuConfig{}, *h.space);
        if (!fresh)
            std::_Exit(4);
        h.context = std::move(fresh).Value();
        h.return_gate = h.context->Capabilities().return_gate_address;
        auto replacement = h.context->CreateThread(init);
        if (!replacement)
            std::_Exit(4);
        retired &= h.context->ContextId() != old_context && replacement.Value() != old.Value() &&
                   !h.context->WaitStopped(cancel.Value(), 1) &&
                   !h.context->RequestInterrupt(old.Value(), InterruptReason::Pause) &&
                   !h.context->ReadRegisters(old.Value()) &&
                   !h.context->Resume(old.Value(), cancel.Value().epoch) &&
                   !h.context->Run(old.Value(), {});
        if (!h.context->DestroyThread(replacement.Value()))
            std::_Exit(4);
    }
    Check("G17b", "100 destroyed/recreated contexts reject real old tickets and handles", retired);
    Check("G17c", "100 real HLT faults outrank subsequent Cancel and refuse Resume",
          fault_priority);
}

void TestInterruptRefusals(Harness &harness) {
    const auto *fixture = FindFixture("return_only");
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
    TestInterruptInterleavings(harness);
    TestStoppedStateExecution(harness);
    Check("G17a", "all test owners destroyed their own threads",
          harness.context->LiveThreadCount() == 0);
    TestPinnedInvalidationRecovery(harness);
    printf("\n");
    // Contract checks last: they publish their own stub at the same address.
    TestContracts(harness);
    TestRetiredContextsAndFaultPriority(harness);

    printf("\n%d check(s), %s (%d failure%s)\n", g_checks, g_failures == 0 ? "ALL PASS" : "FAILED",
           g_failures, g_failures == 1 ? "" : "s");
    printf("SCOPE: real x86-64 executed by FEXCore through the public API, from\n");
    printf("       assembler-generated fixtures, two owners and boundary interrupts. No "
           "HLE/Step/APK.\n");
    return g_failures == 0 ? 0 : 1;
}
