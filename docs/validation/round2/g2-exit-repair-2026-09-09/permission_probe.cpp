#define main original_harness_main
#include "/Users/bytedance/workspace/emulations/ps4/shadps4/tests/guest_cpu/guest_execution_tests.cpp"
#undef main
int main(int argc, char** argv) {
#include "setup.inc"
    auto& h = harness;
    std::string error;
    if (!LoadFixture(h, *FindFixture("constant_a"), error)) return 5;
    ThreadInit init{}; init.entry_rip = {h.code_base}; init.initial_rsp = {h.stack_top};
    auto thread = h.context->CreateThread(init); if (!thread) return 5;
    auto warm = h.context->Run(thread.Value(), {});
    if (!warm || warm.Value().snapshot.registers.Get(Gpr::Rax) != 17) return 5;
    const bool rewrite = argc > 1 && std::string(argv[1]) == "rewrite";
    GuestRange range{{h.code_base}, kMappingSize};
    {
        auto token = h.context->QuiesceContext(1'000'000'000); if (!token) return 5;
        if (!h.space->ReprotectUnderToken(token.Value(), range, GuestPermission::Read | GuestPermission::Write)) return 5;
        if (rewrite) {
            auto f = FindFixture("constant_b");
            std::memcpy(reinterpret_cast<void*>(h.code_base), f->bytes.data(), f->bytes.size());
            std::memcpy(reinterpret_cast<void*>(h.code_base+f->gate_offset), &h.return_gate, 8);
            if (!h.space->ReprotectUnderToken(token.Value(), range, GuestPermission::Read | GuestPermission::Execute)) return 5;
        }
    }
    auto mapping = h.space->Query(range.base);
    auto snap = h.context->ReadRegisters(thread.Value());
    RegisterPatch patch{}; patch.fields = RegisterValidity::Rip; patch.values.rip = h.code_base;
    if (!h.context->WriteRegisters(thread.Value(), patch, snap.Value().stop_epoch)) return 5;
    auto after = h.context->Run(thread.Value(), {});
    bool stale = after && after.Value().primary_reason == StopReason::Returned && after.Value().snapshot.registers.Get(Gpr::Rax) == 17;
    printf("mode=%s warmed=17 mapping_X=%d poisoned=%d Run_ok=%d returned_old_17=%d\n", rewrite?"rewrite":"NX", HasPermission(mapping.Value().permission, GuestPermission::Execute), h.space->HasPoisonedCode(), bool(after), stale);
    if (after) printf("reason=%s actual_rax=%llu\n", ToString(after.Value().primary_reason).data(), (unsigned long long)after.Value().snapshot.registers.Get(Gpr::Rax));
    (void)h.context->DestroyThread(thread.Value());
    return stale ? 2 : 0;
}
