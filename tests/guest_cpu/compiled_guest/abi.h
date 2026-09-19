// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once
// Freestanding shared wire layout: no Android headers or host STL in guest code.
typedef __UINT32_TYPE__ guest_u32;
typedef __UINT64_TYPE__ guest_u64;
typedef guest_u64 (*GuestSlowCall)(guest_u64 address, guest_u64 operation);
enum { GuestWait = 0, GuestWake = 1, GuestCrossingProbe = 2 };
struct GuestMutexProbe {
    guest_u32 state; // 0 free, 1 held, 2 held with possible waiters
    guest_u32 reserved;
    guest_u64 value;
};
struct GuestRequest {
    guest_u64 mode;
    guest_u64 iterations;
    struct GuestMutexProbe* mutex;
    GuestSlowCall slow; // guest veneer VA, never a native ARM64 function pointer
    guest_u64 output;
    double fp_output;
};
#ifdef __cplusplus
static_assert(sizeof(GuestMutexProbe) == 16 && sizeof(GuestRequest) == 48);
extern "C" {
#endif
guest_u64 guest_c_sum8(guest_u64, guest_u64, guest_u64, guest_u64, guest_u64, guest_u64, guest_u64,
                       guest_u64);
double guest_c_scale(double value, double scale);
#ifdef __cplusplus
}
#endif
