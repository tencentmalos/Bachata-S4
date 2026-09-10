// Review diagnostic only: real current backend, stronger syscall fault snapshot assertions.
// No adapter, FEX source or formal tests are changed by this probe.
#define main original_suite_main
#include "guest_execution_tests.cpp"
#undef main
#define Fixtures ReviewFixtures
#include "review_fixtures.h"
#undef Fixtures

int main() {
#include "../g3-n3-probe-2026-09-10/setup.inc"
    auto& h = harness;
    Fex::FexTestSetSyscallShimArmed(*h.context, true);
    Fex::FexTestSetSyscallShimTrace(*h.context, true);
    const auto& f = ReviewFixtures::kAll[0];
    Fixtures::Fixture fixture{f.name, f.bytes, f.gate_offset, f.disassembly};
    std::string error;
    if (!LoadFixture(h, fixture, error)) return 5;
    const auto sentinel = h.stack_base + 0x300;
    *reinterpret_cast<std::uint64_t*>(sentinel) = 0;
    ThreadInit init{};
    init.entry_rip = {h.code_base};
    init.initial_rsp = {h.stack_top};
    init.initial_state.fields = RegisterValidity::Gpr | RegisterValidity::Xmm |
        RegisterValidity::Mxcsr | RegisterValidity::SegmentBases;
    init.initial_state.gpr_mask = 0xffffu & ~(1u << Index(Gpr::Rsp));
    init.initial_state.xmm_mask = 0xffff;
    auto& expected = init.initial_state.values;
    for (std::size_t i = 0; i < kGprCount; ++i) expected.gpr[i] = 0x12340000 + i;
    for (std::size_t i = 0; i < kXmmCount; ++i)
        expected.xmm[i] = {0xABCD0000 + i, 0xDCBA0000 + i};
    expected.Set(Gpr::Rax, 0x777777);
    expected.Set(Gpr::R12, sentinel);
    expected.Set(Gpr::Rsp, h.stack_top);
    expected.mxcsr = 0x3f80;
    expected.fs_base = h.data_base;
    expected.gs_base = h.data_base + 128;
    auto thread = h.context->CreateThread(init);
    if (!thread) return 5;
    auto before = h.context->ReadRegisters(thread.Value());
    if (!before || before.Value().registers.gpr != expected.gpr ||
        before.Value().registers.xmm != expected.xmm) {
        printf("INITIAL_STATE_MISMATCH\n"); return 5;
    }
    auto run = h.context->Run(thread.Value(), {});
    if (!run) { printf("RUN_ERROR %s\n", Describe(run.GetError()).c_str()); return 5; }
    const auto& actual = run.Value().snapshot.registers;
    const auto* trace = Fex::FexTestSyscallTrace();
    // syscall architecturally writes RCX and R11. Their values were captured before the exit.
    expected.Set(Gpr::Rcx, trace->guest_rcx);
    expected.Set(Gpr::R11, trace->guest_r11);
    int mismatches = 0;
    for (std::size_t i = 0; i < kGprCount; ++i) {
        bool ok = actual.gpr[i] == expected.gpr[i];
        printf("GPR %s expected=0x%llx actual=0x%llx match=%d\n",
            std::string(ToString(static_cast<Gpr>(i))).c_str(),
            (unsigned long long)expected.gpr[i], (unsigned long long)actual.gpr[i], ok);
        mismatches += !ok;
    }
    for (std::size_t i = 0; i < kXmmCount; ++i) {
        bool ok = actual.xmm[i] == expected.xmm[i];
        printf("XMM %zu expected=0x%llx:%llx actual=0x%llx:%llx match=%d\n", i,
            (unsigned long long)expected.xmm[i].high, (unsigned long long)expected.xmm[i].low,
            (unsigned long long)actual.xmm[i].high, (unsigned long long)actual.xmm[i].low, ok);
        mismatches += !ok;
    }
    const auto sentinel_value = *reinterpret_cast<std::uint64_t*>(sentinel);
    const bool fault_ok = run.Value().primary_reason == StopReason::GuestFault &&
        run.Value().fault && run.Value().fault->guest_rip == h.code_base &&
        actual.rip == h.code_base && sentinel_value == 0;
    const bool metadata_ok = actual.mxcsr == expected.mxcsr &&
        actual.fs_base == expected.fs_base && actual.gs_base == expected.gs_base;
    printf("FAULT reason=%s sentinel=%llu guest_pc=0x%llx rip=0x%llx expected_pc=0x%llx "
           "validity=0x%x kind=%s fault_ok=%d metadata_ok=%d invocations=%d\n",
        std::string(ToString(run.Value().primary_reason)).c_str(),
        (unsigned long long)sentinel_value, (unsigned long long)run.Value().guest_pc.value_or(0),
        (unsigned long long)actual.rip, (unsigned long long)h.code_base,
        (unsigned)actual.validity, std::string(ToString(run.Value().snapshot.kind)).c_str(),
        fault_ok, metadata_ok, trace->invocations);
    auto destroy = h.context->DestroyThread(thread.Value());
    printf("FAULT_INFO guest_rip=0x%llx\n", (unsigned long long)
        (run.Value().fault ? run.Value().fault->guest_rip.value_or(0) : 0));
    printf("STATE_REVIEW mismatches=%d destroy=%d\n", mismatches, bool(destroy));
    return (!mismatches && fault_ok && metadata_ok && destroy) ? 0 : 2;
}
