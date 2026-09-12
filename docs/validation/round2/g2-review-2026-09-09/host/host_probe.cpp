#include <cstdio>
#include <future>
#include <memory>
#include <vector>
#include <unistd.h>
#include "core/guest_cpu/api/address_space.h"
using namespace Core::GuestCpu;
struct Sink final:CodeInvalidationSink {
 std::promise<void> entered,release;std::shared_future<void> ready=release.get_future().share(); bool fail=false;
 std::string_view Name()const override{return "review";}
 Status DiscardTranslations(GuestRange,InvalidationReason)override{
  entered.set_value();if(fail)return MakeError(ErrorCategory::BackendFailure,"review","injected");ready.wait();return Ok();
 }
};
int main(int argc,char**){
 AddressSpaceConfig cfg{};cfg.reservation_size=1ULL<<20;auto created=GuestAddressSpace::Create(cfg);if(!created)return 5;auto space=std::move(created).Value();
 GuestRange range{space->ReservationBase(),(uint64_t)sysconf(_SC_PAGESIZE)};
 if(!space->Map(range,GuestPermission::Read|GuestPermission::Write))return 5;
 Sink sink;sink.fail=argc>1;if(!space->SetCodeInvalidationSink(&sink))return 5;
 auto q=space->Quiesce(1000000);if(!q)return 5;std::vector<std::byte> bytes(range.size,std::byte{0x5a});
 if(!sink.fail){
  auto publishing=std::async(std::launch::async,[&]{return space->PublishCode(q.Value(),range,bytes);});
  sink.entered.get_future().wait();
  auto remap=space->RemapUnderToken(q.Value(),range,GuestPermission::Read|GuestPermission::Write);
  auto protect=space->ReprotectUnderToken(q.Value(),range,GuestPermission::Read|GuestPermission::Execute);
  printf("remap_during_sink_accepted=%d reprotect_during_sink_accepted=%d\n",bool(remap),bool(protect));
  sink.release.set_value();auto result=publishing.get();std::byte byte{};auto read=space->Read(range.base,{&byte,1});
  printf("publication_reported_success=%d expected_byte=90 actual_byte=%d read=%d\n",bool(result),int(byte),bool(read));
 }else{
  auto pub=space->PublishCode(q.Value(),range,bytes);auto remap=space->RemapUnderToken(q.Value(),range,GuestPermission::Read|GuestPermission::Execute);
  auto info=space->Query(range.base);printf("publication_failed=%d poisoned=%d remap_RX_accepted=%d mapping_executable=%d\n",!pub,space->HasPoisonedCode(),bool(remap),info&&HasPermission(info.Value().permission,GuestPermission::Execute));
 }
 space->ClearCodeInvalidationSink(&sink);
}
