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
#include "core/guest_cpu/fex/fex_context.h"
#include "core/guest_cpu/hle/call_adapter.h"

#include "guest_fixtures.h"

namespace {

using namespace Core::GuestCpu;
using namespace Core::GuestCpu::Hle;

// Native HLE functions for the real guest->host gate test. Pure and stateless so their return
// value is entirely determined by the SysV arguments the guest placed, proving the crossing
// actually marshalled registers.
extern "C" std::uint64_t HleAdd6(std::uint64_t a, std::uint64_t b, std::uint64_t c,
                                 std::uint64_t d, std::uint64_t e, std::uint64_t f) {
    return a + b + c + d + e + f;
}
extern "C" std::uint64_t HleSpill(std::uint64_t a, std::uint64_t b, std::uint64_t c,
                                  std::uint64_t d, std::uint64_t e, std::uint64_t f,
                                  std::uint64_t g, std::uint64_t h) {
    // 8 integer args: a-f in rdi/rsi/rdx/r10/r8/r9 (callgate), g-h spilled on the stack.
    return a + b + c + d + e + f + g + h;
}
extern "C" double HleSum4Double(double a, double b, double c, double d) {
    return a + b + c + d;
}

// H2 buffer marshalling: counts how many times native code was entered so negative cases can prove
// the host function was never reached. Bounded buffers pin count elements across the whole call;
// the native side sees the host pointer and an element count.
using Hle::GuestBoundedBuffer;
std::atomic<int> g_hle_buffer_calls{0};
extern "C" int HleBufferCopySum(GuestBoundedBuffer<const uint64_t> in,
                                GuestBoundedBuffer<uint64_t> out) {
    g_hle_buffer_calls.fetch_add(1, std::memory_order_relaxed);
    if (in.data == nullptr || out.data == nullptr) {
        return -1;
    }
    const uint64_t n = std::min(in.count, out.count);
    uint64_t sum = 0;
    for (uint64_t i = 0; i < n; ++i) {
        sum += in.data[i];
    }
    out.data[0] = sum;
    return 0;
}

Xmm EncodeDouble(double value) {
    std::uint64_t bits{};
    std::memcpy(&bits, &value, sizeof(bits));
    return Xmm{bits, 0};
}

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
constexpr std::uint64_t kDataOffset = 0x30000;
constexpr std::uint64_t kMappingSize = 0x4000;

struct Harness final {
    std::unique_ptr<GuestAddressSpace> space;
    std::unique_ptr<CpuContext> context;
    std::uint64_t code_base{};
    std::uint64_t stack_base{};
    std::uint64_t stack_top{};
    std::uint64_t data_base{};
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
// Runs whatever is at the code base once on an ephemeral guest thread and returns its rax. Used by
// version/guest-store publication tests that only need to read back the executed constant.
std::optional<std::uint64_t> RunConstantOnce(Harness &h, std::uint64_t gtid = 1) {
    ThreadInit init{};
    init.entry_rip = GuestCodeAddress{h.code_base};
    init.initial_rsp = GuestAddress{h.stack_top};
    init.guest_tid = gtid;
    auto thread = h.context->CreateThread(init);
    if (!thread)
        return std::nullopt;
    auto run = h.context->Run(thread.Value(), RunOptions{});
    std::optional<std::uint64_t> rax;
    if (run && run.Value().primary_reason == StopReason::Returned)
        rax = run.Value().snapshot.registers.Get(Gpr::Rax);
    (void)h.context->DestroyThread(thread.Value());
    return rax;
}

bool PublishViaPublicApi(Harness &harness, const Fixtures::Fixture &fixture, std::string &error) {    const GuestRange range{GuestAddress{harness.code_base}, kMappingSize};

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
    GuestRange range{GuestAddress{harness.code_base}, kMappingSize};
    if (!harness.space->Protect(range, GuestPermission::Read | GuestPermission::Write | GuestPermission::Execute)) {
        Check("G10a", "make code writable before failure injection", false); return;
    }
    std::uint64_t rewarmed{}; std::string rewarm_error;
    if (!RunPublishedCode(harness, rewarmed, rewarm_error) || rewarmed != 17) {
        Check("G10a", "rewarm before pinned-write failure", false); return;
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

// --- R2-M01: coordinated quiesce publishes new code to two running owners ---------------------
//
// Two owners run the progress fixture. QuiesceContext stops both and yields a token; under it we
// publish the constant-B code and discard the old translations. After the token drops, both owners
// resume and run the new code, and each returns the new constant rather than the old. The
// coordinator, not the test, must stop both owners: stopping them one at a time would leave a
// window in which one re-enters old code.
bool PublishBytesWithToken(Harness &h, const Fixtures::Fixture &fixture,
                           const QuiescenceToken &token, std::string &error) {
    const GuestRange range{GuestAddress{h.code_base}, kMappingSize};
    std::vector<std::byte> image(kMappingSize, std::byte{0});
    if (fixture.bytes.size() > image.size()) {
        error = "fixture too large";
        return false;
    }
    std::memcpy(image.data(), fixture.bytes.data(), fixture.bytes.size());
    if (fixture.gate_offset >= 0) {
        std::memcpy(image.data() + fixture.gate_offset, &h.return_gate, sizeof(h.return_gate));
    }
    auto published = h.space->PublishCode(token, range, image);
    if (!published) {
        error = "PublishCode: " + Describe(published.GetError());
        return false;
    }
    return true;
}

void TestCoordinatedPublication(Harness &h) {
    if (!LoadProgress(h))
        return;

    // Two owners run the progress loop concurrently, so both are truly inside Run.
    auto pa = PrepareProgress(h, 0), pb = PrepareProgress(h, 1);
    TestOwner a(h, pa), b(h, pb, 4096);
    auto ra = a.Run(), rb = b.Run();
    if (!(WaitProgress(pa, 0) && WaitProgress(pb, 0))) {
        Check("G20", "both owners warm before quiesce", false);
        return;
    }

    bool ok = true;
    std::string detail;

    // Coordinated stop of both owners in one transaction. The token is scoped so it releases before
    // we expect owners to run again.
    {
        auto token = h.context->QuiesceContext(1'000'000'000);
        if (!token) {
            Check("G20", "QuiesceContext stops two running owners", false,
                  Describe(token.GetError()));
            return;
        }

        // While the token lives, new Run must be rejected by the execution-lease gate.
        auto blocked_future = a.Run();
        auto blocked_result = Await(blocked_future);
        if (blocked_result.HasValue()) {
            ok = false;
            detail = "Run was admitted during quiescence";
        }

        // Both owners must have stopped (their first Runs returned PauseRequested).
        auto xa = Await(ra), xb = Await(rb);
        if (!xa.HasValue() || !xb.HasValue() ||
            xa.Value().primary_reason != StopReason::PauseRequested ||
            xb.Value().primary_reason != StopReason::PauseRequested) {
            ok = false;
            detail = "owners did not stop under quiesce";
        }

        Check("G20a", "QuiesceContext stops both owners and refuses new execution", ok, detail);
        // token released at block end.
    }

    const auto before_a = Progress(pa), before_b = Progress(pb);

    // The coordinator consumed its own pauses, so the owners' next Run executes fresh rather than
    // returning paused. Both must keep progressing in the same code, proving the transaction ended
    // cleanly rather than wedging the context.
    ra = a.Run();
    rb = b.Run();
    const bool progressed_after = WaitProgress(pa, before_a) && WaitProgress(pb, before_b);
    Check("G20b", "both owners resume and progress after quiesce", progressed_after);
}

// Publish a fixture's bytes at the code base under an already-held token, leaving the mapping RW so
// the caller controls when to re-grant execute. Returns the image size in bytes written.
bool PublishImageUnderToken(Harness &h, const Fixtures::Fixture &fixture,
                            const QuiescenceToken &token, std::string &error) {
    const GuestRange range{GuestAddress{h.code_base}, kMappingSize};
    std::vector<std::byte> image(kMappingSize, std::byte{0});
    if (fixture.bytes.size() > image.size()) {
        error = "fixture too large";
        return false;
    }
    std::memcpy(image.data(), fixture.bytes.data(), fixture.bytes.size());
    if (fixture.gate_offset >= 0) {
        std::memcpy(image.data() + fixture.gate_offset, &h.return_gate, sizeof(h.return_gate));
    }
    auto published = h.space->PublishCode(token, range, image);
    if (!published) {
        error = "PublishCode: " + Describe(published.GetError());
        return false;
    }
    return true;
}

// Serial fresh-thread publication smoke. This is auxiliary to R2-M01 only: it does not
// retain two native owners or warmed guest handles across the 100 iterations.
void TestCoordinatedVersionSwitch(Harness &h) {
    const auto *a_fixture = FindFixture("constant_a");
    const auto *b_fixture = FindFixture("constant_b");
    if (!a_fixture || !b_fixture) {
        Check("G21", "constant_a/b fixtures present", false);
        return;
    }
    {
        std::string e;
        if (!PublishViaPublicApi(h, *a_fixture, e)) {
            Check("G21", "publish initial constant_a", false, e);
            return;
        }
    }

    auto run_constant = [&](std::uint64_t gtid, std::uint64_t stack_delta)
                            -> std::optional<std::uint64_t> {
        ThreadInit init{};
        init.entry_rip = GuestCodeAddress{h.code_base};
        init.initial_rsp = GuestAddress{h.stack_top - stack_delta};
        init.guest_tid = gtid;
        auto created = h.context->CreateThread(init);
        if (!created)
            return std::nullopt;
        auto result = h.context->Run(created.Value(), RunOptions{});
        std::optional<std::uint64_t> rax;
        if (result && result.Value().primary_reason == StopReason::Returned) {
            rax = result.Value().snapshot.registers.Get(Gpr::Rax);
        }
        (void)h.context->DestroyThread(created.Value());
        return rax;
    };

    bool all_new = true;
    std::string detail;
    for (int epoch = 0; epoch < 100; ++epoch) {  // G22
        const bool use_b = (epoch % 2) == 0;
        const std::uint64_t expected = use_b ? 34u : 17u;
        const Fixtures::Fixture &fixture = use_b ? *b_fixture : *a_fixture;

        // A context quiesce with no running owners still yields a token (coordinator readiness).
        // Publish the new bytes under it.
        {
            auto token = h.context->QuiesceContext(1'000'000'000);
            if (!token) {
                all_new = false;
                detail = "epoch " + std::to_string(epoch) + ": quiesce: " +
                         Describe(token.GetError());
                break;
            }
            auto w = h.space->ReprotectUnderToken(
                token.Value(), GuestRange{GuestAddress{h.code_base}, kMappingSize},
                GuestPermission::Read | GuestPermission::Write);
            std::string pub_error;
            if (!w || !PublishImageUnderToken(h, fixture, token.Value(), pub_error)) {
                all_new = false;
                detail = "epoch " + std::to_string(epoch) + ": " +
                         (w ? pub_error : Describe(w.GetError()));
                break;
            }
            auto rx = h.space->ReprotectUnderToken(token.Value(),
                GuestRange{GuestAddress{h.code_base}, kMappingSize},
                GuestPermission::Read | GuestPermission::Execute);
            if (!rx) {
                all_new = false;
                detail = "Reprotect(RX): " + Describe(rx.GetError());
                break;
            }
        }

        // Two fresh guest handles execute serially on this same host thread.
        auto ra_a = run_constant(200 + epoch * 2, 0);
        auto ra_b = run_constant(201 + epoch * 2, 8192);
        if (!ra_a || !ra_b || *ra_a != expected || *ra_b != expected) {
            all_new = false;
            detail = "epoch " + std::to_string(epoch) + ": expected " + std::to_string(expected) +
                     " got a=" + (ra_a ? std::to_string(*ra_a) : "none") +
                     " b=" + (ra_b ? std::to_string(*ra_b) : "none");
            break;
        }
    }

    Check("G21a", "100 serialized fresh-thread publication smoke iterations", all_new,
          detail);
}

// Serial fresh-thread remap smoke, auxiliary to R2-M03. Publish/Remap also invalidate,
// so this cannot prove Clear independently; G23 supplies that distinct regression.
void TestCoordinatedRemap(Harness &h) {
    const auto *a_fixture = FindFixture("constant_a");
    const auto *b_fixture = FindFixture("constant_b");
    if (!a_fixture || !b_fixture) {
        Check("G22", "constant_a/b fixtures present", false);
        return;
    }
    const GuestRange code{GuestAddress{h.code_base}, kMappingSize};

    std::string e;
    if (!PublishViaPublicApi(h, *a_fixture, e)) {
        Check("G22", "publish initial constant_a", false, e);
        return;
    }

    auto run_constant = [&](std::uint64_t gtid, std::uint64_t stack_delta)
                            -> std::optional<std::uint64_t> {
        ThreadInit init{};
        init.entry_rip = GuestCodeAddress{h.code_base};
        init.initial_rsp = GuestAddress{h.stack_top - stack_delta};
        init.guest_tid = gtid;
        auto created = h.context->CreateThread(init);
        if (!created)
            return std::nullopt;
        auto result = h.context->Run(created.Value(), RunOptions{});
        std::optional<std::uint64_t> rax;
        if (result && result.Value().primary_reason == StopReason::Returned)
            rax = result.Value().snapshot.registers.Get(Gpr::Rax);
        (void)h.context->DestroyThread(created.Value());
        return rax;
    };

    bool ok = true;
    std::string detail;
    for (int epoch = 0; epoch < 100; ++epoch) {  // G22
        const bool use_b = (epoch % 2) == 0;
        const std::uint64_t expected = use_b ? 34u : 17u;
        const Fixtures::Fixture &fixture = use_b ? *b_fixture : *a_fixture;
        const std::uint64_t mapping_before = h.space->MappingGeneration();
        const std::uint64_t code_before = h.space->CodeGeneration();

        {
            auto token = h.context->QuiesceContext(1'000'000'000);
            if (!token) {
                ok = false;
                detail = "quiesce: " + Describe(token.GetError());
                break;
            }
            // Fresh backing at the same VA.
            auto remapped = h.space->RemapUnderToken(
                token.Value(), code, GuestPermission::Read | GuestPermission::Write);
            if (!remapped) {
                ok = false;
                detail = "remap: " + Describe(remapped.GetError());
                break;
            }
            // The remap zeroes the backing: the previous fixture's first bytes must be gone.
            {
                std::byte first = static_cast<std::byte>(0xAB);
                auto pin = h.space->AcquirePinnedSpan(
                    GuestRange{GuestAddress{h.code_base}, 1}, /*writable=*/false);
                if (pin)
                    first = pin.Value().Bytes()[0];
                if (first != std::byte{0}) {
                    ok = false;
                    detail = "remap did not zero the backing (old bytes survived)";
                    break;
                }
            }
            // Publish the new bytes over the fresh backing, then clear the whole JIT.
            std::string pub_error;
            if (!PublishImageUnderToken(h, fixture, token.Value(), pub_error)) {
                ok = false;
                detail = pub_error;
                break;
            }
            auto cleared = h.context->ClearCodeCache(token.Value());
            if (!cleared) {
                ok = false;
                detail = "ClearCodeCache: " + Describe(cleared.GetError());
                break;
            }
            auto rx = h.space->ReprotectUnderToken(token.Value(), code,
                GuestPermission::Read | GuestPermission::Execute);
            if (!rx) {
                ok = false;
                detail = "Reprotect(RX): " + Describe(rx.GetError());
                break;
            }
        }

        const std::uint64_t mapping_after = h.space->MappingGeneration();
        const std::uint64_t code_after = h.space->CodeGeneration();
        if (mapping_after <= mapping_before || code_after <= code_before) {
            ok = false;
            detail = "generations did not advance: mapping " + std::to_string(mapping_before) +
                     "->" + std::to_string(mapping_after) + " code " +
                     std::to_string(code_before) + "->" + std::to_string(code_after);
            break;
        }

        auto ra = run_constant(300 + epoch * 2, 0);
        auto rb = run_constant(301 + epoch * 2, 8192);
        if (!ra || !rb || *ra != expected || *rb != expected) {
            ok = false;
            detail = "epoch " + std::to_string(epoch) + " expected " + std::to_string(expected) +
                     " got " + (ra ? std::to_string(*ra) : "none") + "/" +
                     (rb ? std::to_string(*rb) : "none");
            break;
        }
    }

    Check("G22a", "100 serialized fresh-thread remap smoke iterations", ok,
          detail);
}

std::atomic<bool> hold_entered{false}, hold_release{false};
void HoldOwnerSignal(int) {
    hold_entered.store(true, std::memory_order_release);
    while (!hold_release.load(std::memory_order_acquire)) {
    }
}
// Compare every public architectural field, without padding or cache-generation metadata.
constexpr auto kArchitecturalFields = RegisterValidity::Gpr | RegisterValidity::Rip |
    RegisterValidity::Rflags | RegisterValidity::Xmm | RegisterValidity::Mxcsr | RegisterValidity::SegmentBases;
bool SameRegisters(const RegisterFile& x, const RegisterFile& y) {
    return HasAll(x.validity, kArchitecturalFields) && x.validity == y.validity &&
           x.gpr == y.gpr && x.rip == y.rip && x.rflags == y.rflags && x.xmm == y.xmm &&
           x.mxcsr == y.mxcsr && x.fs_base == y.fs_base && x.gs_base == y.gs_base;
}
bool SameArchitecture(const CpuSnapshot& a, const CpuSnapshot& b) {
    return SameRegisters(a.registers, b.registers) && a.thread_id == b.thread_id && a.thread_generation == b.thread_generation &&
           a.stop_epoch == b.stop_epoch && a.invocation_id == b.invocation_id && a.kind == b.kind;
}
// Regression for the reviewed ClearCodeCache path: keep the SAME warmed native thread,
// write B while its mapping is RWX, and do not use PublishCode/Remap to invalidate on its behalf.
void TestFullCacheRetirement(Harness& h) {
    std::string error;
    if (!LoadFixture(h, *FindFixture("constant_a"), error)) {
        Check("G23setup", "load constant A", false, error); return;
    }
    ThreadInit init{};
    init.entry_rip = {h.code_base}; init.initial_rsp = {h.stack_top};
    init.initial_state.fields = RegisterValidity::Gpr | RegisterValidity::Xmm |
        RegisterValidity::Mxcsr | RegisterValidity::SegmentBases;
    init.initial_state.gpr_mask = 0xffffu & ~(1u << Index(Gpr::Rsp));
    init.initial_state.xmm_mask = 0xffff;
    for (std::size_t i = 0; i < kGprCount; ++i) init.initial_state.values.gpr[i] = 0x12340000 + i;
    for (std::size_t i = 0; i < kXmmCount; ++i) init.initial_state.values.xmm[i] = {0xABCD0000 + i, 0xDCBA0000 + i};
    init.initial_state.values.mxcsr = 0x3f80;
    init.initial_state.values.fs_base = h.data_base;
    init.initial_state.values.gs_base = h.data_base + 128;
    std::array<std::byte, 128> memory{};
    memory.fill(std::byte{0xA5});
    if (!h.space->Write({h.data_base}, memory)) std::_Exit(4);
    auto thread = h.context->CreateThread(init);
    if (!thread) { Check("G23setup", "create persistent thread", false); return; }
    auto first = h.context->Run(thread.Value(), {});
    bool warm = first && first.Value().snapshot.registers.Get(Gpr::Rax) == 17;
    {
        AddressSpaceConfig config{};
        config.reservation_size = 1ULL << 20;
        auto other = GuestAddressSpace::Create(config);
        if (!other) std::_Exit(4);
        auto foreign = other.Value()->Quiesce(1000);
        auto refused = h.context->ClearCodeCache(foreign.Value());
        Check("G23a", "ClearCodeCache refuses another address space's token",
              !refused && refused.Category() == ErrorCategory::InvalidArgument);
    }
    const GuestRange range{{h.code_base}, kMappingSize};
    bool cleared = false, preserved = false;
    {
        auto token = h.context->QuiesceContext(1'000'000'000);
        if (!token) std::_Exit(4);
        auto before = h.context->ReadRegisters(thread.Value());
        auto rx_clear = h.context->ClearCodeCache(token.Value());
        auto again = h.context->InvalidateCode(token.Value(), range, InvalidationReason::HostWrite);
        Check("G23b", "RX Clear followed by ordinary invalidation does not deadlock", rx_clear && again);
        // Warm A again below before the RW test; RX clear alone must preserve guest state.
        auto after = h.context->ReadRegisters(thread.Value());
        std::array<std::byte, 128> after_memory{};
        preserved = before && after && SameArchitecture(before.Value(), after.Value()) &&
                    h.space->Read({h.data_base}, after_memory) && memory == after_memory;
    }
    auto restart = [&] {
        auto snapshot = h.context->ReadRegisters(thread.Value());
        RegisterPatch patch{}; patch.fields = RegisterValidity::Rip; patch.values.rip = h.code_base;
        if (!h.context->WriteRegisters(thread.Value(), patch, snapshot.Value().stop_epoch)) std::_Exit(4);
        return h.context->Run(thread.Value(), {});
    };
    if (!h.space->Protect(range, GuestPermission::Read | GuestPermission::Write |
                                  GuestPermission::Execute)) std::_Exit(4);
    auto rewarmed = restart();
    warm &= rewarmed && rewarmed.Value().snapshot.registers.Get(Gpr::Rax) == 17;
    {
        auto token = h.context->QuiesceContext(1'000'000'000);
        if (!token) std::_Exit(4);
        const auto* fixture = FindFixture("constant_b");
        std::memcpy(reinterpret_cast<void*>(h.code_base), fixture->bytes.data(), fixture->bytes.size());
        std::memcpy(reinterpret_cast<void*>(h.code_base + fixture->gate_offset), &h.return_gate, 8);
        auto before = h.context->ReadRegisters(thread.Value());
        cleared = bool(h.context->ClearCodeCache(token.Value()));
        auto after = h.context->ReadRegisters(thread.Value());
        preserved &= before && after && SameArchitecture(before.Value(), after.Value());
    }
    auto fresh = restart();
    if (rewarmed && fresh) {
        auto expected = rewarmed.Value().snapshot.registers;
        expected.Set(Gpr::Rax, 34); // The new fixture deliberately changes this result only.
        preserved &= SameRegisters(expected, fresh.Value().snapshot.registers);
        std::array<std::byte, 128> after_memory{};
        preserved &= h.space->Read({h.data_base}, after_memory) && memory == after_memory;
    }
    Check("G23c", "same warmed RWX thread executes B after Clear alone (no permission mutation)",
          warm && cleared && preserved && fresh && fresh.Value().snapshot.registers.Get(Gpr::Rax) == 34);
    (void)h.context->DestroyThread(thread.Value());
}

// --- N1 / R2-M01: the SAME two persistent owners switch versions across 100 epochs ------------
//
// Two native owners are created once and kept for the whole test (handle and generation invariant).
// Each epoch the coordinator stops both while they are running version X, republishes version Y at
// the code base, and on resume both owners re-dispatch and must write Y's marker while their counter
// keeps advancing. Fresh threads are never used: this proves the running owners' cached decode is
// what got replaced on the same handle/TID, which a fresh-thread smoke cannot show.
// Shared body for G25 (publish) and G26 (same-VA remap). When do_remap is true the code VA is
// remapped to a fresh zeroed backing under the token before publishing, so the old backing is
// physically replaced -- not merely overwritten.
void RunPersistentOwnerSwitch(Harness& h, const char* id, bool do_remap) {
    const auto* va = FindFixture("version_a_loop");
    const auto* vb = FindFixture("version_b_loop");
    if (!va || !vb) {
        Check(id, "version_a/b loop fixtures present", false);
        return;
    }

    // Two progress slots: qword[0]=version marker, qword[8]=advancing counter.
    auto slot_addr = [&](int owner) -> std::uint64_t {
        return h.stack_base + 0x400 + owner * 128;
    };
    for (int owner = 0; owner < 2; ++owner) {
        new (reinterpret_cast<void*>(slot_addr(owner))) std::uint64_t{0};
        new (reinterpret_cast<void*>(slot_addr(owner)+8)) std::uint64_t{0};
    }
    auto marker_of = [&](int owner) { return Progress(slot_addr(owner)); };
    auto counter_of = [&](int owner) { return Progress(slot_addr(owner)+8); };
    constexpr std::uint64_t kMarkerA = 0x0A0A0A0A0A0A0A0AULL;
    constexpr std::uint64_t kMarkerB = 0x0B0B0B0B0B0B0B0BULL;

    // Publish A first (memory-only path is fine: no owners are running yet).
    {
        std::string e;
        if (!PublishViaPublicApi(h, *va, e)) {
            Check(id, "publish initial version A", false, e);
            return;
        }
    }

    // Two persistent owners. They are created once and destroyed once at the end; every epoch
    // reuses these exact handles.
    std::optional<TestOwner> owner_a_storage, owner_b_storage;
    owner_a_storage.emplace(h, slot_addr(0));
    owner_b_storage.emplace(h, slot_addr(1), 4096);
    TestOwner& oa = *owner_a_storage;
    TestOwner& ob = *owner_b_storage;

    for (TestOwner* own : {&oa, &ob}) {
        auto seeded = own->Submit([&] {
            auto snap = h.context->ReadRegisters(own->handle);
            if (!snap) return false;
            RegisterPatch patch{};
            patch.fields = RegisterValidity::Gpr | RegisterValidity::Xmm | RegisterValidity::Mxcsr | RegisterValidity::SegmentBases;
            patch.gpr_mask = 0xffffu & ~(1u << Index(Gpr::Rsp)) & ~(1u << Index(Gpr::Rdi));
            patch.xmm_mask = 0xffff;
            for (std::size_t i = 0; i < kGprCount; ++i) patch.values.gpr[i] = 0x12340000 + i;
            for (std::size_t i = 0; i < kXmmCount; ++i) patch.values.xmm[i] = {0xABCD0000 + i, 0xDCBA0000 + i};
            patch.values.mxcsr = 0x3f80;
            patch.values.fs_base = h.data_base;
            patch.values.gs_base = h.data_base + 128;
            return bool(h.context->WriteRegisters(own->handle, patch, snap.Value().stop_epoch));
        });
        if (!Await(seeded)) std::_Exit(4);
    }

    auto run_both = [&] {
        auto fa = oa.Run();
        auto fb = ob.Run();
        return std::pair{std::move(fa), std::move(fb)};
    };

    auto [fa0, fb0] = run_both();
    // Wait until both markers and counters appear.
    const auto warm_deadline = std::chrono::steady_clock::now() + std::chrono::seconds(2);
    auto warmed = [&] {
        while (std::chrono::steady_clock::now() < warm_deadline) {
            if (marker_of(0) == kMarkerA && marker_of(1) == kMarkerA &&
                counter_of(0) > 0 && counter_of(1) > 0)
                return true;
            std::this_thread::yield();
        }
        return false;
    };
    if (!warmed()) {
        Check(id, "both persistent owners warm on version A", false);
        return;
    }
    const auto a_id = oa.handle, b_id = ob.handle;
    const auto a_tid = oa.Tid(), b_tid = ob.Tid();

    bool ok = true;
    std::string detail;
    for (int epoch = 0; epoch < 100; ++epoch) {
        const bool to_b = (epoch % 2) == 0;
        const std::uint64_t want_marker = to_b ? kMarkerB : kMarkerA;
        const Fixtures::Fixture& fixture = to_b ? *vb : *va;

        std::uint64_t stop_a{}, stop_b{}, token_epoch{};
        const std::uint64_t c0_before = counter_of(0), c1_before = counter_of(1);

        {
            auto token = h.context->QuiesceContext(1'000'000'000);
            if (!token) {
                ok = false;
                detail = "epoch " + std::to_string(epoch) + ": quiesce " +
                         Describe(token.GetError());
                break;
            }
            auto stopped_a = Await(fa0), stopped_b = Await(fb0);
            if (!stopped_a || !stopped_b ||
                stopped_a.Value().primary_reason != StopReason::PauseRequested ||
                stopped_b.Value().primary_reason != StopReason::PauseRequested ||
                stopped_a.Value().thread_id != a_id.id || stopped_b.Value().thread_id != b_id.id ||
                stopped_a.Value().thread_generation != a_id.generation ||
                stopped_b.Value().thread_generation != b_id.generation ||
                oa.Tid() != a_tid || ob.Tid() != b_tid || token.Value().StoppedThreadCount() != 2) {
                ok = false; detail = "owner/stop identity changed"; break;
            }
            stop_a = stopped_a.Value().stop_epoch;
            stop_b = stopped_b.Value().stop_epoch;
            token_epoch = token.Value().Epoch();
            std::array<std::uint64_t, 4> before_slots{marker_of(0), counter_of(0), marker_of(1), counter_of(1)};
            // Both owners' runs return under the drain.
            const GuestRange range{GuestAddress{h.code_base}, kMappingSize};
            if (do_remap) {
                // Replace the backing with a fresh zeroed (RW) one at the same VA. The publish
                // below supplies the bytes; this proves the old backing, not just the bytes, is gone.
                auto remapped = h.space->RemapUnderToken(
                    token.Value(), range, GuestPermission::Read | GuestPermission::Write);
                if (!remapped) {
                    ok = false;
                    detail = "epoch " + std::to_string(epoch) + ": remap " +
                             Describe(remapped.GetError());
                    break;
                }
            } else {
                auto rw = h.space->ReprotectUnderToken(
                    token.Value(), range, GuestPermission::Read | GuestPermission::Write);
                if (!rw) {
                    ok = false;
                    detail = "epoch " + std::to_string(epoch) + ": RW " + Describe(rw.GetError());
                    break;
                }
            }
            std::string pub_e;
            if (!PublishImageUnderToken(h, fixture, token.Value(), pub_e)) {
                ok = false;
                detail = "epoch " + std::to_string(epoch) + ": publish " + pub_e;
                break;
            }
            auto rx = h.space->ReprotectUnderToken(
                token.Value(), range, GuestPermission::Read | GuestPermission::Execute);
            if (!rx) {
                ok = false;
                detail = "epoch " + std::to_string(epoch) + ": RX " + Describe(rx.GetError());
                break;
            }

            auto state_a = h.context->ReadRegisters(oa.handle), state_b = h.context->ReadRegisters(ob.handle);
            std::array<std::uint64_t, 4> after_slots{marker_of(0), counter_of(0), marker_of(1), counter_of(1)};
            if (!state_a || !state_b || !SameArchitecture(stopped_a.Value().snapshot, state_a.Value()) ||
                !SameArchitecture(stopped_b.Value().snapshot, state_b.Value()) || before_slots != after_slots) {
                ok = false; detail = "publication changed stopped architectural state or data"; break;
            }

            // Reset each owner's RIP to the new fixture entry while the token is held: every owner
            // is parked and its command queue is drained here, so there is no in-flight blocking
            // Run ahead of this task to deadlock against. WriteRegisters applies on the owner thread
            // at the stopped safe point.
            for (TestOwner* own : {&oa, &ob}) {
                auto reset = own->Submit([&] {
                    auto snap = h.context->ReadRegisters(own->handle);
                    if (!snap)
                        return std::string{"ReadRegisters: " + Describe(snap.GetError())};
                    RegisterPatch patch{};
                    patch.fields = RegisterValidity::Rip;
                    patch.values.rip = h.code_base;
                    auto written = h.context->WriteRegisters(own->handle, patch,
                                                             snap.Value().stop_epoch);
                    if (!written)
                        return std::string{"WriteRegisters: " + Describe(written.GetError())};
                    return std::string{};
                });
                auto reset_err = Await(reset);
                if (!reset_err.empty()) {
                    ok = false;
                    detail = "epoch " + std::to_string(epoch) + ": RIP reset: " + reset_err;
                    break;
                }
            }
            if (!ok)
                break;
            // token released at block end.
        }
        if (!ok)
            break;
        fa0 = oa.Run();
        fb0 = ob.Run();
        // Both blocking Runs are now in flight on their owners; they are observed below.
        //
        // Marker flip proves the freshly published code is executing; the version loop resets its
        // own counter on entry (xor rax,rax), so the counter is NOT monotonic across versions.
        // Instead: wait for both markers to match, then wait for both counters to advance while
        // the new marker is held -- proving live new code on each owner's independent slot -- and
        // verify both independent slots make progress (equal counter values are legal).
        const auto until = std::chrono::steady_clock::now() + std::chrono::seconds(3);
        bool flipped = false;
        while (std::chrono::steady_clock::now() < until) {
            if (marker_of(0) == want_marker && marker_of(1) == want_marker &&
                counter_of(0) > 0 && counter_of(1) > 0) {
                flipped = true;
                break;
            }
            std::this_thread::yield();
        }
        bool live = false;
        if (flipped) {
            const std::uint64_t s0 = counter_of(0), s1 = counter_of(1);
            const auto grow_until = std::chrono::steady_clock::now() + std::chrono::seconds(2);
            while (std::chrono::steady_clock::now() < grow_until) {
                if (counter_of(0) > s0 && counter_of(1) > s1) {
                    live = true;
                    break;
                }
                std::this_thread::yield();
            }
        }
        printf("EPOCH {\"case\":\"%s\",\"epoch\":%d,\"tid_a\":%llu,\"tid_b\":%llu,"
               "\"handle_a\":%llu,\"handle_b\":%llu,\"generation_a\":%llu,\"generation_b\":%llu,"
               "\"stop_a\":%llu,\"stop_b\":%llu,\"token\":%llu,\"old_counter_a\":%llu,\"old_counter_b\":%llu,"
               "\"new_marker_a\":%llu,\"new_marker_b\":%llu,\"new_counter_a\":%llu,\"new_counter_b\":%llu,\"ok\":%s}\n",
               id, epoch, (unsigned long long)a_tid, (unsigned long long)b_tid,
               (unsigned long long)a_id.id, (unsigned long long)b_id.id,
               (unsigned long long)a_id.generation, (unsigned long long)b_id.generation,
               (unsigned long long)stop_a, (unsigned long long)stop_b, (unsigned long long)token_epoch,
               (unsigned long long)c0_before, (unsigned long long)c1_before,
               (unsigned long long)marker_of(0), (unsigned long long)marker_of(1),
               (unsigned long long)counter_of(0), (unsigned long long)counter_of(1), flipped && live ? "true" : "false");
        if (!(flipped && live)) {
            ok = false;
            detail = "epoch " + std::to_string(epoch) + " want marker " + Hex(want_marker) +
                     " got m0=" + Hex(marker_of(0)) + " m1=" + Hex(marker_of(1)) +
                     " c0=" + std::to_string(counter_of(0)) + " c1=" + std::to_string(counter_of(1)) +
                     " flipped=" + std::to_string(flipped) + " live=" + std::to_string(live);
            break;
        }
    }

    if (fa0.valid() || fb0.valid()) {
        auto final_stop = h.context->QuiesceContext(1'000'000'000);
        if (!final_stop) std::_Exit(4);
        if (fa0.valid()) ok &= bool(Await(fa0));
        if (fb0.valid()) ok &= bool(Await(fb0));
    }
    Check((do_remap ? "G26a" : "G25a"), "100 epochs: the SAME two persistent owners switch versions and advance", ok,
          detail);
    Check((do_remap ? "G26b" : "G25b"), "persistent handles stayed stable",
          oa.handle.IsValid() && ob.handle.IsValid() && oa.Tid() != ob.Tid());
}

void TestPersistentOwnerVersionSwitch(Harness& h) {
    RunPersistentOwnerSwitch(h, "G25", /*do_remap=*/false);
}
void TestPersistentOwnerRemap(Harness& h) {
    RunPersistentOwnerSwitch(h, "G26", /*do_remap=*/true);
}

// Install initial fixtures at independent mapped addresses. Only setup uses host fixture bytes.
bool InstallAt(Harness& h, const Fixtures::Fixture& fixture, std::uint64_t address) {
    auto q = h.context->QuiesceContext(1'000'000'000);
    if (!q) return false;
    GuestRange range{{address}, kMappingSize};
    if (!h.space->ReprotectUnderToken(q.Value(), range, GuestPermission::Read | GuestPermission::Write)) return false;
    std::vector<std::byte> image(kMappingSize);
    std::memcpy(image.data(), fixture.bytes.data(), fixture.bytes.size());
    if (fixture.gate_offset >= 0) std::memcpy(image.data()+fixture.gate_offset, &h.return_gate, 8);
    return h.space->PublishCode(q.Value(), range, image) &&
           h.space->ReprotectUnderToken(q.Value(), range, GuestPermission::Read | GuestPermission::Execute);
}

Result<RunResult> RestartAt(Harness& h, ThreadHandle handle, std::uint64_t rip,
                            std::optional<std::uint32_t> immediate = {}) {
    auto snap = h.context->ReadRegisters(handle);
    if (!snap) return snap.GetError();
    RegisterPatch patch{}; patch.fields = RegisterValidity::Rip; patch.values.rip = rip;
    if (immediate) {
        patch.fields |= RegisterValidity::Gpr;
        patch.gpr_mask = 1u << Index(Gpr::Rsi); patch.values.Set(Gpr::Rsi, *immediate);
    }
    auto written = h.context->WriteRegisters(handle, patch, snap.Value().stop_epoch);
    if (!written) return written.GetError();
    return h.context->Run(handle, {});
}

void TestGuestStorePublication(Harness& h) {
    auto must = [&](bool value, const char* operation) {
        if (!value) { Check("G27setup", operation, false); std::_Exit(4); }
    };
    must(InstallAt(h, *FindFixture("constant_a"), h.code_base), "install target A");
    must(InstallAt(h, *FindFixture("smc_writer"), h.data_base), "install independent writer");
    {
        TestOwner a(h, 0), b(h, 0, 4096);
        const auto aid = a.handle, bid = b.handle;
        const auto atid = a.Tid(), btid = b.Tid();
        ThreadInit init{}; init.entry_rip = {h.data_base}; init.initial_rsp = {h.stack_top - 8192};
        init.initial_state.fields = RegisterValidity::Gpr;
        init.initial_state.gpr_mask = 1u << Index(Gpr::Rdi);
        init.initial_state.values.Set(Gpr::Rdi, h.code_base);
        auto writer = h.context->CreateThread(init); must(bool(writer), "create writer");
        std::uint32_t previous = 17;
        bool ok = true;
        for (int epoch = 0; epoch < 10; ++epoch) {
            auto ra = a.Submit([&] { return RestartAt(h, a.handle, h.code_base); });
            auto rb = b.Submit([&] { return RestartAt(h, b.handle, h.code_base); });
            auto va = Await(ra), vb = Await(rb);
            ok &= va && vb && va.Value().snapshot.registers.Get(Gpr::Rax) == previous &&
                  vb.Value().snapshot.registers.Get(Gpr::Rax) == previous;
            GuestRange target{{h.code_base}, kMappingSize};
            std::array<std::byte, 5> before_bytes{};
            must(bool(h.space->Read(target.base, before_bytes)), "read actual old target bytes");
            std::uint32_t before_immediate{}; std::memcpy(&before_immediate, before_bytes.data()+1, 4);
            ok &= before_bytes[0] == std::byte{0xB8} && before_immediate == previous;
            {
                auto q = h.context->QuiesceContext(1'000'000'000); must(bool(q), "stop target owners");
                must(bool(h.space->ReprotectUnderToken(q.Value(), target,
                    GuestPermission::Read | GuestPermission::Write)), "revoke target X");
            }
            // Targets are stopped with X revoked and all old target translations retired.
            // Only the independent RX writer runs in this explicitly admitted interval.
            const std::uint32_t version = 34 + epoch;
            auto wrote = RestartAt(h, writer.Value(), h.data_base, version);
            must(wrote && wrote.Value().primary_reason == StopReason::Returned, "guest writer returns");
            std::array<std::byte, 5> bytes{};
            must(bool(h.space->Read(target.base, bytes)), "read guest-authored target");
            std::uint32_t observed{}; std::memcpy(&observed, bytes.data()+1, 4);
            ok &= bytes[0] == std::byte{0xB8} && observed == version;
            {
                auto q = h.context->QuiesceContext(1'000'000'000); must(bool(q), "publication quiesce");
                // No host image or memcpy: the guest already wrote the target's actual bytes.
                must(bool(h.space->InvalidateCode(q.Value(), target, InvalidationReason::GuestPublish)), "publish guest stores");
                must(bool(h.space->ReprotectUnderToken(q.Value(), target,
                    GuestPermission::Read | GuestPermission::Execute)), "commit RX under token");
            }
            ra = a.Submit([&] { return RestartAt(h, a.handle, h.code_base); });
            rb = b.Submit([&] { return RestartAt(h, b.handle, h.code_base); });
            va = Await(ra); vb = Await(rb);
            ok &= va && vb && va.Value().snapshot.registers.Get(Gpr::Rax) == version &&
                  vb.Value().snapshot.registers.Get(Gpr::Rax) == version &&
                  va.Value().thread_id == aid.id && vb.Value().thread_id == bid.id &&
                  va.Value().thread_generation == aid.generation && vb.Value().thread_generation == bid.generation &&
                  a.Tid() == atid && b.Tid() == btid;
            printf("STORE {\"epoch\":%d,\"old\":%u,\"old_immediate\":%u,\"guest_immediate\":%u,"
                   "\"target_a\":%llu,\"target_b\":%llu,\"generation_a\":%llu,\"generation_b\":%llu,"
                   "\"tid_a\":%llu,\"tid_b\":%llu,\"result_a\":%llu,\"result_b\":%llu,\"ok\":%s}\n",
                   epoch, previous, before_immediate, observed, (unsigned long long)aid.id, (unsigned long long)bid.id,
                   (unsigned long long)aid.generation, (unsigned long long)bid.generation,
                   (unsigned long long)atid, (unsigned long long)btid,
                   (unsigned long long)(va ? va.Value().snapshot.registers.Get(Gpr::Rax) : 0),
                   (unsigned long long)(vb ? vb.Value().snapshot.registers.Get(Gpr::Rax) : 0), ok?"true":"false");
            previous = version;
        }
        must(bool(h.context->DestroyThread(writer.Value())), "destroy writer");
        Check("G27a", "10 varying guest stores directly modify hot code reused by both original owners", ok);
    }
    must(bool(h.space->Protect({{h.data_base}, kMappingSize}, GuestPermission::Read | GuestPermission::Write)), "restore data mapping");
}

void TestPermissionRetirement(Harness& h) {
    bool protected_old = true, rewritten = true;
    // A separately cached caller jumps into a second mapping. Removing X/unmapping the target
    // must retire block links as well as the entry lookup; a check of Run's initial RIP cannot pass.
    for (int mode = 0; mode < 3; ++mode) {
        if (!InstallAt(h, *FindFixture("jump_target"), h.code_base) ||
            !InstallAt(h, *FindFixture("constant_a"), h.data_base)) std::_Exit(4);
        ThreadInit init{}; init.entry_rip = {h.code_base}; init.initial_rsp = {h.stack_top};
        init.initial_state.fields = RegisterValidity::Gpr; init.initial_state.gpr_mask = 1u << Index(Gpr::Rdi);
        init.initial_state.values.Set(Gpr::Rdi, h.data_base);
        auto thread = h.context->CreateThread(init); if (!thread) std::_Exit(4);
        auto warm = h.context->Run(thread.Value(), {});
        protected_old &= warm && warm.Value().snapshot.registers.Get(Gpr::Rax) == 17;
        GuestRange target{{h.data_base}, kMappingSize};
        if (mode == 0) {
            auto q = h.context->QuiesceContext(1'000'000'000); if (!q) std::_Exit(4);
            if (!h.space->ReprotectUnderToken(q.Value(), target, GuestPermission::Read | GuestPermission::Write)) std::_Exit(4);
        } else if (mode == 1) {
            if (!h.space->Protect(target, GuestPermission::Read | GuestPermission::Write)) std::_Exit(4);
        } else if (!h.space->Unmap(target)) std::_Exit(4);
        auto denied = RestartAt(h, thread.Value(), h.code_base);
        protected_old &= !denied || denied.Value().primary_reason == StopReason::GuestFault;
        if (!h.context->DestroyThread(thread.Value())) std::_Exit(4);
        if (mode == 2 && !h.space->Map(target, GuestPermission::Read | GuestPermission::Write)) std::_Exit(4);
    }
    // Reprotect's documented automatic retirement permits omitted explicit Clear/Publish here,
    // but the existing thread must execute B, never the old A.
    if (!InstallAt(h, *FindFixture("constant_a"), h.code_base)) std::_Exit(4);
    ThreadInit init{}; init.entry_rip = {h.code_base}; init.initial_rsp = {h.stack_top};
    auto thread = h.context->CreateThread(init); if (!thread) std::_Exit(4);
    auto warm = h.context->Run(thread.Value(), {});
    {
        auto q = h.context->QuiesceContext(1'000'000'000); if (!q) std::_Exit(4);
        GuestRange range{{h.code_base}, kMappingSize};
        if (!h.space->ReprotectUnderToken(q.Value(), range, GuestPermission::Read | GuestPermission::Write)) std::_Exit(4);
        const std::uint32_t b = 34; std::memcpy(reinterpret_cast<void*>(h.code_base+1), &b, 4);
        if (!h.space->ReprotectUnderToken(q.Value(), range, GuestPermission::Read | GuestPermission::Execute)) std::_Exit(4);
    }
    auto fresh = RestartAt(h, thread.Value(), h.code_base);
    rewritten &= warm && fresh && warm.Value().snapshot.registers.Get(Gpr::Rax) == 17 &&
                 fresh.Value().snapshot.registers.Get(Gpr::Rax) == 34;
    if (!h.context->DestroyThread(thread.Value())) std::_Exit(4);
    Check("G28a", "cached cross-block jumps cannot enter token/plain NX or an unmapped range", protected_old);
    Check("G28b", "RW rewrite then RX on the same thread automatically retires A before B", rewritten);
}

// --- R2-H05: an unregistered-entry fault is attributed to its owner, not shared -----------------
//
// One owner executes an entry with no HLE/gate path (a bare HLT faults the guest). That owner must
// return GuestFault; a concurrently running owner on a normal fixture must NOT see the fault and
// must keep running and then cancel cleanly. Repeated 100 times with mixed ordering, the fault is
// never consumed by the wrong owner.
void TestFaultAttribution(Harness& h) {
    // Good code (progress) lives at code_base; the faulting fixture (bare HLT) lives on a SEPARATE
    // page (data_base) so one thread can spin on progress while another faults, concurrently.
    if (!LoadProgress(h)) {
        Check("G30", "load progress fixture", false);
        return;
    }
    const auto* bare = FindFixture("bare_hlt");
    if (!bare) {
        Check("G30", "bare_hlt fixture present", false);
        return;
    }
    const GuestRange fault_range{GuestAddress{h.data_base}, kMappingSize};
    {
        std::string e;
        // Publish the faulting bytes to the data page directly (no gate patch needed; HLT faults
        // before any return).
        auto rw = h.space->Protect(fault_range, GuestPermission::Read | GuestPermission::Write);
        auto pin = h.space->AcquirePinnedSpan(fault_range, /*writable=*/true);
        if (!rw || !pin) {
            Check("G30", "map data page RW for fault code", false,
                  rw ? Describe(pin.GetError()) : Describe(rw.GetError()));
            return;
        }
        auto bytes = pin.Value().WritableBytes();
        std::memset(bytes.data(), 0, bytes.size());
        std::memcpy(bytes.data(), bare->bytes.data(), bare->bytes.size());
    }
    if (!h.space->Protect(fault_range, GuestPermission::Read | GuestPermission::Execute)) {
        Check("G30", "publish fault page RX", false);
        return;
    }

    bool good_never_faulted = true;
    bool fault_owner_always_faulted = true;
    bool good_progressed = false;

    for (int iter = 0; iter < 100; ++iter) {
        const auto good_slot = PrepareProgress(h, 0);

        // Start the good owner on the progress loop first so it is executing before the fault.
        TestOwner good(h, good_slot);
        auto good_run = good.Run();
        if (!WaitProgress(good_slot, 0)) {
            good_never_faulted = false;
            break;
        }

        // Fault owner: fresh thread running bare HLT on the data page -> GuestFault.
        ThreadInit bad{};
        bad.entry_rip = GuestCodeAddress{h.data_base};
        bad.initial_rsp = GuestAddress{h.stack_top};
        bad.guest_tid = 500 + iter;
        auto bad_thread = h.context->CreateThread(bad);
        if (!bad_thread) {
            fault_owner_always_faulted = false;
            break;
        }
        auto bad_result = h.context->Run(bad_thread.Value(), RunOptions{});
        const bool bad_faulted =
            bad_result && bad_result.Value().primary_reason == StopReason::GuestFault;
        (void)h.context->DestroyThread(bad_thread.Value());
        if (!bad_faulted) {
            fault_owner_always_faulted = false;
            break;
        }

        // The good owner must still be running its own progress code, not stopped by the fault.
        const auto before = Progress(good_slot);
        if (!WaitProgress(good_slot, before)) {
            good_never_faulted = false;
            break;
        }
        // Cancel the good owner; it must return Cancelled, never GuestFault.
        auto cancel = h.context->RequestInterrupt(good.handle, InterruptReason::Cancel);
        auto receipt = h.context->WaitStopped(cancel.Value(), 1'000'000'000);
        auto good_result = Await(good_run);
        const bool good_clean =
            receipt && good_result && good_result.Value().primary_reason == StopReason::Cancelled;
        if (!good_clean) {
            good_never_faulted = false;
            break;
        }
        good_progressed = true;
    }

    Check("G30a", "the faulting owner returns GuestFault every round", fault_owner_always_faulted);
    Check("G30b", "the concurrent owner never inherits the fault and runs its own code",
          good_never_faulted && good_progressed);
}

void TestRealHleGate(Harness& h) {
    auto* registry =
        static_cast<HleCallRegistry*>(Fex::FexHleRegistryPointer(*h.context));
    if (registry == nullptr) {
        Check("G31", "HLE registry reachable on the FEX backend", false);
        return;
    }

    const auto op_add6 = registry->Register(&HleAdd6, "add6");
    const auto op_spill = registry->Register(&HleSpill, "spill");
    const auto op_dbl = registry->Register(&HleSum4Double, "sum4double");
    if (!op_add6 || !op_spill || !op_dbl) {
        Check("G31", "register typed HLE functions", false);
        return;
    }

    std::string e;
    auto run_with = [&](const Fixtures::Fixture& fixture, const RegisterPatch& initial,
                        std::optional<CpuSnapshot>& out) -> bool {
        ThreadInit init{};
        init.entry_rip = GuestCodeAddress{h.code_base};
        init.initial_rsp = GuestAddress{h.stack_top};
        init.guest_tid = 600;
        init.initial_state = initial;
        auto thr = h.context->CreateThread(init);
        if (!thr) return false;
        auto run = h.context->Run(thr.Value(), RunOptions{});
        if (run) out = run.Value().snapshot;
        (void)h.context->DestroyThread(thr.Value());
        return bool(run) && run.Value().primary_reason == StopReason::Returned;
    };

    bool int6_ok = true, spill_ok = true, dbl_ok = true;
    for (int i = 0; i < 10; ++i) {
        // 6 integer args, callgate: 4th integer is in r10.
        if (!LoadFixture(h, *FindFixture("hle_call_returns_rax"), e)) {
            Check("G31", "load HLE integer fixture", false, e);
            return;
        }
        RegisterPatch p{};
        p.fields = RegisterValidity::Gpr;
        p.gpr_mask = (1u << Index(Gpr::Rax)) | (1u << Index(Gpr::Rdi)) |
                     (1u << Index(Gpr::Rsi)) | (1u << Index(Gpr::Rdx)) |
                     (1u << Index(Gpr::R10)) | (1u << Index(Gpr::R8)) |
                     (1u << Index(Gpr::R9));
        p.values.Set(Gpr::Rax, op_add6.Value());
        p.values.Set(Gpr::Rdi, 1);
        p.values.Set(Gpr::Rsi, 2);
        p.values.Set(Gpr::Rdx, 3);
        p.values.Set(Gpr::R10, 4);  // 4th integer via callgate r10
        p.values.Set(Gpr::R8, 5);
        p.values.Set(Gpr::R9, 6);
        std::optional<CpuSnapshot> snap;
        if (!run_with(*FindFixture("hle_call_returns_rax"), p, snap) || !snap ||
            snap->registers.Get(Gpr::Rax) != 21) {
            int6_ok = false;
            break;
        }

        // 8 integer args: g,h (7th,8th) spill to the guest stack.
        RegisterPatch ps{};
        ps.fields = RegisterValidity::Gpr;
        ps.gpr_mask = p.gpr_mask;
        ps.values = p.values;
        ps.values.Set(Gpr::Rax, op_spill.Value());
        ps.values.Set(Gpr::Rdi, 1);
        ps.values.Set(Gpr::Rsi, 2);
        ps.values.Set(Gpr::Rdx, 3);
        ps.values.Set(Gpr::R10, 4);
        ps.values.Set(Gpr::R8, 5);
        ps.values.Set(Gpr::R9, 6);
        // Stack slots for the 7th/8th integer args. The callgate cursor reads spills above the
        // return-address slot: seed a dummy return slot at [rsp], arg7 at [rsp+8], arg8 at [rsp+16].
        // Use a dedicated aligned top for the spill frame.
        const std::uint64_t spill_base = h.stack_base + 0x600;
        const GuestAddress spill_top{spill_base};
        std::vector<std::uint64_t> spill_frame{0x0 /*ret addr*/, 7, 8};
        (void)h.space->Write(spill_top,
                            {reinterpret_cast<std::byte*>(spill_frame.data()),
                             spill_frame.size() * sizeof(std::uint64_t)});
        ThreadInit spill_init{};
        spill_init.entry_rip = GuestCodeAddress{h.code_base};
        spill_init.initial_rsp = GuestAddress{spill_base};
        spill_init.guest_tid = 601;
        spill_init.initial_state = ps;
        auto spill_thr = h.context->CreateThread(spill_init);
        std::optional<CpuSnapshot> snaps;
        bool spill_run = false;
        if (spill_thr) {
            auto r = h.context->Run(spill_thr.Value(), RunOptions{});
            if (r && r.Value().primary_reason == StopReason::Returned) {
                snaps = r.Value().snapshot;
                spill_run = snaps->registers.Get(Gpr::Rax) == 36;
            }
            (void)h.context->DestroyThread(spill_thr.Value());
        }
        if (!spill_run) {
            spill_ok = false;
            break;
        }

        // 4 doubles -> double return.
        if (!LoadFixture(h, *FindFixture("hle_call_returns_xmm"), e)) {
            Check("G31", "load HLE xmm fixture", false, e);
            return;
        }
        RegisterPatch pd{};
        pd.fields = RegisterValidity::Gpr | RegisterValidity::Xmm;
        pd.gpr_mask = 1u << Index(Gpr::Rax) | 1u << Index(Gpr::Rdi);
        pd.xmm_mask = 0b1111;  // xmm0..3 hold the four double args; rdi is the output slot
        pd.values.Set(Gpr::Rax, op_dbl.Value());
        pd.values.Set(Gpr::Rdi, h.stack_base + 0x800);
        pd.values.xmm[0] = EncodeDouble(0.25);
        pd.values.xmm[1] = EncodeDouble(0.25);
        pd.values.xmm[2] = EncodeDouble(0.5);
        pd.values.xmm[3] = EncodeDouble(1.0);
        std::optional<CpuSnapshot> snapd;
        if (!run_with(*FindFixture("hle_call_returns_xmm"), pd, snapd)) {
            dbl_ok = false;
            break;
        }
        double stored{};
        {
            std::array<std::byte, 8> b{};
            auto rd = h.space->Read(GuestAddress{h.stack_base + 0x800}, b);
            std::memcpy(&stored, b.data(), 8);
            if (!rd || stored < 1.99 || stored > 2.01) {
                dbl_ok = false;
                break;
            }
        }
    }

    Check("G31a", "real guest syscall gate returns the 6-integer sum", int6_ok);
    Check("G31b", "stack-spilled 7th/8th integer args cross the gate", spill_ok);
    Check("G31c", "4-double arguments produce a double host return", dbl_ok);
}

void TestHleBufferPinning(Harness& h) {
    auto* registry =
        static_cast<HleCallRegistry*>(Fex::FexHleRegistryPointer(*h.context));
    if (registry == nullptr) {
        Check("G32", "HLE registry reachable", false);
        return;
    }
    const auto op = registry->Register(&HleBufferCopySum, "buffer-copy-sum");
    if (!op) {
        Check("G32", "register buffer HLE function", false);
        return;
    }

    std::string e;
    if (!LoadFixture(h, *FindFixture("hle_call_returns_rax"), e)) {
        Check("G32", "load HLE fixture", false, e);
        return;
    }

    // Guest buffers live on the guest stack/data region.
    const std::uint64_t in_buf = h.stack_base + 0x500;
    const std::uint64_t out_buf = h.stack_base + 0x600;

    auto run_buffer = [&](std::uint64_t in_ptr, std::uint64_t out_ptr, std::uint64_t count,
                          bool expect_ok) -> bool {
        ThreadInit init{};
        init.entry_rip = GuestCodeAddress{h.code_base};
        init.initial_rsp = GuestAddress{h.stack_top};
        init.guest_tid = 700;
        init.initial_state.fields = RegisterValidity::Gpr;
        init.initial_state.gpr_mask = (1u << Index(Gpr::Rax)) | (1u << Index(Gpr::Rdi)) |
                                      (1u << Index(Gpr::Rsi)) | (1u << Index(Gpr::Rdx)) |
                                      (1u << Index(Gpr::R10));
        init.initial_state.values.Set(Gpr::Rax, op.Value());
        // BoundedBuffer in:  pointer(rdi), count(rsi). BoundedBuffer out: pointer(rdx), count(r10).
        init.initial_state.values.Set(Gpr::Rdi, in_ptr);
        init.initial_state.values.Set(Gpr::Rsi, count);
        init.initial_state.values.Set(Gpr::Rdx, out_ptr);
        init.initial_state.values.Set(Gpr::R10, count);
        auto thr = h.context->CreateThread(init);
        if (!thr) return !expect_ok;
        auto run = h.context->Run(thr.Value(), RunOptions{});
        auto destroyed = h.context->DestroyThread(thr.Value());
        (void)destroyed;
        if (expect_ok) {
            return run && run.Value().primary_reason == StopReason::Returned;
        }
        // A refused HLE call runs but reports a per-thread GuestFault (not a returned result): the
        // guest reached the gate but the marshalled call was rejected before native entry.
        return bool(run) && run.Value().primary_reason == StopReason::GuestFault;
    };

    // Positive: seed in[] = {10,20,30}, out[] receives 60.
    std::vector<std::uint64_t> input{10, 20, 30};
    (void)h.space->Write(GuestAddress{in_buf},
                         {reinterpret_cast<std::byte*>(input.data()), input.size() * 8});
    const int calls_before = g_hle_buffer_calls.load();
    bool good = run_buffer(in_buf, out_buf, 3, true);
    std::uint64_t result{};
    auto rd = h.space->Read(GuestAddress{out_buf},
                            {reinterpret_cast<std::byte*>(&result), 8});
    good &= rd && result == 60 &&
            g_hle_buffer_calls.load() == calls_before + 1;

    // Negatives: bad input pointer (unmapped high address), and a count that overflows past the
    // reservation. In every case the native function must not run (call count unchanged) and the
    // guest Run must be refused.
    bool bad = true; std::string bad_detail;
#define BADSTEP(cond, what) do { if(!(cond)){ bad=false; bad_detail=what; fprintf(stderr,"[g32] FAIL %s\n", what);} } while(0)
    BADSTEP(run_buffer(0xFFFF00000000ULL, out_buf, 1, false), "bad input");
    BADSTEP(run_buffer(in_buf, 0xFFFF00000000ULL, 1, false), "bad output");
    BADSTEP(run_buffer(in_buf, out_buf, 0xFFFFFFFFFFFFFFULL, false), "overflow");
    BADSTEP(g_hle_buffer_calls.load() == calls_before + 1, "no native entry");

    Check("G32a", "pinned in/out buffers marshal and the host writes the result", good);
    Check("G32b", "bad/overflow pointers never reach native code", bad, bad_detail);
}

void TestCoordinatorRecovery(Harness& h) {
    // External Pause/Cancel/Shutdown both before and after the coordinator's own request, with one
    // owner provably held at a lock-free point while the other owner stops independently.
    //
    // Determinism: instead of an arbitrary-PC SIGUSR1 freeze (which could land while FEX held its
    // context-wide block-link rwlock and serially block the other owner -- the G24 flake), this uses
    // a test-only Run-entry gate (FexTestRunGate): the held owner's Run parks AFTER the backend set
    // running/lease and released every coordinator/context/address-space lock, BEFORE entering the
    // JIT -- never in a signal handler, never holding a FEX lock. The second owner runs the real JIT
    // and drains/stops on its own. Releasing the gate lets the held owner enter the JIT where it
    // services the already-pending Pause/Cancel at the block-entry fault page.
    Fex::FexTestRunGate* gate =
        static_cast<Fex::FexTestRunGate*>(Fex::FexTestRunGatePointer(*h.context));

    // Per-stage first-failure detail. A Check is emitted every iteration so the 100-iteration
    // identity and the runner's repeat/worst-verdict accounting are preserved.
    std::string iter_failure;
    auto record = [&](const char* stage, int iter, bool ok, std::string detail = {}) {
        if (!ok) {
            const std::string msg = std::string(stage) + (detail.empty() ? "" : (": " + detail));
            std::fprintf(stderr, "[g24] FAIL iter=%d order=%d stage=%s %s\n",
                         iter, iter % 2, stage, detail.c_str());
            if (iter_failure.empty()) iter_failure = msg;
        }
        return ok;
    };

    for (int iteration = 0; iteration < 100; ++iteration) {
        iter_failure.clear();
        const int ordering = iteration % 2;
        const auto external_reason = static_cast<InterruptReason>((iteration / 2) % 3);
        const auto expected_reason = external_reason == InterruptReason::Pause ?
                                     StopReason::PauseRequested : StopReason::Cancelled;
        if (!LoadProgress(h)) return;
        const auto progress = PrepareProgress(h), second_progress = PrepareProgress(h, 1);
        TestOwner owner(h, progress), second(h, second_progress, 4096);

        // Hold the first owner at its lock-free Run-entry safe point before it enters the JIT.
        if (!gate || !gate->Arm(owner.handle.id)) std::_Exit(4);
        auto run = owner.Run();
        {
            const auto ad = std::chrono::steady_clock::now() + std::chrono::seconds(2);
            while (!gate->Arrived() && std::chrono::steady_clock::now() < ad) std::this_thread::yield();
            if (!gate->Arrived()) std::_Exit(4);
        }
        // The second owner is not gated; it runs the real guest JIT and must drain independently.
        auto second_run = second.Run();
        if (!WaitProgress(second_progress, 0)) std::_Exit(4);

        const auto until = std::chrono::steady_clock::now() + std::chrono::seconds(1);
        std::optional<InterruptTicket> cancel;
        if (ordering == 0) cancel = h.context->RequestInterrupt(owner.handle, external_reason).Value();
        auto draining = std::async(std::launch::async, [&] { return h.context->QuiesceContext(200'000'000); });
        while (!h.space->IsQuiescent() && std::chrono::steady_clock::now() < until) std::this_thread::yield();
        ThreadInit init{}; init.entry_rip = {h.code_base}; init.initial_rsp = {h.stack_top};
        auto extra = h.context->CreateThread(init);
        bool closed = true;
        closed &= record("admission CreateThread refused while draining", iteration,
                         !extra && extra.Category() == ErrorCategory::Busy,
                         extra ? "CreateThread was accepted during drain (expected Busy)"
                               : Describe(extra.GetError()));
        if (extra) (void)h.context->DestroyThread(extra.Value());
        if (ordering == 1) cancel = h.context->RequestInterrupt(owner.handle, external_reason).Value();
        auto concurrent = h.context->QuiesceContext(1'000'000);
        closed &= record("concurrent QuiesceContext refused with Busy", iteration,
                         !concurrent && concurrent.Category() == ErrorCategory::Busy,
                         concurrent ? "second quiesce succeeded" : Describe(concurrent.GetError()));
        auto early = Await(draining);
        closed &= record("first QuiesceContext timed out, space quiescent, lease refused", iteration,
                         !early && early.Category() == ErrorCategory::Timeout && h.space->IsQuiescent() &&
                         !h.space->AcquireExecutionLease(),
                         early ? "drain returned a token (expected Timeout)"
                               : Describe(early.GetError()));
        // While the first owner is still held at its lock-free entry point, the second (real JIT)
        // owner must independently reach its stop. Deterministic by construction; still waits on the
        // actual stop event rather than a timed poll.
        const bool second_stopped =
            second_run.wait_for(std::chrono::seconds(2)) == std::future_status::ready;
        closed &= record("second owner reached its stop while the first owner is still held", iteration,
                         second_stopped,
                         second_stopped ? "" : "second owner did not stop independently of the held owner");

        // Release the held owner; it enters the JIT and services its already-pending Pause/Cancel.
        if (!gate->Release(5'000)) std::_Exit(4);
        auto stopped = Await(run), other_stopped = Await(second_run);
        auto retry = h.context->QuiesceContext(1'000'000'000);
        bool recovered = record("retry QuiesceContext recovered both owners (count==2)", iteration,
                                retry && retry.Value().StoppedThreadCount() == 2,
                                retry ? ("count=" + std::to_string(retry.Value().StoppedThreadCount()))
                                      : Describe(retry.GetError()));
        if (!retry) std::_Exit(4);
        auto stale_resume = h.context->Resume(owner.handle, cancel->epoch);
        recovered &= record("Resume against the frozen quiescence is refused", iteration, !stale_resume,
                            stale_resume ? "Resume was accepted" : Describe(stale_resume.GetError()));
        retry.Value() = QuiescenceToken{};
        const auto before = Progress(progress);
        auto forbidden_run = owner.Run();
        auto cancelled = Await(forbidden_run);
        auto receipt = h.context->WaitStopped(*cancel, 1'000'000);
        recovered &= record("forbidden run surfaces the external request at the expected reason", iteration,
                            cancelled && receipt &&
                            cancelled.Value().primary_reason == expected_reason,
                            cancelled ? ("reason=" + std::to_string(static_cast<int>(cancelled.Value().primary_reason))
                                        + " expected=" + std::to_string(static_cast<int>(expected_reason)))
                                      : std::string{"owner did not stop"});
        recovered &= record("progress is preserved across the forbidden run", iteration,
                            stopped && other_stopped && Progress(progress) == before,
                            "progress advanced before resume");
        auto live_resume = h.context->Resume(owner.handle, cancel->epoch);
        recovered &= record("Resume after quiescence release is accepted", iteration, bool(live_resume),
                            live_resume ? "" : Describe(live_resume.GetError()));
        auto resumed = owner.Run();
        recovered &= record("resumed owner really advances its progress counter", iteration,
                            WaitProgress(progress, before));
        auto finish = h.context->RequestInterrupt(owner.handle, InterruptReason::Cancel);
        auto finished_wait = h.context->WaitStopped(finish.Value(), 1'000'000'000);
        bool finished = record("final Cancel stops the resumed owner cleanly", iteration,
                               bool(finished_wait),
                               finished_wait ? "" : Describe(finished_wait.GetError()));
        (void)Await(resumed);
        const bool iter_ok = closed && recovered && finished;
        Check(ordering == 0 ? "G24a" : "G24b",
              ordering == 0 ? "timeout recovery preserves an earlier external request and closes admission" :
                              "timeout recovery preserves a later external request and closes admission",
              iter_ok,
              iter_ok ? "" : ("iter=" + std::to_string(iteration) + " " + iter_failure));
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
    harness.data_base = base + kDataOffset;
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
    auto data_map = harness.space->Map(GuestRange{GuestAddress{harness.data_base}, kMappingSize},
                                       GuestPermission::Read | GuestPermission::Write);
    if (!data_map) {
        printf("FAILED: could not map guest data: %s\n", Describe(data_map.GetError()).c_str());
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
    TestCoordinatedPublication(harness);
    TestCoordinatedVersionSwitch(harness);
    TestCoordinatedRemap(harness);
    TestFullCacheRetirement(harness);
    TestPersistentOwnerVersionSwitch(harness);
    TestPersistentOwnerRemap(harness);
    TestGuestStorePublication(harness);
    TestPermissionRetirement(harness);
    TestFaultAttribution(harness);
    TestRealHleGate(harness);
    TestHleBufferPinning(harness);
    TestCoordinatorRecovery(harness);
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
