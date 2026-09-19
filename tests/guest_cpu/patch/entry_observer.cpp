#include "shad_entry.h"
struct Triple { shad_u64 a, b, c; };
static shad_u64 counts[8];
extern "C" void clobber_observer_state();
extern "C" void observe(const ShadGuestEntryContext* c) {
    __atomic_add_fetch(&counts[0], 1, __ATOMIC_RELAXED);
    if (c->gpr->rdi == 1 && c->gpr->rsi == 2 && c->gpr->rdx == 3 &&
        c->gpr->rcx == 4 && c->gpr->r8 == 5 && c->gpr->r9 == 6 &&
        reinterpret_cast<const shad_u64*>(c->rsp)[1] == 7 &&
        reinterpret_cast<const shad_u64*>(c->rsp)[2] == 8)
        __atomic_add_fetch(&counts[1], 1, __ATOMIC_RELAXED);
    // An actual HLE crossing must also leave the opaque entry state intact.
    shad_sdk_counter(1, 1);
    clobber_observer_state();
}
extern "C" shad_u64 entry_count() { return __atomic_load_n(&counts[0], __ATOMIC_RELAXED); }
extern "C" shad_u64 argument_count() { return __atomic_load_n(&counts[1], __ATOMIC_RELAXED); }
extern "C" shad_u64 probe_observer_fp(shad_u64 fp, shad_u64 sret) {
    const auto v = reinterpret_cast<double(*)(double,double)>(fp)(1.25, 2.5);
    const auto s = reinterpret_cast<Triple(*)(shad_u64)>(sret)(5);
    return v == 3.75 && s.a == 5 && s.b == 6 && s.c == 7;
}
extern "C" shad_u64 loop_observer(shad_u64 address, shad_u64 n) {
    auto f = reinterpret_cast<shad_u64(*)(shad_u64,shad_u64,shad_u64,shad_u64,
                                        shad_u64,shad_u64,shad_u64,shad_u64)>(address);
    shad_u64 sum = 0;
    for (shad_u64 i=0; i<n; ++i) sum += f(1,2,3,4,5,6,7,8);
    return sum;
}
