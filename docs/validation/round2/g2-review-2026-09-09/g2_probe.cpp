#define main original_harness_main
#include "/Users/bytedance/workspace/emulations/ps4/shadps4/tests/guest_cpu/guest_execution_tests.cpp"
#undef main
#include <sys/mman.h>
void Emit(const char* what, bool result) {printf("%s=%d\n",what,result);fflush(stdout);}
void Cache(Harness& h, const std::string& mode) {
 std::string err; if(!LoadFixture(h,*FindFixture("constant_a"),err)) std::_Exit(5);
 ThreadInit init{}; init.entry_rip={h.code_base};init.initial_rsp={h.stack_top};
 auto thread=h.context->CreateThread(init);if(!thread)std::_Exit(5);
 auto initial=h.context->Run(thread.Value(),{}); Emit("warm_A_17",initial && initial.Value().snapshot.registers.Get(Gpr::Rax)==17);
 if(mode=="foreign") {
  AddressSpaceConfig cfg{};cfg.reservation_size=1ULL<<20;cfg.max_address=QueryBackendCapabilities().max_guest_address;
  auto other=GuestAddressSpace::Create(cfg);auto q=other.Value()->Quiesce(1000);
  Emit("foreign_token_accepted",bool(h.context->ClearCodeCache(q.Value())));
  std::_Exit(0); // Avoid a destructor following the buggy unlocked cache call.
 }
 const GuestRange range{{h.code_base},kMappingSize};
 {
  auto token=h.context->QuiesceContext(1000000000);if(!token)std::_Exit(5);
  if(mode=="rw_skip") {
   auto remap=h.space->RemapUnderToken(token.Value(),range,GuestPermission::Read|GuestPermission::Write);
   if(!remap)std::_Exit(5);
   auto f=FindFixture("constant_b");std::memcpy(reinterpret_cast<void*>(h.code_base),f->bytes.data(),f->bytes.size());
   std::memcpy(reinterpret_cast<void*>(h.code_base+f->gate_offset),&h.return_gate,8);
   Emit("clear_RW_accepted",bool(h.context->ClearCodeCache(token.Value())));
   if(!h.space->ReprotectUnderToken(token.Value(),range,GuestPermission::Read|GuestPermission::Execute))std::_Exit(5);
  } else {
   Emit("clear_RX_returned",bool(h.context->ClearCodeCache(token.Value())));
   printf("calling_regular_invalidation_after_clear\n");fflush(stdout);
   Emit("regular_invalidation_returned",bool(h.context->InvalidateCode(token.Value(),range,InvalidationReason::HostWrite)));
  }
 }
 auto snap=h.context->ReadRegisters(thread.Value());RegisterPatch p{};p.fields=RegisterValidity::Rip;p.values.rip=h.code_base;
 if(!h.context->WriteRegisters(thread.Value(),p,snap.Value().stop_epoch))std::_Exit(5);
 auto r=h.context->Run(thread.Value(),{});
 printf("expected_B=34 actual=%llu\n",(unsigned long long)(r?r.Value().snapshot.registers.Get(Gpr::Rax):0));fflush(stdout);
 (void)h.context->DestroyThread(thread.Value());
}
void Admission(Harness& h, bool cancel) {
 if(!LoadProgress(h)) std::_Exit(5);
 auto p1=PrepareProgress(h,0),p2=PrepareProgress(h,1);
 TestOwner a(h,p1),b(h,p2,4096);auto ra=a.Run(),rb=b.Run();
 if(!WaitProgress(p1,0)||!WaitProgress(p2,0))std::_Exit(5);
 struct sigaction act{},old{};act.sa_handler=HoldOwnerSignal;act.sa_flags=SA_ONSTACK;sigemptyset(&act.sa_mask);sigaction(SIGUSR1,&act,&old);
 hold_release.store(false);hold_entered.store(false);
 syscall(SYS_tgkill,getpid(),a.Tid(),SIGUSR1);syscall(SYS_tgkill,getpid(),b.Tid(),SIGUSR1);
 std::this_thread::sleep_for(std::chrono::milliseconds(20));
 auto q=std::async(std::launch::async,[&]{return h.context->QuiesceContext(cancel?1000000000:200000000);});
 std::this_thread::sleep_for(std::chrono::milliseconds(30));
 Emit("coordinator_still_waiting",q.wait_for(std::chrono::milliseconds(0))!=std::future_status::ready);
 Emit("admission_closed_while_stopping",h.space->IsQuiescent());
 ThreadInit init{};init.entry_rip={h.code_base};init.initial_rsp={h.stack_top};
 auto extra=h.context->CreateThread(init);Emit("create_admitted_while_stopping",bool(extra));if(extra)(void)h.context->DestroyThread(extra.Value());
 if(cancel) {
  auto ca=h.context->RequestInterrupt(a.handle,InterruptReason::Cancel);auto cb=h.context->RequestInterrupt(b.handle,InterruptReason::Cancel);
  hold_release.store(true);auto qa=Await(q);auto xa=Await(ra),xb=Await(rb);
  Emit("quiesce_succeeded",bool(qa));Emit("both_original_runs_cancelled",xa&&xb&&xa.Value().primary_reason==StopReason::Cancelled&&xb.Value().primary_reason==StopReason::Cancelled);
  if(qa)qa.Value()=QuiescenceToken{};
  auto av=Progress(p1),bv=Progress(p2);ra=a.Run();rb=b.Run();std::this_thread::sleep_for(std::chrono::milliseconds(30));
  Emit("cancelled_owner_reexecuted_without_resume",Progress(p1)>av||Progress(p2)>bv);
 } else {
  auto result=Await(q);Emit("quiesce_timed_out",!result&&result.GetError().category==ErrorCategory::Timeout);
  auto lease=h.space->AcquireExecutionLease();Emit("execution_admitted_after_timeout",bool(lease));if(lease)lease.Value()=ExecutionLease{};
  hold_release.store(true);
 }
 (void)h.context->RequestInterrupt(a.handle,InterruptReason::Cancel);(void)h.context->RequestInterrupt(b.handle,InterruptReason::Cancel);
 auto xa=Await(ra),xb=Await(rb);sigaction(SIGUSR1,&old,nullptr);
}
int main(int argc,char**argv) {
#include "setup.inc"
 std::string mode=argc>1?argv[1]:"clear_rx";
 if(mode=="admission"||mode=="cancel")Admission(harness,mode=="cancel");else Cache(harness,mode);
 return 0;
}
