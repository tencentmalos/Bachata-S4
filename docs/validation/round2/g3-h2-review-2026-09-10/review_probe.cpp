#define main original_suite_main
#include "/Users/bytedance/workspace/emulations/ps4/shadps4/tests/guest_cpu/guest_execution_tests.cpp"
#undef main
#define Fixtures ReviewFixtures
#include "review_fixtures.h"
#undef Fixtures
#include <stdexcept>
std::uint64_t ReadNativeRound() { return ::fegetround(); }
std::uint64_t ThrowNative() { throw std::runtime_error("review-native-exception"); }
int main(int argc, char** argv) {
#include "setup.inc"
    auto& h=harness;
    auto* registry=static_cast<HleCallRegistry*>(Fex::FexHleRegistryPointer(*h.context));
    std::string mode=argc>1?argv[1]:"unknown", error;
    std::uint64_t operation=0x777777;
    if(mode=="fp") operation=registry->Register(&ReadNativeRound,"read-round").Value();
    if(mode=="exception") operation=registry->Register(&ThrowNative,"throw").Value();
    if(mode=="rejected") operation=registry->Register(&HleBufferCopySum,"buffer").Value();
    const auto& f=ReviewFixtures::kAll[0];
    Fixtures::Fixture fixture{f.name,f.bytes,f.gate_offset,f.disassembly};
    if (!LoadFixture(h,fixture,error)) { printf("setup %s\n",error.c_str()); return 5; }
    auto sentinel=h.stack_base+0x300;
    *reinterpret_cast<std::uint64_t*>(sentinel)=0;
    ThreadInit init{}; init.entry_rip={h.code_base};init.initial_rsp={h.stack_top};
    init.initial_state.fields=RegisterValidity::Gpr|RegisterValidity::Mxcsr;
    init.initial_state.gpr_mask=(1u<<Index(Gpr::Rax))|(1u<<Index(Gpr::R12))|(1u<<Index(Gpr::Rdi))|(1u<<Index(Gpr::Rsi))|(1u<<Index(Gpr::Rdx))|(1u<<Index(Gpr::R10));
    init.initial_state.values.Set(Gpr::Rax,operation);init.initial_state.values.Set(Gpr::R12,sentinel);
    init.initial_state.values.Set(Gpr::Rdi,0xFFFF00000000ULL);init.initial_state.values.Set(Gpr::Rsi,1);
    init.initial_state.values.Set(Gpr::Rdx,h.stack_base+0x400);init.initial_state.values.Set(Gpr::R10,1);
    init.initial_state.values.mxcsr=0x3f80; // guest down
    ::fesetround(FE_UPWARD);
    auto t=h.context->CreateThread(init);if(!t)return 5;
    auto r=h.context->Run(t.Value(),{});
    auto value=*reinterpret_cast<std::uint64_t*>(sentinel);
    auto actual=r?r.Value().snapshot.registers.Get(Gpr::Rax):0;
    printf("mode=%s ok=%d reason=%s sentinel=%llu rax=%llu native_calls=%d host_round_after=%d expected_host_round=%d\n",mode.c_str(),bool(r),r?std::string(ToString(r.Value().primary_reason)).c_str():"error",(unsigned long long)value,(unsigned long long)actual,g_hle_buffer_calls.load(),::fegetround(),FE_UPWARD);
    (void)h.context->DestroyThread(t.Value());
    if(mode=="fp") return actual==FE_UPWARD?0:2;
    return value==0?0:2;
}
