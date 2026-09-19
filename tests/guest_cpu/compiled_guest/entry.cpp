// SPDX-License-Identifier: GPL-2.0-or-later
#include "abi.h"

namespace {
// Mechanism probe, NOT the Orbis pthread ABI. One guest atomic word is the
// sole lock state. The host only implements expected-value wait/wake.
class Lock {
public:
    Lock(GuestMutexProbe& m, GuestSlowCall slow) : m(m), slow(slow) {
        guest_u32 expected = 0;
        if (__atomic_compare_exchange_n(&m.state, &expected, 1, false, __ATOMIC_ACQUIRE,
                                        __ATOMIC_RELAXED))
            return;
        while (__atomic_exchange_n(&m.state, 2, __ATOMIC_ACQUIRE) != 0)
            slow(reinterpret_cast<guest_u64>(&m.state), GuestWait);
    }
    ~Lock() {
        if (__atomic_exchange_n(&m.state, 0, __ATOMIC_RELEASE) == 2)
            slow(reinterpret_cast<guest_u64>(&m.state), GuestWake);
    }
    Lock(const Lock&) = delete;
    Lock& operator=(const Lock&) = delete;

private:
    GuestMutexProbe& m;
    GuestSlowCall slow;
};
} // namespace

extern "C" guest_u64 guest_entry(GuestRequest* request) {
    if (request->mode == 0) {
        request->output = guest_c_sum8(1, 2, 3, 4, 5, 6, 7, request->iterations);
        request->fp_output = guest_c_scale(1.5, 4.0);
        return request->output;
    }
    if (request->mode > 2)
        return ~guest_u64{0};
    for (guest_u64 i = 0; i < request->iterations; ++i) {
        if (request->mode == 2)
            request->slow(0, GuestCrossingProbe);
        {
            Lock lock(*request->mutex, request->slow);
            ++request->mutex->value;
        } // real C++ destructor, emitted by the guest compiler
        if (request->mode == 2)
            request->slow(0, GuestCrossingProbe);
    }
    return request->iterations;
}
