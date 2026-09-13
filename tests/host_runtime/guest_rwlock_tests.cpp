// SPDX-License-Identifier: GPL-2.0-or-later
#include <cstdio>
#include <future>
#include <thread>
#include "core/host_runtime/guest_rwlock.h"
#include "core/host_runtime/guest_mutex.h"
#include "core/host_runtime/guest_libc_policy.h"
using namespace Core::GuestCpu;
using namespace Core::HostRuntime;
int main() {
    unsigned checks{}, failures{};
    auto check = [&](bool ok) { ++checks; if (!ok) { ++failures; std::printf("FAIL %u\n", checks); } };
    AddressSpaceConfig config; config.reservation_size = 65536;
    auto made = GuestAddressSpace::Create(config); if (!made) return 2;
    auto space = std::move(made).Value(); const auto base = space->ReservationBase().value;
    if (!space->Map({GuestAddress{base},65536},GuestPermission::Read|GuestPermission::Write)) return 3;
    u64 allocated=base+4096;
    GuestRwlockDomain domain(*space,[&]{allocated+=64;return allocated;});
    check(domain.Init(0,0)==POSIX_EFAULT);
    check(domain.Init(base,0)==0);
    check(domain.Init(base,0)==POSIX_EBUSY);
    check(domain.Lock(base,1,false,false,{})==0);
    check(domain.Lock(base,2,false,true,{})==0); // independent readers coexist
    check(domain.Lock(base,3,true,true,{})==POSIX_EBUSY);
    check(domain.Lock(base,1,true,false,{})==POSIX_EDEADLK);
    check(domain.Unlock(base,9)==POSIX_EPERM);
    check(domain.Destroy(base)==POSIX_EBUSY);
    auto writer=std::async(std::launch::async,[&]{return domain.Lock(base,3,true,false,{});});
    auto deadline=std::chrono::steady_clock::now()+std::chrono::seconds(2);
    while(domain.Pending(base)==0 && std::chrono::steady_clock::now()<deadline) std::this_thread::yield();
    check(domain.Pending(base)==1 && space->Counts().live_pins==0);
    check(domain.Unlock(base,1)==0);
    check(writer.wait_for(std::chrono::milliseconds(0))!=std::future_status::ready);
    check(domain.Unlock(base,2)==0);
    if(writer.wait_for(std::chrono::seconds(2))!=std::future_status::ready) std::_Exit(4);
    check(writer.get()==0);
    check(domain.Lock(base,4,false,false,{},std::chrono::system_clock::now())==POSIX_ETIMEDOUT);
    std::stop_source cancel;
    auto reader=std::async(std::launch::async,[&]{return domain.Lock(base,4,false,false,cancel.get_token());});
    deadline=std::chrono::steady_clock::now()+std::chrono::seconds(2);
    while(domain.Pending(base)==0 && std::chrono::steady_clock::now()<deadline) std::this_thread::yield();
    check(domain.Pending(base)==1); cancel.request_stop();
    if(reader.wait_for(std::chrono::seconds(2))!=std::future_status::ready) std::_Exit(4);
    check(reader.get()==POSIX_EINTR);
    check(domain.Unlock(base,3)==0); check(domain.Destroy(base)==0);
    check(domain.Lock(base,1,false,true,{})==POSIX_EINVAL);
    check(domain.Attribute(base+8,0)==0);
    check(domain.Attribute(base+8,3,1)==POSIX_EINVAL);
    check(domain.Attribute(base+8,5,3)==POSIX_EINVAL);
    check(domain.Attribute(base+8,5,2)==0);
    check(domain.Init(base,base+8)==0); check(domain.Attribute(base+8,1)==0);
    check(domain.Lock(base,1,false,false,{})==0);
    check(domain.Lock(base,1,false,false,{})==POSIX_EDEADLK);
    check(domain.Unlock(base,1)==0); check(domain.Destroy(base)==0);
    check(domain.Lock(base+16,1,false,false,{})==0); // lazy static initialization
    check(domain.Unlock(base+16,1)==0); check(domain.Destroy(base+16)==0);
    std::recursive_mutex publication;
    GuestMutexDomain mutexes(*space,[&]{allocated+=64;return allocated;}, &publication);
    check(mutexes.AttributeInit(base+24)==0);
    check(mutexes.AttributePolicy(base+24,0,false,false)==0);
    check(mutexes.AttributePolicy(base+24,1,false,false)==0);
    check(mutexes.AttributePolicy(base+24,0,true,false)==POSIX_EFAULT);
    check(mutexes.AttributePolicy(base+24,1,false,true)==POSIX_EINVAL);
    check(mutexes.AttributePolicy(base+24,0,false,true)==0);
    auto read32=[&](u64 address){u32 v{}; if(!space->Read(GuestAddress{address},std::as_writable_bytes(std::span{&v,1})))std::_Exit(3);return v;};
    auto read64=[&](u64 address){u64 v{}; if(!space->Read(GuestAddress{address},std::as_writable_bytes(std::span{&v,1})))std::_Exit(3);return v;};
    check(mutexes.AttributePolicy(base+24,base+40,true,false)==0 && read32(base+40)==1);
    check(mutexes.AttributePolicy(base+24,3,false,false)==POSIX_EINVAL);
    check(mutexes.AttributeCeiling(base+24,700,false)==POSIX_EINVAL);
    check(mutexes.Init(base+32,base+24)==0);
    check(read32(read64(base+32)+20)==1); // actual guest-visible mutex prefix
    check(mutexes.AttributePolicy(base+24,2,false,false)==0);
    check(read32(read64(base+32)+20)==1); // create consumed a copy
    check(mutexes.AttributeCeiling(base+24,base+40,true)==0 && read32(base+40)==767);
    check(mutexes.AttributeCeiling(base+24,700,false)==0);
    check(mutexes.AttributeCeiling(base+24,base+40,true)==0 && read32(base+40)==700);
    check(mutexes.AttributeCeiling(base+24,255,false)==POSIX_EINVAL);
    check(mutexes.AttributeCeiling(base+24,768,false)==POSIX_EINVAL);
    check(mutexes.AttributeKind(read64(base+24))==1);
    check(mutexes.Attribute(base+24,0,0)==0);
    // Coordinator owns VM publication while an independent HLE owner tries to
    // write a mutex prefix. It must wait, then write once, with no guest pin.
    std::unique_lock vm(publication);
    auto token=space->Quiesce(0); if(!token) return 4;
    std::promise<void> entering;
    auto locking=std::async(std::launch::async,[&]{entering.set_value();return mutexes.Lock(base+32,1,false,{});});
    entering.get_future().get();
    check(locking.wait_for(std::chrono::milliseconds(50))!=std::future_status::ready && space->Counts().live_pins==0);
    token.Value()=QuiescenceToken{}; vm.unlock();
    if(locking.wait_for(std::chrono::seconds(2))!=std::future_status::ready)std::_Exit(4);
    check(locking.get()==0);
    check(mutexes.Lock(base+32,2,true,{})==POSIX_EBUSY);
    check(mutexes.Unlock(base+32,2)==POSIX_EPERM);
    check(mutexes.Unlock(base+32,1)==0);
    check(mutexes.Destroy(base+32)==0);
    check(IsGuestLibcFunction("gQX+4GDQjpM") && IsGuestLibcFunction("MUjC4lbHrK4") && !IsGuestLibcFunction("UnknownRunt"));
    std::printf("RWLOCK_LIBC checks=%u failures=%u\n",checks,failures);return failures?1:0;
}
