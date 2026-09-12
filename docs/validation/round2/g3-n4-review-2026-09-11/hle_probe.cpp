// Independent review probe, compiled against the current unmodified production backend.
#define main original_suite_main
#include "guest_execution_tests.cpp"
#undef main
#include <stdexcept>

std::uint64_t ReviewAddOne(std::uint64_t x) { return x + 1; }
std::uint64_t ReviewRounding(std::uint64_t) { return ::fegetround(); }
std::uint64_t ReviewThrow(std::uint64_t) { throw std::runtime_error("review native exception"); }

int main(int argc, char** argv) {
#include "../g3-n3-probe-2026-09-10/setup.inc"
    auto& h = harness;
    std::string mode = argc > 1 ? argv[1] : "unknown";
    auto* registry = static_cast<HleCallRegistry*>(Fex::FexHleRegistryPointer(*h.context));
    std::uint64_t op = 0x777777;
    if (mode == "valid") op = registry->Register(&ReviewAddOne, "review-add").Value();
    if (mode == "fp") op = registry->Register(&ReviewRounding, "review-fp").Value();
    if (mode == "throw") op = registry->Register(&ReviewThrow, "review-throw").Value();
    std::string error;
    if (!LoadFixture(h, *FindFixture("syscall_writes_sentinel"), error)) return 5;
    const auto sentinel = h.stack_base + 0x300;
    *reinterpret_cast<std::uint64_t*>(sentinel) = 0;
    ThreadInit init{};
    init.entry_rip = {h.code_base}; init.initial_rsp = {h.stack_top};
    init.initial_state.fields = RegisterValidity::Gpr | RegisterValidity::Xmm | RegisterValidity::Mxcsr;
    init.initial_state.gpr_mask = 0xffffu & ~(1u << Index(Gpr::Rsp));
    init.initial_state.xmm_mask = 0xffff;
    auto& expected = init.initial_state.values;
    for (std::size_t i = 0; i < kGprCount; ++i) expected.gpr[i] = 0x12340000 + i;
    for (std::size_t i = 0; i < kXmmCount; ++i) expected.xmm[i] = {0xABCD0000 + i, 0xDCBA0000 + i};
    expected.Set(Gpr::Rax, op); expected.Set(Gpr::R12, sentinel); expected.Set(Gpr::Rsp, h.stack_top);
    expected.Set(Gpr::Rdi, 40);
    expected.mxcsr = 0x3f80; // guest rounding down
    auto thread = h.context->CreateThread(init);
    if (!thread) return 5;
    auto before = h.context->ReadRegisters(thread.Value());
    if (!before || before.Value().registers.gpr != expected.gpr ||
        before.Value().registers.xmm != expected.xmm) return 5;
    expected.Set(Gpr::Rcx, h.code_base + 2); // architectural syscall successor PC, not arg4 from R10
    expected.Set(Gpr::R11, before.Value().registers.rflags);
    if (mode == "valid") expected.Set(Gpr::Rax, 41);
    if (mode == "fp") expected.Set(Gpr::Rax, FE_UPWARD);
    if (mode != "unknown") expected.Set(Gpr::R15, h.return_gate);
    fenv_t saved_host{}; ::fegetenv(&saved_host); ::fesetround(FE_UPWARD);
    std::printf("RUN mode=%s host_round=%d guest_expected_round=%d\n",
                mode.c_str(), FE_UPWARD, FE_DOWNWARD); std::fflush(stdout);
    auto run = h.context->Run(thread.Value(), {});
    const int host_after = ::fegetround(); ::fesetenv(&saved_host);
    if (!run) { printf("RUN_ERROR %s\n", Describe(run.GetError()).c_str()); return 5; }
    const auto& actual = run.Value().snapshot.registers;
    int gpr_bad = 0, xmm_bad = 0;
    for (std::size_t i = 0; i < kGprCount; ++i) {
        bool ok = expected.gpr[i] == actual.gpr[i]; gpr_bad += !ok;
        printf("GPR %s expected=0x%llx actual=0x%llx match=%d\n",
            std::string(ToString(static_cast<Gpr>(i))).c_str(),
            (unsigned long long)expected.gpr[i], (unsigned long long)actual.gpr[i], ok);
    }
    for (std::size_t i = 0; i < kXmmCount; ++i) xmm_bad += expected.xmm[i] != actual.xmm[i];
    auto sentinel_value = *reinterpret_cast<std::uint64_t*>(sentinel);
    bool outcome = mode == "unknown" ?
        run.Value().primary_reason == StopReason::GuestFault && sentinel_value == 0 &&
        run.Value().fault && run.Value().fault->guest_rip == h.code_base :
        run.Value().primary_reason == StopReason::Returned && sentinel_value == 42;
    printf("RESULT mode=%s reason=%s sentinel=%llu gpr_mismatches=%d xmm_mismatches=%d "
           "outcome=%d validity=0x%x host_restored=%d native_result=%llu\n", mode.c_str(),
        std::string(ToString(run.Value().primary_reason)).c_str(), (unsigned long long)sentinel_value,
        gpr_bad, xmm_bad, outcome, (unsigned)actual.validity, host_after == FE_UPWARD,
        (unsigned long long)actual.Get(Gpr::Rax));
    auto destroy = h.context->DestroyThread(thread.Value());
    return outcome && !gpr_bad && !xmm_bad && host_after == FE_UPWARD && destroy ? 0 : 2;
}
