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
    // Lock objects now carry their state in guest memory, so every allocated object
    // (including the 5000-lock scale run at base+1 MiB) must be mapped.
    AddressSpaceConfig config; config.reservation_size = 8ULL << 20;
    auto made = GuestAddressSpace::Create(config); if (!made) return 2;
    auto space = std::move(made).Value(); const auto base = space->ReservationBase().value;
    if (!space->Map({GuestAddress{base},8ULL << 20},GuestPermission::Read|GuestPermission::Write)) return 3;
    u64 allocated=base+0x20000; // clear of the slot ranges used below
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
    // Guest-visible state shared with the guest fast path: the word, the write owner
    // and a real per-thread hold table (owners above are bare ids without a table).
    {
        auto r32=[&](u64 a){u32 v{}; if(!space->Read(GuestAddress{a},std::as_writable_bytes(std::span{&v,1})))std::_Exit(3);return v;};
        auto r64=[&](u64 a){u64 v{}; if(!space->Read(GuestAddress{a},std::as_writable_bytes(std::span{&v,1})))std::_Exit(3);return v;};
        const u64 thread = base + 0x40000 + SHAD_SYNC_RW_HOLDS_BELOW_HANDLE; // a thread handle
        const u64 holds = thread - SHAD_SYNC_RW_HOLDS_BELOW_HANDLE;
        const u64 s = base + 0x30000;
        check(domain.Init(s,0)==0);
        const u64 obj = r64(s);
        check(domain.Lock(s,thread,false,false,{})==0);
        check(r32(obj)==1 && r64(holds+16)==obj && r64(holds+24)==1);
        check(domain.Lock(s,thread,false,false,{})==0); // recursion: table depth only
        check(r32(obj)==1 && r64(holds+24)==2);
        check(domain.Lock(s,thread,true,true,{})==POSIX_EBUSY);
        check(domain.Unlock(s,thread)==0 && domain.Unlock(s,thread)==0);
        check(r32(obj)==0 && r64(holds+16)==0);
        check(domain.Unlock(s,thread)==POSIX_EPERM);
        check(domain.Lock(s,thread,true,false,{})==0);
        check(r32(obj)==SHAD_SYNC_RW_WRITE_LOCKED && r64(obj+SHAD_SYNC_RW_WRITER)==thread);
        check(domain.Lock(s,thread,false,true,{})==POSIX_EBUSY);
        check(domain.Unlock(s,thread)==0 && r32(obj)==0 && r64(obj+SHAD_SYNC_RW_WRITER)==0);
        // A full table spills into the host's overflow list and flags the thread.
        for (unsigned i=0;i<16;++i) check(domain.Init(s+8+8*i,0)==0 && domain.Lock(s+8+8*i,thread,false,false,{})==0);
        bool counted = r64(holds)==1;
        for (unsigned i=0;i<16;++i) counted &= r32(r64(s+8+8*i))==1;
        check(counted);
        check(domain.Lock(s+8+8*15,thread,false,false,{})==0); // recursive read of the overflow hold
        check(domain.Unlock(s+8+8*15,thread)==0 && r32(r64(s+8+8*15))==1);
        bool released = true;
        for (unsigned i=0;i<16;++i) released &= domain.Unlock(s+8+8*i,thread)==0 && r32(r64(s+8+8*i))==0;
        check(released && r64(holds)==0);
        // Type 1: a parked writer keeps new readers out; the thread already reading
        // may read again.
        check(domain.Attribute(s+0x100,0)==0 && domain.Attribute(s+0x100,5,1)==0 && domain.Init(s+0x108,s+0x100)==0);
        const u64 pref = s+0x108, pobj = r64(pref);
        check(domain.Lock(pref,thread,false,false,{})==0);
        auto parked=std::async(std::launch::async,[&]{return domain.Lock(pref,7,true,false,{});});
        auto until=std::chrono::steady_clock::now()+std::chrono::seconds(2);
        while(domain.Pending(pref)==0 && std::chrono::steady_clock::now()<until) std::this_thread::yield();
        check(domain.Pending(pref)==1 && (r32(pobj)&SHAD_SYNC_RW_WRITE_WAITERS));
        check(domain.Lock(pref,8,false,true,{})==POSIX_EBUSY);
        check(domain.Lock(pref,thread,false,false,{})==0);
        check(domain.Unlock(pref,thread)==0 && parked.wait_for(std::chrono::milliseconds(0))!=std::future_status::ready);
        check(domain.Unlock(pref,thread)==0);
        if(parked.wait_for(std::chrono::seconds(2))!=std::future_status::ready) std::_Exit(4);
        check(parked.get()==0 && r32(pobj)==SHAD_SYNC_RW_WRITE_LOCKED && r64(pobj+SHAD_SYNC_RW_WRITER)==7);
        check(domain.Unlock(pref,7)==0 && r32(pobj)==0);
        // A writer that gives up lets in the readers it kept out.
        check(domain.Lock(pref,thread,false,false,{})==0);
        std::stop_source give_up;
        auto writer2=std::async(std::launch::async,[&]{return domain.Lock(pref,7,true,false,give_up.get_token());});
        until=std::chrono::steady_clock::now()+std::chrono::seconds(2);
        while(domain.Pending(pref)==0 && std::chrono::steady_clock::now()<until) std::this_thread::yield();
        auto reader2=std::async(std::launch::async,[&]{return domain.Lock(pref,8,false,false,{});});
        until=std::chrono::steady_clock::now()+std::chrono::seconds(2);
        while(domain.Pending(pref)<2 && std::chrono::steady_clock::now()<until) std::this_thread::yield();
        check(domain.Pending(pref)==2);
        give_up.request_stop();
        if(writer2.wait_for(std::chrono::seconds(2))!=std::future_status::ready ||
           reader2.wait_for(std::chrono::seconds(2))!=std::future_status::ready) std::_Exit(4);
        check(writer2.get()==POSIX_EINTR && reader2.get()==0 && r32(pobj)==2);
        check(domain.Unlock(pref,8)==0 && domain.Unlock(pref,thread)==0 && r32(pobj)==0);
        // Destroy refuses a held lock and marks the word of a destroyed one.
        check(domain.Lock(s,thread,false,false,{})==0 && domain.Destroy(s)==POSIX_EBUSY);
        check(domain.Unlock(s,thread)==0 && domain.Destroy(s)==0 && r32(obj)==SHAD_SYNC_RW_DESTROYED && r64(s)==1);
        check(domain.Lock(s,thread,false,false,{})==POSIX_EINVAL && domain.Unlock(s,thread)==POSIX_EINVAL);
        bool destroyed = domain.Destroy(pref)==0;
        for (unsigned i=0;i<16;++i) destroyed &= domain.Destroy(s+8+8*i)==0;
        check(destroyed);
    }
    GuestMutexDomain mutexes(*space, [&] {
        allocated += 64;
        return allocated;
    });
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
    auto token = space->Quiesce(0);
    if (!token)
        return 4;
    std::promise<void> entering;
    auto locking=std::async(std::launch::async,[&]{entering.set_value();return mutexes.Lock(base+32,1,false,{});});
    entering.get_future().get();
    check(locking.wait_for(std::chrono::milliseconds(50))!=std::future_status::ready && space->Counts().live_pins==0);
    token.Value() = QuiescenceToken{};
    if(locking.wait_for(std::chrono::seconds(2))!=std::future_status::ready)std::_Exit(4);
    check(locking.get()==0);
    check(mutexes.Lock(base+32,2,true,{})==POSIX_EBUSY);
    check(mutexes.Unlock(base+32,2)==POSIX_EPERM);
    check(mutexes.Unlock(base+32,1)==0);
    check(mutexes.Destroy(base+32)==0);
    check(IsGuestLibcFunction("gQX+4GDQjpM") && IsGuestLibcFunction("MUjC4lbHrK4") && !IsGuestLibcFunction("UnknownRunt"));
    // MHR initializes thousands of simultaneous locks. Lifetime churn must not
    // exhaust an artificial quota either; all handles remain distinct.
    u64 next_handle = base + 0x100000;
    bool exhausted = false;
    GuestRwlockDomain many(*space, [&] { return exhausted ? u64{0} : (next_handle += 0x100); });
    unsigned create_errors{}, operation_errors{}, churn_errors{};
    for (unsigned i = 0; i < 5000; ++i) {
        const auto slot = base + 512 + i * 8;
        if (many.Init(slot, 0)) { ++create_errors; continue; }
        if (many.Lock(slot, 1, true, true, {})) ++operation_errors;
        if (many.Unlock(slot, 1)) ++operation_errors;
    }
    for (unsigned i = 0; i < 5000; ++i) {
        const auto slot = base + 512 + i * 8;
        if (many.Destroy(slot)) ++operation_errors;
    }
    check(create_errors == 0);
    check(operation_errors == 0);
    for (unsigned i = 0; i < 5000; ++i) {
        if (many.Attribute(base + 512, 0)) ++churn_errors;
        if (many.Attribute(base + 512, 1)) ++churn_errors;
    }
    check(churn_errors == 0);
    exhausted = true;
    const auto before = read64(base + 520);
    check(many.Init(base + 520, 0) == POSIX_ENOMEM);
    check(read64(base + 520) == before);
    check(many.Attribute(base + 512, 0) == POSIX_ENOMEM);
    check(read64(base + 512) == 0);
    check(many.Lock(base + 512, 1, false, true, {}) == POSIX_ENOMEM);
    check(read64(base + 512) == 0);
    exhausted = false;
    check(many.Init(base + 520, 0) == 0);
    check(many.Lock(base + 520, 1, false, true, {}) == 0);
    check(many.Unlock(base + 520, 1) == 0);
    check(many.Destroy(base + 520) == 0);
    std::printf("RWLOCK_SCALE create_errors=%u operation_errors=%u churn_errors=%u\n",
                create_errors, operation_errors, churn_errors);
    std::printf("RWLOCK_LIBC checks=%u failures=%u\n",checks,failures);return failures?1:0;
}
