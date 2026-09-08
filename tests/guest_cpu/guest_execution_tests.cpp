// SPDX-FileCopyrightText: Copyright 2026 shadPS4 Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later
//
// First real guest execution: hand-assembled x86-64 running under FEXCore through the public API.
//
// Everything before this round proved the backend could be built and initialised. This proves the
// translation path itself: x86 bytes go in, and the architectural effects come out in the registers
// the public snapshot reports.
//
// The fixtures are hand-assembled rather than produced by an assembler at build time, because the
// build host is arm64 and shipping an x86 toolchain dependency for a handful of instructions would
// cost more than it saves. Every byte sequence is commented with its disassembly.

#include <cinttypes>
#include <cstdio>
#include <cstring>
#include <vector>

#include <unistd.h>

#include "core/guest_cpu/api/address_space.h"
#include "core/guest_cpu/api/context.h"

namespace {

using namespace Core::GuestCpu;

int g_failures = 0;

void Check(const char* id, const char* name, bool condition, const char* detail = "") {
    printf("[%-6s] %-58s %s", id, name, condition ? "PASS" : "FAIL");
    if (detail != nullptr && detail[0] != '\0') {
        printf(" -- %s", detail);
    }
    printf("\n");
    fflush(stdout);
    if (!condition) {
        ++g_failures;
    }
}

void CheckU64(const char* id, const char* name, std::uint64_t actual, std::uint64_t expected) {
    char detail[128];
    std::snprintf(detail, sizeof(detail), "expected 0x%" PRIx64 ", got 0x%" PRIx64, expected, actual);
    Check(id, name, actual == expected, actual == expected ? "" : detail);
}

// Layout inside the guest reservation. Code and stack are separate mappings so an overrun of one
// cannot silently land in the other.
constexpr std::uint64_t kCodeOffset = 0x10000;
constexpr std::uint64_t kStackOffset = 0x20000;
constexpr std::uint64_t kMappingSize = 0x4000;

struct Fixture final {
    std::unique_ptr<GuestAddressSpace> space;
    std::unique_ptr<CpuContext> context;
    std::uint64_t code_base{};
    std::uint64_t stack_top{};
    std::uint64_t return_gate{};
};

// Writes `code` into the guest code mapping and publishes it as executable.
//
// RW to write, then RX to execute: never RWX. This is the publication model the spec requires, and
// it is also what a W^X host would enforce anyway.
bool LoadCode(Fixture& fixture, const std::vector<std::uint8_t>& code) {
    const GuestRange range{GuestAddress{fixture.code_base}, kMappingSize};

    // Make it writable again: a previous fixture will have left it RX.
    auto writable = fixture.space->Protect(range, GuestPermission::Read | GuestPermission::Write);
    if (!writable) {
        printf("  could not make the code mapping writable: %s\n",
               Describe(writable.GetError()).c_str());
        return false;
    }

    {
        auto pin = fixture.space->AcquirePinnedSpan(range, /*writable=*/true);
        if (!pin) {
            printf("  could not pin the code mapping: %s\n", Describe(pin.GetError()).c_str());
            return false;
        }
        auto bytes = pin.Value().Bytes();
        if (code.size() > bytes.size()) {
            printf("  fixture is larger than the code mapping\n");
            return false;
        }
        // Zero the rest so a shorter fixture cannot run into the previous one's tail.
        std::memset(bytes.data(), 0, bytes.size());
        std::memcpy(bytes.data(), code.data(), code.size());
        // The pin is released here, before the permission change: Protect refuses while a writer
        // still holds the span, which is the interlock that keeps a code transaction honest.
    }

    auto executable = fixture.space->Protect(range, GuestPermission::Read | GuestPermission::Execute);
    if (!executable) {
        printf("  could not publish the code mapping as executable: %s\n",
               Describe(executable.GetError()).c_str());
        return false;
    }

    // Discard any translation of the previous fixture at this address. Without this the backend
    // reuses the cached block and silently runs the old code -- which is precisely the stale-decode
    // failure the publication contract exists to prevent, and it is easy to mistake for a wrong
    // result from the new fixture.
    auto quiesced = fixture.space->Quiesce(/*timeout_ns=*/1'000'000'000);
    if (!quiesced) {
        printf("  could not quiesce before invalidation: %s\n",
               Describe(quiesced.GetError()).c_str());
        return false;
    }
    auto invalidated = fixture.context->InvalidateCode(quiesced.Value(), range,
                                                       InvalidationReason::HostWrite);
    if (!invalidated) {
        printf("  could not invalidate translated code: %s\n",
               Describe(invalidated.GetError()).c_str());
        return false;
    }
    return true;
}

// Runs one fixture from a clean thread and returns the stop result.
//
// `setup` receives the initial register patch so each case can seed only what it needs; unset
// registers stay at their architectural default rather than at whatever the last case left.
template <typename SetupFn>
bool RunFixture(Fixture& fixture, const char* id, const std::vector<std::uint8_t>& code,
                SetupFn&& setup, RunResult& out) {
    if (!LoadCode(fixture, code)) {
        Check(id, "load fixture into guest memory", false);
        return false;
    }

    ThreadInit init{};
    init.entry_rip = GuestCodeAddress{fixture.code_base};
    init.initial_rsp = GuestAddress{fixture.stack_top};
    init.guest_tid = 1;
    setup(init.initial_state);

    auto thread = fixture.context->CreateThread(init);
    if (!thread) {
        char detail[256];
        std::snprintf(detail, sizeof(detail), "%s", Describe(thread.GetError()).c_str());
        Check(id, "create guest thread", false, detail);
        return false;
    }

    auto result = fixture.context->Run(thread.Value(), RunOptions{});
    if (!result) {        char detail[256];
        std::snprintf(detail, sizeof(detail), "%s", Describe(result.GetError()).c_str());
        Check(id, "run guest code", false, detail);
        (void)fixture.context->DestroyThread(thread.Value());
        return false;
    }

    out = result.Value();
    auto destroyed = fixture.context->DestroyThread(thread.Value());
    if (!destroyed) {
        Check(id, "destroy guest thread", false, Describe(destroyed.GetError()).c_str());
        return false;
    }
    return true;
}

// Appends "mov r15, imm64; jmp r15" so the fixture ends by jumping to the return gate.
//
// r15 rather than rax: rax is a natural result register and several fixtures check it, so using it
// as the jump scratch would overwrite the very value under test. An absolute jump keeps the
// fixtures independent of where the gate happens to be mapped.
void AppendReturnToGate(std::vector<std::uint8_t>& code, std::uint64_t gate) {
    code.push_back(0x49);  // REX.WB
    code.push_back(0xBF);  // mov r15, imm64
    for (int i = 0; i < 8; ++i) {
        code.push_back(static_cast<std::uint8_t>((gate >> (i * 8)) & 0xFF));
    }
    code.push_back(0x41);  // REX.B
    code.push_back(0xFF);  // jmp
    code.push_back(0xE7);  // r15
}

// --- C01: integer arithmetic, flags and branches -----------------------------------------------
void TestIntegerArithmetic(Fixture& fixture) {
    // add r8, r9        49 01 C8
    // mov r10, r8       4D 89 C2
    // xor r11, r11      4D 31 DB
    // sub r10, 5        49 83 EA 05
    std::vector<std::uint8_t> code{
        0x49, 0x01, 0xC8,
        0x4D, 0x89, 0xC2,
        0x4D, 0x31, 0xDB,
        0x49, 0x83, 0xEA, 0x05,
    };
    AppendReturnToGate(code, fixture.return_gate);

    RunResult result{};
    const bool ran = RunFixture(fixture, "G01", code, [](RegisterPatch& patch) {
        patch.fields = RegisterValidity::Gpr;
        patch.gpr_mask = (1u << Index(Gpr::R8)) | (1u << Index(Gpr::R9));
        patch.values.Set(Gpr::R8, 0x1000);
        patch.values.Set(Gpr::R9, 0x234);
    }, result);
    if (!ran) {
        return;
    }

    Check("G01a", "integer fixture returned through the gate",
          result.primary_reason == StopReason::Returned,
          result.primary_reason == StopReason::Returned ? ""
                                                        : std::string(ToString(result.primary_reason)).c_str());
    CheckU64("G01b", "add r8, r9 produced the sum", result.snapshot.registers.Get(Gpr::R8), 0x1234);
    CheckU64("G01c", "mov r10, r8 copied the value", result.snapshot.registers.Get(Gpr::R10),
             0x1234 - 5);
    CheckU64("G01d", "xor r11, r11 cleared the register",
             result.snapshot.registers.Get(Gpr::R11), 0);
    Check("G01e", "snapshot reports GPRs as valid",
          HasAll(result.snapshot.registers.validity, RegisterValidity::Gpr));
    Check("G01f", "snapshot kind is a safe point",
          result.snapshot.kind == SnapshotKind::SafePoint);
}

// --- C01: taken and not-taken branches ---------------------------------------------------------
void TestBranches(Fixture& fixture) {
    // xor rax, rax          48 31 C0
    // cmp rdi, rsi          48 39 F7
    // jne +7                75 07
    // mov rax, 1            48 C7 C0 01 00 00 00     (executed only when equal)
    // (fallthrough)
    std::vector<std::uint8_t> code{
        0x48, 0x31, 0xC0,
        0x48, 0x39, 0xF7,
        0x75, 0x07,
        0x48, 0xC7, 0xC0, 0x01, 0x00, 0x00, 0x00,
    };
    AppendReturnToGate(code, fixture.return_gate);

    // Equal: the branch is not taken, so rax becomes 1.
    RunResult equal{};
    if (RunFixture(fixture, "G02", code, [](RegisterPatch& patch) {
            patch.fields = RegisterValidity::Gpr;
            patch.gpr_mask = (1u << Index(Gpr::Rdi)) | (1u << Index(Gpr::Rsi));
            patch.values.Set(Gpr::Rdi, 42);
            patch.values.Set(Gpr::Rsi, 42);
        }, equal)) {
        CheckU64("G02a", "not-taken branch fell through to the mov",
                 equal.snapshot.registers.Get(Gpr::Rax), 1);
    }

    // Unequal: the branch is taken, so the mov is skipped and rax stays 0.
    RunResult unequal{};
    if (RunFixture(fixture, "G02", code, [](RegisterPatch& patch) {
            patch.fields = RegisterValidity::Gpr;
            patch.gpr_mask = (1u << Index(Gpr::Rdi)) | (1u << Index(Gpr::Rsi));
            patch.values.Set(Gpr::Rdi, 42);
            patch.values.Set(Gpr::Rsi, 43);
        }, unequal)) {
        CheckU64("G02b", "taken branch skipped the mov",
                 unequal.snapshot.registers.Get(Gpr::Rax), 0);
    }
}

// --- C01: load/store through the guest stack ---------------------------------------------------
void TestLoadStore(Fixture& fixture) {
    // push rdi          57
    // push rsi          56
    // pop  rax          58        -> rax = rsi
    // pop  rcx          59        -> rcx = rdi
    std::vector<std::uint8_t> code{0x57, 0x56, 0x58, 0x59};
    AppendReturnToGate(code, fixture.return_gate);

    RunResult result{};
    if (!RunFixture(fixture, "G03", code, [](RegisterPatch& patch) {
            patch.fields = RegisterValidity::Gpr;
            patch.gpr_mask = (1u << Index(Gpr::Rdi)) | (1u << Index(Gpr::Rsi));
            patch.values.Set(Gpr::Rdi, 0xAAAA'BBBB'CCCC'DDDDull);
            patch.values.Set(Gpr::Rsi, 0x1111'2222'3333'4444ull);
        }, result)) {
        return;
    }

    CheckU64("G03a", "pop rax read back the second push",
             result.snapshot.registers.Get(Gpr::Rax), 0x1111'2222'3333'4444ull);
    CheckU64("G03b", "pop rcx read back the first push",
             result.snapshot.registers.Get(Gpr::Rcx), 0xAAAA'BBBB'CCCC'DDDDull);
    // Two pushes and two pops must leave the stack pointer where it started. A mismatch here would
    // mean the guest stack and the host's idea of it have diverged.
    CheckU64("G03c", "stack pointer is balanced after push/pop",
             result.snapshot.registers.Rsp(), fixture.stack_top);

    // Read the guest stack directly. This separates "the guest never ran" from "the guest ran but
    // its registers were not published": the pushes leave their operands in memory either way, and
    // memory is not subject to register spilling.
    auto stack = fixture.space->AcquirePinnedSpan(
        GuestRange{GuestAddress{fixture.stack_top - 16}, 16}, /*writable=*/false);
    if (stack) {
        std::uint64_t slot0 = 0;
        std::uint64_t slot1 = 0;
        std::memcpy(&slot0, stack.Value().Bytes().data(), sizeof(slot0));
        std::memcpy(&slot1, stack.Value().Bytes().data() + 8, sizeof(slot1));
        char detail[160];
        std::snprintf(detail, sizeof(detail), "stack[-16]=0x%" PRIx64 " stack[-8]=0x%" PRIx64,
                      slot0, slot1);
        Check("G03d", "the pushes actually wrote the guest stack",
              slot1 == 0xAAAA'BBBB'CCCC'DDDDull || slot0 == 0x1111'2222'3333'4444ull, detail);
    }
}

// --- C02: SSE2 -----------------------------------------------------------------------------
void TestSse2(Fixture& fixture) {
    // movq xmm0, rdi     66 48 0F 6E C7
    // movq xmm1, rsi     66 48 0F 6E CE
    // paddq xmm0, xmm1   66 0F D4 C1
    // movq rax, xmm0     66 48 0F 7E C0
    std::vector<std::uint8_t> code{
        0x66, 0x48, 0x0F, 0x6E, 0xC7,
        0x66, 0x48, 0x0F, 0x6E, 0xCE,
        0x66, 0x0F, 0xD4, 0xC1,
        0x66, 0x48, 0x0F, 0x7E, 0xC0,
    };
    AppendReturnToGate(code, fixture.return_gate);

    RunResult result{};
    if (!RunFixture(fixture, "G04", code, [](RegisterPatch& patch) {
            patch.fields = RegisterValidity::Gpr;
            patch.gpr_mask = (1u << Index(Gpr::Rdi)) | (1u << Index(Gpr::Rsi));
            patch.values.Set(Gpr::Rdi, 0x0000'0001'0000'0002ull);
            patch.values.Set(Gpr::Rsi, 0x0000'0010'0000'0020ull);
        }, result)) {
        return;
    }

    CheckU64("G04a", "paddq result moved back to a GPR",
             result.snapshot.registers.Get(Gpr::Rax), 0x0000'0011'0000'0022ull);
    CheckU64("G04b", "xmm0 low half holds the packed sum",
             result.snapshot.registers.xmm[0].low, 0x0000'0011'0000'0022ull);
    Check("G04c", "snapshot reports XMM as valid",
          HasAll(result.snapshot.registers.validity, RegisterValidity::Xmm));
}

// --- D05: a real HLT is not a normal return ----------------------------------------------------
void TestUnregisteredHlt(Fixture& fixture) {
    // hlt    F4    -- at the fixture's own address, not the registered gate.
    std::vector<std::uint8_t> code{0xF4};

    RunResult result{};
    if (!RunFixture(fixture, "G05", code, [](RegisterPatch&) {}, result)) {
        return;
    }

    // The whole point of the registered gate: a HLT anywhere else must not look like a clean
    // return, or a crashing guest would be indistinguishable from a finished one.
    Check("G05a", "a HLT outside the gate is not reported as Returned",
          result.primary_reason != StopReason::Returned,
          std::string(ToString(result.primary_reason)).c_str());
    Check("G05b", "the stop is reported as a guest fault",
          result.primary_reason == StopReason::GuestFault);
    Check("G05c", "fault carries a guest RIP", result.fault.has_value() &&
                                                   result.fault->guest_rip.has_value());
    Check("G05d", "snapshot kind is Faulted, not SafePoint",
          result.snapshot.kind == SnapshotKind::Faulted);
}

// --- contract checks that do not need execution ------------------------------------------------
void TestContracts(Fixture& fixture) {
    const auto caps = fixture.context->Capabilities();
    Check("G06a", "capabilities declare base integer support",
          Contains(caps.features, GuestFeature::BaseInteger));
    Check("G06b", "capabilities declare SSE2 support", Contains(caps.features, GuestFeature::Sse2));
    // C04 is a conditional MUST: AVX must not be declared without an execution and state-restore
    // test, and there is none.
    Check("G06c", "capabilities do not claim AVX", !Contains(caps.features, GuestFeature::Avx));
    Check("G06d", "capabilities report the injected host page size",
          caps.host_page_size == static_cast<std::uint64_t>(::sysconf(_SC_PAGESIZE)));

    // A second live context must be refused rather than quietly sharing the first one's backend.
    auto second = CreateContext(CpuConfig{}, *fixture.space);
    Check("G06e", "a second concurrent context is refused with AlreadyActive",
          !second && second.Category() == ErrorCategory::AlreadyActive);

    // An unmapped entry point is a caller error, not something to discover by crashing in the JIT.
    ThreadInit bad{};
    bad.entry_rip = GuestCodeAddress{fixture.space->ReservationBase().value + 0x7F00'0000};
    bad.initial_rsp = GuestAddress{fixture.stack_top};
    auto rejected = fixture.context->CreateThread(bad);
    Check("G06f", "an unmapped entry_rip is refused",
          !rejected && rejected.Category() == ErrorCategory::InvalidArgument);

    // Step is refused before execution rather than silently running a whole block (T07).
    // Publish a minimal fixture first so this exercises a genuinely runnable thread; without it the
    // code page is still RW and CreateThread would refuse for an unrelated reason.
    std::vector<std::uint8_t> stub;
    AppendReturnToGate(stub, fixture.return_gate);
    if (!LoadCode(fixture, stub)) {
        Check("G06g", "publish a stub fixture for the handle checks", false);
        return;
    }

    ThreadInit ok{};
    ok.entry_rip = GuestCodeAddress{fixture.code_base};
    ok.initial_rsp = GuestAddress{fixture.stack_top};
    auto thread = fixture.context->CreateThread(ok);
    if (thread) {
        auto stepped = fixture.context->Step(thread.Value(), StepOptions{});
        Check("G06g", "Step is refused with Unsupported, not silently run as a block",
              !stepped && stepped.Category() == ErrorCategory::Unsupported);

        // A stale epoch must not be able to write registers computed against an older stop.
        RegisterPatch patch{};
        patch.fields = RegisterValidity::Gpr;
        patch.gpr_mask = 1u << Index(Gpr::Rax);
        patch.values.Set(Gpr::Rax, 1);
        auto stale = fixture.context->WriteRegisters(thread.Value(), patch, /*stop_epoch=*/9999);
        Check("G06h", "WriteRegisters refuses a stale stop epoch",
              !stale && stale.Category() == ErrorCategory::StaleEpoch);

        (void)fixture.context->DestroyThread(thread.Value());

        // Operations on a destroyed handle must be refused, not applied to a reused slot.
        auto after = fixture.context->ReadRegisters(thread.Value());
        Check("G06i", "a destroyed handle is refused with InvalidHandle",
              !after && after.Category() == ErrorCategory::InvalidHandle);
    }
}

} // namespace

int main() {
    printf("guest execution through the public CPU API\n");
    printf("host page size: %ld\n\n", ::sysconf(_SC_PAGESIZE));

    Fixture fixture{};

    AddressSpaceConfig space_config{};
    space_config.reservation_size = std::uint64_t{1} << 28;
    auto space = GuestAddressSpace::Create(space_config);
    if (!space) {
        printf("FAILED: could not create the guest address space: %s\n",
               Describe(space.GetError()).c_str());
        return 1;
    }
    fixture.space = std::move(space).Value();

    const std::uint64_t base = fixture.space->ReservationBase().value;
    fixture.code_base = base + kCodeOffset;
    fixture.stack_top = base + kStackOffset + kMappingSize - 16;

    // Code is mapped RX, not RWX: the fixture bytes are written through a pinned span before the
    // permission is applied, mirroring the publication model the spec requires.
    auto code_map = fixture.space->Map(GuestRange{GuestAddress{fixture.code_base}, kMappingSize},
                                       GuestPermission::Read | GuestPermission::Write);
    if (!code_map) {
        printf("FAILED: could not map guest code: %s\n", Describe(code_map.GetError()).c_str());
        return 1;
    }
    auto stack_map = fixture.space->Map(GuestRange{GuestAddress{base + kStackOffset}, kMappingSize},
                                        GuestPermission::Read | GuestPermission::Write);
    if (!stack_map) {
        printf("FAILED: could not map guest stack: %s\n", Describe(stack_map.GetError()).c_str());
        return 1;
    }

    auto context = CreateContext(CpuConfig{}, *fixture.space);
    if (!context) {
        printf("FAILED: could not create the CPU context: %s\n",
               Describe(context.GetError()).c_str());
        return 1;
    }
    fixture.context = std::move(context).Value();

    // The gate address comes from the backend, so the fixtures jump to wherever it actually mapped
    // rather than to a hardcoded address that could silently drift.
    fixture.return_gate = fixture.context->Capabilities().return_gate_address;
    if (fixture.return_gate == 0) {
        printf("FAILED: the backend did not report a return gate address\n");
        return 1;
    }

    printf("guest reservation base: 0x%" PRIx64 "\n", base);
    printf("code at 0x%" PRIx64 ", stack top 0x%" PRIx64 "\n", fixture.code_base,
           fixture.stack_top);
    printf("return gate at 0x%" PRIx64 "\n\n", fixture.return_gate);

    TestIntegerArithmetic(fixture);
    TestBranches(fixture);
    TestLoadStore(fixture);
    TestSse2(fixture);
    TestUnregisteredHlt(fixture);
    printf("\n");
    // Contract checks last: they publish their own stub at the same address, and running them
    // first would leave a translation cached there for the fixtures to trip over.
    TestContracts(fixture);

    printf("\n%s (%d failure%s)\n", g_failures == 0 ? "ALL PASS" : "FAILED", g_failures,
           g_failures == 1 ? "" : "s");
    printf("SCOPE: real x86-64 executed by FEXCore through the public API. Covers integer,\n");
    printf("       branch, load/store and SSE2 fixtures plus the return-gate contract.\n");
    return g_failures == 0 ? 0 : 1;
}
