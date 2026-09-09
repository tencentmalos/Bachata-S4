#include <future>
#define main original_harness_main
#include "/Users/bytedance/workspace/emulations/ps4/shadps4/tests/guest_cpu/guest_execution_tests.cpp"
#undef main

void ReviewApi(Harness& harness, bool cancel_parked) {
  std::string error;
  if (!LoadFixture(harness, *FindFixture("spin_loop"), error)) { printf("setup error=%s\n", error.c_str()); return; }
  std::promise<ThreadHandle> ready;
  std::atomic<bool> done{false}, release{false};
  std::thread owner([&] {
    ThreadInit init{}; init.entry_rip={harness.code_base}; init.initial_rsp={harness.stack_top};
    auto t=harness.context->CreateThread(init);
    if (!t) { ready.set_value({}); return; }
    ready.set_value(t.Value());
    auto r=harness.context->Run(t.Value(), {});
    printf("owner_return result=%d reason=%s\n", bool(r), r ? std::string(ToString(r.Value().primary_reason)).c_str() : Describe(r.GetError()).c_str()); fflush(stdout);
    done.store(true);
    while (!release.load()) std::this_thread::sleep_for(std::chrono::milliseconds(1));
    auto d=harness.context->DestroyThread(t.Value());
    printf("owner_destroy=%d\n", bool(d)); fflush(stdout);
  });
  auto handle=ready.get_future().get();
  if (!handle.IsValid()) { owner.join(); return; }
  std::this_thread::sleep_for(std::chrono::milliseconds(150));
  auto p=harness.context->RequestInterrupt(handle,InterruptReason::Pause);
  auto ack=harness.context->WaitStopped(p.Value(),1000000000);
  printf("pause_ack=%d\n",bool(ack)); fflush(stdout);
  if (!ack) _exit(2);
  auto read=harness.context->ReadRegisters(handle);
  printf("read_while_parked=%d error=%s\n",bool(read),read?"":Describe(read.GetError()).c_str());
  if (!cancel_parked) {
    auto repeat=harness.context->WaitStopped(p.Value(),1000000);
    printf("repeat_wait stop_epoch=%llu first=%llu\n",(unsigned long long)(repeat?repeat.Value().stop_epoch:0),(unsigned long long)ack.Value().stop_epoch);
    auto resume=harness.context->Resume(handle,p.Value().epoch);
    std::this_thread::sleep_for(std::chrono::milliseconds(50));
    auto old=harness.context->WaitStopped(p.Value(),1000000);
    auto reading=harness.context->ReadRegisters(handle);
    printf("wait_old_after_resume=%d live_read=%d error=%s\n",bool(old),bool(reading),reading?"":Describe(reading.GetError()).c_str());
    auto p2=harness.context->RequestInterrupt(handle,InterruptReason::Pause);
    auto a2=harness.context->WaitStopped(p2.Value(),1000000000);
    printf("pause2_ack=%d\n",bool(a2)); fflush(stdout);
    if (!a2) _exit(2);
    auto stale=harness.context->Resume(handle,p.Value().epoch);
    printf("stale_resume_epoch1_after_ack2=%d\n",bool(stale));
    std::this_thread::sleep_for(std::chrono::milliseconds(50));
  }
  auto c=harness.context->RequestInterrupt(handle,InterruptReason::Cancel);
  auto ca=harness.context->WaitStopped(c.Value(),100000000);
  printf("cancel_ack=%d done=%d error=%s\n",bool(ca),done.load(),ca?"":Describe(ca.GetError()).c_str()); fflush(stdout);
  if (!done.load()) {
    auto wake=harness.context->Resume(handle,p.Value().epoch);
    printf("cleanup_resume=%d\n",bool(wake)); fflush(stdout);
  }
  for(int i=0;i<1000 && !done.load();++i) std::this_thread::sleep_for(std::chrono::milliseconds(1));
  if (!done.load()) { printf("cleanup failed\n"); fflush(stdout); _exit(3); }
  release.store(true); owner.join();
  printf("remaining_threads=%zu\n",harness.context->LiveThreadCount());
}

int main(int argc, char**) {
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


ReviewApi(harness, argc>1);
return 0;
}
