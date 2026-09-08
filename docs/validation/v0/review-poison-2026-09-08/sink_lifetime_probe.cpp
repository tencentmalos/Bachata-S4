#include "core/guest_cpu/api/address_space.h"
#include <atomic>
#include <thread>
#include <array>
#include <cstdio>
using namespace Core::GuestCpu;
struct WaitingSink final : CodeInvalidationSink {
 std::atomic<bool> entered{false}, release{false}, completed{false};
 std::string_view Name() const override {return "waiting-review";}
 Status DiscardTranslations(GuestRange, InvalidationReason) override {
  entered.store(true);
  while(!release.load()) std::this_thread::yield();
  completed.store(true);
  return Ok();
 }
};
int main() {
 auto r=GuestAddressSpace::Create({}); if(!r) return 2;
 auto space=std::move(r).Value(); GuestRange range{space->ReservationBase(),HostPageSize()};
 if(!space->Map(range,GuestPermission::Read|GuestPermission::Write)) return 3;
 WaitingSink first,second;
 if(!space->SetCodeInvalidationSink(&first)) return 4;
 auto q=space->Quiesce(1000); if(!q) return 5;
 std::array<std::byte,1> code{std::byte{1}};
 bool publication_ok=false;
 std::thread writer([&]{publication_ok=bool(space->PublishCode(q.Value(),{range.base,1},code));});
 while(!first.entered.load()) std::this_thread::yield();
 space->ClearCodeInvalidationSink(&first);
 auto registered=space->SetCodeInvalidationSink(&second);
 printf("clear_returned_with_callback_active=%d replacement_registered=%d\n",!first.completed.load(),bool(registered));
 // Keep both objects alive: demonstrate missing drain without deliberately causing UAF.
 first.release.store(true);writer.join();
 printf("old_sink_publication_succeeded_after_replacement=%d\n",publication_ok);
 space->ClearCodeInvalidationSink(&second);
}
