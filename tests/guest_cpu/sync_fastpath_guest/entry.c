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
u64 shad_pthread_getspecific(u64 key);
u64 shad_pthread_getthreadid(void);
u64 shad_pthread_setspecific(u64 key, u64 value);
u64 shad_pthread_equal(u64 a, u64 b);
u64 shad_sceKernelGetProcessTimeCounter(void);
u64 shad_sceKernelReadTsc(void);
u64 shad_sceKernelGetTscFrequency(void);
int shad_pthread_rwlock_rdlock(u64* slot);
int shad_pthread_rwlock_tryrdlock(u64* slot);
int shad_pthread_rwlock_wrlock(u64* slot);
int shad_pthread_rwlock_trywrlock(u64* slot);
int shad_pthread_rwlock_unlock(u64* slot);
u64 shad_scePthreadRwlockRdlock(u64* slot);
u64 shad_scePthreadRwlockWrlock(u64* slot);
u64 shad_scePthreadRwlockUnlock(u64* slot);

enum {
    ModePosixLoop = 0,   /* iterations x (lock, ++counter, unlock); first error or 0 */
    ModeTrylock = 1,     /* one trylock, no unlock */
    ModeLock = 2,        /* one lock, no unlock */
    ModeUnlock = 3,      /* one unlock */
    ModeSelf = 4,        /* pthread_self */
    ModeSceLoop = 5,     /* iterations x (sce lock, ++counter, sce unlock); first sce error or 0 */
    ModeSceTrylock = 6,  /* one scePthreadMutexTrylock */
    ModeRecursive = 7,   /* lock twice, ++counter, unlock twice; first error or 0 */
    ModeGetspecific = 8, /* pthread_getspecific(iterations as the raw rdi key) */
    ModeGetthreadid = 9, /* scePthreadGetthreadid */
    ModeSetspecific = 10, /* pthread_setspecific(iterations as the raw rdi key, slot as value) */
    ModeEqual = 11,       /* pthread_equal(iterations, slot) */
    ModeProcessTime = 12, /* sceKernelGetProcessTimeCounter */
    ModeReadTsc = 13,     /* sceKernelReadTsc */
    ModeTscFrequency = 14, /* sceKernelGetTscFrequency */
    ModeRwReadLoop = 15,   /* iterations x (rdlock, unlock); first error or 0 */
    ModeRwWriteLoop = 16,  /* iterations x (wrlock, ++counter, unlock); first error or 0 */
    ModeRwRead = 17,       /* one rdlock */
    ModeRwTryRead = 18,    /* one tryrdlock */
    ModeRwWrite = 19,      /* one wrlock */
    ModeRwTryWrite = 20,   /* one trywrlock */
    ModeRwUnlock = 21,     /* one unlock */
    ModeRwSceLoop = 22,    /* iterations x (sce rd, sce unlock, sce wr, ++counter, sce unlock) */
    ModeRwReadMany = 23,   /* rdlock slot[0..iterations); first error or 0 */
    ModeRwUnlockMany = 24, /* unlock slot[iterations-1..0]; first error or 0 */
    /* iterations x: every 8th a writer (++counter), otherwise a reader that checks
     * the counter does not move under its read lock (99 if it does). */
    ModeRwMixedLoop = 25
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
    case ModeGetspecific: return shad_pthread_getspecific(r->iterations);
    case ModeGetthreadid: return shad_pthread_getthreadid();
    case ModeSetspecific: return shad_pthread_setspecific(r->iterations, (u64)r->slot);
    case ModeEqual: return shad_pthread_equal(r->iterations, (u64)r->slot);
    case ModeProcessTime: return shad_sceKernelGetProcessTimeCounter();
    case ModeReadTsc: return shad_sceKernelReadTsc();
    case ModeTscFrequency: return shad_sceKernelGetTscFrequency();
    case ModeRwReadLoop:
        for (u64 i = 0; i < r->iterations; ++i) {
            int e = shad_pthread_rwlock_rdlock(r->slot);
            if (e) return (u64)e;
            e = shad_pthread_rwlock_unlock(r->slot);
            if (e) return (u64)e;
        }
        return 0;
    case ModeRwWriteLoop:
        for (u64 i = 0; i < r->iterations; ++i) {
            int e = shad_pthread_rwlock_wrlock(r->slot);
            if (e) return (u64)e;
            ++*r->counter;
            e = shad_pthread_rwlock_unlock(r->slot);
            if (e) return (u64)e;
        }
        return 0;
    case ModeRwRead: return (u64)shad_pthread_rwlock_rdlock(r->slot);
    case ModeRwTryRead: return (u64)shad_pthread_rwlock_tryrdlock(r->slot);
    case ModeRwWrite: return (u64)shad_pthread_rwlock_wrlock(r->slot);
    case ModeRwTryWrite: return (u64)shad_pthread_rwlock_trywrlock(r->slot);
    case ModeRwUnlock: return (u64)shad_pthread_rwlock_unlock(r->slot);
    case ModeRwSceLoop:
        for (u64 i = 0; i < r->iterations; ++i) {
            u64 e = shad_scePthreadRwlockRdlock(r->slot);
            if (!e) e = shad_scePthreadRwlockUnlock(r->slot);
            if (!e) e = shad_scePthreadRwlockWrlock(r->slot);
            if (e) return e;
            ++*r->counter;
            e = shad_scePthreadRwlockUnlock(r->slot);
            if (e) return e;
        }
        return 0;
    case ModeRwReadMany:
        for (u64 i = 0; i < r->iterations; ++i) {
            const int e = shad_pthread_rwlock_rdlock(r->slot + i);
            if (e) return (u64)e;
        }
        return 0;
    case ModeRwUnlockMany:
        for (u64 i = r->iterations; i-- > 0;) {
            const int e = shad_pthread_rwlock_unlock(r->slot + i);
            if (e) return (u64)e;
        }
        return 0;
    case ModeRwMixedLoop:
        for (u64 i = 0; i < r->iterations; ++i) {
            const int write = (i & 7) == 0;
            int e = write ? shad_pthread_rwlock_wrlock(r->slot) : shad_pthread_rwlock_rdlock(r->slot);
            if (e) return (u64)e;
            if (write) {
                ++*r->counter;
            } else {
                const u64 before = *(volatile u64*)r->counter;
                for (int spin = 0; spin < 16; ++spin) __builtin_ia32_pause();
                if (*(volatile u64*)r->counter != before) return 99;
            }
            e = shad_pthread_rwlock_unlock(r->slot);
            if (e) return (u64)e;
        }
        return 0;
    default: return ~(u64)0;
    }
}
