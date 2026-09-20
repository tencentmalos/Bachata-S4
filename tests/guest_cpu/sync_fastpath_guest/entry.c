/* SPDX-License-Identifier: GPL-2.0-or-later */
/* Test driver linked together with the production guest/runtime/sync payload.
 * It only calls the payload's exported entries the way a game import would. */
#include "../../../src/core/host_runtime/guest_sync_abi.h"
typedef __UINT64_TYPE__ u64;

int shad_pthread_mutex_lock(u64* slot);
int shad_pthread_mutex_trylock(u64* slot);
int shad_pthread_mutex_unlock(u64* slot);
u64 shad_scePthreadMutexLock(u64* slot);
u64 shad_scePthreadMutexTrylock(u64* slot);
u64 shad_scePthreadMutexUnlock(u64* slot);
u64 shad_pthread_self(void);

enum {
    ModePosixLoop = 0,   /* iterations x (lock, ++counter, unlock); first error or 0 */
    ModeTrylock = 1,     /* one trylock, no unlock */
    ModeLock = 2,        /* one lock, no unlock */
    ModeUnlock = 3,      /* one unlock */
    ModeSelf = 4,        /* pthread_self */
    ModeSceLoop = 5,     /* iterations x (sce lock, ++counter, sce unlock); first sce error or 0 */
    ModeSceTrylock = 6,  /* one scePthreadMutexTrylock */
    ModeRecursive = 7    /* lock twice, ++counter, unlock twice; first error or 0 */
};
struct Request {
    u64 mode, iterations;
    u64* slot;
    u64* counter;
};

__attribute__((visibility("default"))) u64 guest_entry(struct Request* r) {
    switch (r->mode) {
    case ModePosixLoop:
        for (u64 i = 0; i < r->iterations; ++i) {
            int e = shad_pthread_mutex_lock(r->slot);
            if (e) return (u64)e;
            ++*r->counter;
            e = shad_pthread_mutex_unlock(r->slot);
            if (e) return (u64)e;
        }
        return 0;
    case ModeTrylock: return (u64)shad_pthread_mutex_trylock(r->slot);
    case ModeLock: return (u64)shad_pthread_mutex_lock(r->slot);
    case ModeUnlock: return (u64)shad_pthread_mutex_unlock(r->slot);
    case ModeSelf: return shad_pthread_self();
    case ModeSceLoop:
        for (u64 i = 0; i < r->iterations; ++i) {
            u64 e = shad_scePthreadMutexLock(r->slot);
            if (e) return e;
            ++*r->counter;
            e = shad_scePthreadMutexUnlock(r->slot);
            if (e) return e;
        }
        return 0;
    case ModeSceTrylock: return shad_scePthreadMutexTrylock(r->slot);
    case ModeRecursive: {
        int e = shad_pthread_mutex_lock(r->slot);
        if (e) return (u64)e;
        e = shad_pthread_mutex_lock(r->slot);
        if (e) return (u64)e;
        ++*r->counter;
        e = shad_pthread_mutex_unlock(r->slot);
        if (e) return (u64)e;
        return (u64)shad_pthread_mutex_unlock(r->slot);
    }
    default: return ~(u64)0;
    }
}
