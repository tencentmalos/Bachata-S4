#include "core/guest_cpu/api/address_space.h"
#include <array>
#include <cstdio>
#include <cstdlib>
using namespace Core::GuestCpu;
int main() {
 auto ar=GuestAddressSpace::Create({}),br=GuestAddressSpace::Create({});
 if(!ar||!br) return 2;
 auto a=std::move(ar).Value(), b=std::move(br).Value();
 GuestRange range{b->ReservationBase(),HostPageSize()};
 if(!b->Map(range,GuestPermission::Read|GuestPermission::Write)) return 3;
 auto qa=a->Quiesce(1000),qb=b->Quiesce(1000);
 if(!qa||!qb) return 4;
 std::array<std::byte,1> data{std::byte{0x42}};
 GuestRange byte{range.base,1};
 auto pub=b->PublishCode(qa.Value(),byte,data);
 auto inv=b->InvalidateCode(qa.Value(),byte,InvalidationReason::HostWrite);
 std::array<std::byte,1> observed{};
 if(!b->Read(byte.base,observed)) return 5;
 printf("foreign_token_is_from_target=%d same_epoch=%d publish_accepted=%d invalidate_accepted=%d byte=%u\n",qa.Value().IsFrom(b.get()),qa.Value().Epoch()==qb.Value().Epoch(),bool(pub),bool(inv),std::to_integer<unsigned>(observed[0]));
 auto reader=b->AcquirePinnedSpan(byte,false);
 if(!reader) return 6;
 reader.Value().Bytes()[0]=std::byte{0x99};
 if(!b->Read(byte.base,observed)) return 7;
 auto denied=b->Write(byte.base,data);
 printf("reader_writable_flag=%d read_pin_write_during_quiesce=%u explicit_write_accepted=%d\n",reader.Value().Writable(),std::to_integer<unsigned>(observed[0]),bool(denied));
}
