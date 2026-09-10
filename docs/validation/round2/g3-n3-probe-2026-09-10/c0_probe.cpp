// N3 C0/C1 probe: a user-installed syscall wrapper is called by the real FEX JIT. C0 verifies
// transparent forwarding (normal HLE continuation unchanged); C1 verifies the fault immediate-exit
// (GuestFault, successor sentinel NOT written, fault rip = syscall PC). Built against the real FEX
// static libs and the real guest harness. The harness is pulled in via the test TU (the build adds
// tests/guest_cpu to the include path); fixtures are generated from the sibling review.S.
#define main original_suite_main
#include "guest_execution_tests.cpp"
#undef main
#define Fixtures ReviewFixtures
#include "review_fixtures.h"
#undef Fixtures

std::uint64_t C0AddOne(std::uint64_t x) { return x + 1; }

int main(int argc, char** argv) {
#include "setup.inc"
    auto& h = harness;
    auto* registry = static_cast<HleCallRegistry*>(Fex::FexHleRegistryPointer(*h.context));

    // Arm the C0 forwarding shim with trace enabled (C1 syscall-point facts).
    Fex::FexTestSetSyscallShimArmed(*h.context, true);
    Fex::FexTestSetSyscallShimTrace(*h.context, true);

    std::string mode = argc > 1 ? argv[1] : "good";
    std::uint64_t good_op = registry->Register(&C0AddOne, "c0-addone").Value();
    std::uint64_t op = (mode == "good") ? good_op : 0x777777u;

    const auto& f = ReviewFixtures::kAll[0];
    Fixtures::Fixture fixture{f.name, f.bytes, f.gate_offset, f.disassembly};
    std::string error;
    if (!LoadFixture(h, fixture, error)) { printf("setup %s\n", error.c_str()); return 5; }

    const auto sentinel = h.stack_base + 0x300;
    *reinterpret_cast<std::uint64_t*>(sentinel) = 0;
    ThreadInit init{};
    init.entry_rip = {h.code_base};
    init.initial_rsp = {h.stack_top};
    init.initial_state.fields = RegisterValidity::Gpr | RegisterValidity::Mxcsr;
    init.initial_state.gpr_mask = (1u << Index(Gpr::Rax)) | (1u << Index(Gpr::R12));
    init.initial_state.values.Set(Gpr::Rax, op);
    init.initial_state.values.Set(Gpr::R12, sentinel);
    init.initial_state.values.mxcsr = 0x3f80;
    auto t = h.context->CreateThread(init);
    if (!t) return 5;
    auto r = h.context->Run(t.Value(), {});
    const auto sentinel_value = *reinterpret_cast<std::uint64_t*>(sentinel);
    const auto rax = r ? r.Value().snapshot.registers.Get(Gpr::Rax) : 0;
    const auto reason = r ? std::string(ToString(r.Value().primary_reason)) : "error";

    Fex::FexTestSetSyscallShimArmed(*h.context, false);
    Fex::FexTestSetSyscallShimTrace(*h.context, false);
    if (const auto* tr = Fex::FexTestSyscallTrace()) {
        printf("TRACE invocations=%d\n", tr->invocations);
        printf("TRACE shim_sp=0x%llx shim_x28=0x%llx shim_lr=0x%llx\n",
               (unsigned long long)tr->shim_sp, (unsigned long long)tr->shim_x28,
               (unsigned long long)tr->shim_lr);
        printf("TRACE returning_stack=0x%llx in_syscall=0x%llx frame=0x%llx\n",
               (unsigned long long)tr->returning_stack, (unsigned long long)tr->in_syscall,
               (unsigned long long)tr->frame_addr);
        printf("TRACE shim_sp-returning_stack=%lld (positive => shim frame above dispatcher save area)\n",
               (long long)(tr->shim_sp - tr->returning_stack));
        printf("TRACE guest_rip=0x%llx rcx=0x%llx r11=0x%llx callret_sp=0x%llx\n",
               (unsigned long long)tr->guest_rip, (unsigned long long)tr->guest_rcx,
               (unsigned long long)tr->guest_r11, (unsigned long long)tr->callret_sp);
    }
    (void)h.context->DestroyThread(t.Value());

    printf("C0 mode=%s reason=%s rax=%llu sentinel=%llu\n", mode.c_str(), reason.c_str(),
           (unsigned long long)rax, (unsigned long long)sentinel_value);

    // C0 expectations:
    //  good -> Returned, rax = addone(rdi=0)+... we passed rax=op, RDI default 0 -> 1; successor
    //          sentinel store executes (sentinel == 42 as the fixture writes 42).
    //  unknown -> pre-existing behaviour: sentinel 42 still written in C0 (this is what C1 fixes),
    //          and reason is a fault later -- but the point of C0 is ONLY that the shim transparently
    //          forwards with no behaviour change, so good must be exactly as without the shim.
    if (mode == "good") {
        const bool sentinel_written = (sentinel_value == 42);
        const bool rax_ok = (rax == 1);
        printf("C0 good: sentinel_written(continuation ok)=%d rax_addone=%d\n",
               sentinel_written ? 1 : 0, rax_ok ? 1 : 0);
        return (sentinel_written && rax_ok) ? 0 : 2;
    }
    // unknown: with the C1 immediate-exit wrapper, the successor store must NOT run: GuestFault,
    // sentinel stays 0. (Before C1 the fall-through wrote 42 and the fault was reported only at Run
    // end.) The fault PC must be the syscall instruction.
    printf("C1 unknown (immediate exit): sentinel=%llu reason=%s\n",
           (unsigned long long)sentinel_value, reason.c_str());
    const bool fault = (r && r.Value().primary_reason == StopReason::GuestFault);
    const auto fault_pc = r && r.Value().guest_pc.has_value() ? *r.Value().guest_pc : 0xdead;
    const auto snap_pc = r ? r.Value().snapshot.registers.rip : 0xdead;
    const bool fault_pc_is_syscall = (fault_pc == h.code_base) || (snap_pc == h.code_base);
    printf("C1 unknown: fault=%d sentinel_zero=%d fault_pc_syscall=%d "
           "(guest_pc=0x%llx snap_rip=0x%llx syscall_pc=0x%llx)\n",
           fault ? 1 : 0, sentinel_value == 0 ? 1 : 0, fault_pc_is_syscall ? 1 : 0,
           (unsigned long long)fault_pc, (unsigned long long)snap_pc,
           (unsigned long long)h.code_base);
    return (fault && sentinel_value == 0 && fault_pc_is_syscall) ? 0 : 2;
}
