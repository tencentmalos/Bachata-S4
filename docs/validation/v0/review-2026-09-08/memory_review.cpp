#include "core/guest_cpu/api/address_space.h"
#include "core/guest_cpu/hle/call_adapter.h"
#include <cstdio>
using namespace Core::GuestCpu;
int main() {
 auto r=GuestAddressSpace::Create(AddressSpaceConfig{}); auto s=std::move(r).Value();
 auto base=s->ReservationBase(); auto page=HostPageSize();
 GuestRange both{base,2*page}, one{base,page};
 if (!s->Map(both,GuestPermission::Read)) return 2;
 auto changed=s->Protect(one,GuestPermission::Read|GuestPermission::Write);
 auto second=s->ValidateRange({GuestAddress{base.value+page},page},GuestPermission::Write);
 printf("partial_protect=%d untouched_second_page_reported_writable=%d\n",bool(changed),bool(second));
 if (!s->Protect(both,GuestPermission::Read|GuestPermission::Write)) return 3;
 {auto pin=s->AcquirePinnedSpan(one,true); auto prot=s->Protect(one,GuestPermission::Read);
 printf("writer_pin=%d protect_while_pinned=%d\n",bool(pin),bool(prot));}
 if (!s->Protect(both,GuestPermission::Read|GuestPermission::Write)) return 4;
 auto q=s->Quiesce(1000000); auto pin=s->AcquirePinnedSpan(one,true);
 printf("quiesce=%d new_writer_while_quiesced=%d\n",bool(q),bool(pin));
 return 0;
}
