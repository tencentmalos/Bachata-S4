/* SPDX-License-Identifier: GPL-2.0-or-later */
/* App-shipped guest fast path for Orbis/POSIX mutexes. Freestanding x86-64 C:
 * no libc, TLS, globals or constructors. Built by scripts/android/build-guest-payload
 * and published by the host runtime at load time for every title.
 *
 * Algorithm: FreeBSD libthr mutex_lock_common as ported by desktop
 * src/core/libraries/kernel/threads/mutex.cpp (self-ownership by type, bounded
 * spin, then block), with the umtx wait/wake replaced by two host primitives and
 * the lock word using the three-state protocol validated by
 * tests/guest_cpu/compiled_guest (0 free, 1 held, 2 held with possible waiters).
 * The host HLE implements the same word protocol, so guest and host callers
 * interoperate on one object. */
#include "../../../src/core/host_runtime/guest_sync_abi.h"

typedef __UINT32_TYPE__ u32;
typedef __UINT64_TYPE__ u64;

/* POSIX errno values returned by pthread_* (never -1/errno for this family). */
enum { EPERM = 1, EINTR = 4, EDEADLK = 11, EBUSY = 16, EAGAIN = 35 };
#define SCE_ERROR(e) ((u64)0x80020000u | (u64)(u32)(e))

struct Mutex {
    u64 owner;
    u32 count, spins, yields, protocol;
    u32 state, unused;
    u32 flags;
};
_Static_assert(__builtin_offsetof(struct Mutex, state) == SHAD_SYNC_MUTEX_STATE, "state");
_Static_assert(__builtin_offsetof(struct Mutex, flags) == SHAD_SYNC_MUTEX_FLAGS, "flags");
_Static_assert(sizeof(struct Mutex) == SHAD_SYNC_MUTEX_PREFIX_SIZE, "prefix");

/* Host-filled before the image becomes executable; volatile so the zero
 * initializer is never constant-folded. Read-only at run time. */
__attribute__((section(".shad_imports"), used, visibility("default")))
const volatile u64 shad_sync_imports[ShadSyncImportCount];

typedef u64 (*WaitFn)(u64 address, u64 expected, u64 width);
typedef u64 (*WakeFn)(u64 address, u64 count);
typedef int (*SlowFn)(u64* slot);

static inline u64 Self(void) {
    /* Tcb::tcb_thread: the runtime thread handle also returned by pthread_self. */
    u64 value;
    __asm__ __volatile__("movq %%fs:0x10, %0" : "=r"(value));
    return value;
}
static inline struct Mutex* Object(u64 address) {
    /* Static initializers (0/1), destroyed (2) and anything outside the arena
     * take the checked HLE path, which also performs lazy initialization. */
    if (address - SHAD_SYNC_ARENA_BASE >= SHAD_SYNC_ARENA_LIMIT - SHAD_SYNC_ARENA_BASE)
        return 0;
    return (struct Mutex*)address;
}

static int LockCommon(u64* slot, int try_only) {
    struct Mutex* m = Object(*slot);
    if (!m)
        return ((SlowFn)shad_sync_imports[try_only ? ShadSyncImportMutexTrylock
                                                   : ShadSyncImportMutexLock])(slot);
    const u64 self = Self();
    u32 expected = SHAD_SYNC_STATE_FREE;
    if (__atomic_compare_exchange_n(&m->state, &expected, SHAD_SYNC_STATE_HELD, 0,
                                    __ATOMIC_ACQUIRE, __ATOMIC_RELAXED)) {
        m->owner = self;
        m->count = 1;
        return 0;
    }
    if (m->owner == self) {
        /* Same order as the host HLE: recursive depth, then try, then error. */
        const u32 type = m->flags & SHAD_SYNC_TYPE_MASK;
        if (type == SHAD_SYNC_TYPE_RECURSIVE) {
            if (m->count == 0xffffffffu)
                return EAGAIN;
            ++m->count;
            return 0;
        }
        if (try_only)
            return EBUSY;
        if (type != SHAD_SYNC_TYPE_NORMAL)
            return EDEADLK;
        /* Normal type deadlocks on self-lock, as libthr/desktop do. */
    }
    if (try_only)
        return EBUSY;
    for (u32 spins = m->spins; spins; --spins) {
        __builtin_ia32_pause();
        expected = SHAD_SYNC_STATE_FREE;
        if (__atomic_load_n(&m->state, __ATOMIC_RELAXED) == SHAD_SYNC_STATE_FREE &&
            __atomic_compare_exchange_n(&m->state, &expected, SHAD_SYNC_STATE_HELD, 0,
                                        __ATOMIC_ACQUIRE, __ATOMIC_RELAXED)) {
            m->owner = self;
            m->count = 1;
            return 0;
        }
    }
    while (__atomic_exchange_n(&m->state, SHAD_SYNC_STATE_CONTENDED, __ATOMIC_ACQUIRE) !=
           SHAD_SYNC_STATE_FREE) {
        const u64 r = ((WaitFn)shad_sync_imports[ShadSyncImportWait])(
            (u64)&m->state, SHAD_SYNC_STATE_CONTENDED, sizeof(u32));
        /* Cancellation or a refused address ends the wait without ownership;
         * the contended state is conservative and only costs one extra wake. */
        if (r)
            return (int)r;
    }
    m->owner = self;
    m->count = 1;
    return 0;
}

static int UnlockCommon(u64* slot) {
    struct Mutex* m = Object(*slot);
    if (!m)
        return ((SlowFn)shad_sync_imports[ShadSyncImportMutexUnlock])(slot);
    if (m->owner != Self())
        return EPERM;
    if (m->count > 1) {
        --m->count;
        return 0;
    }
    m->count = 0;
    m->owner = 0;
    if (__atomic_exchange_n(&m->state, SHAD_SYNC_STATE_FREE, __ATOMIC_RELEASE) ==
        SHAD_SYNC_STATE_CONTENDED)
        ((WakeFn)shad_sync_imports[ShadSyncImportWake])((u64)&m->state, 1);
    return 0;
}

#define EXPORT __attribute__((visibility("default"), used))
EXPORT int shad_pthread_mutex_lock(u64* slot) { return LockCommon(slot, 0); }
EXPORT int shad_pthread_mutex_trylock(u64* slot) { return LockCommon(slot, 1); }
EXPORT int shad_pthread_mutex_unlock(u64* slot) { return UnlockCommon(slot); }
EXPORT u64 shad_scePthreadMutexLock(u64* slot) {
    const int e = LockCommon(slot, 0);
    return e ? SCE_ERROR(e) : 0;
}
EXPORT u64 shad_scePthreadMutexTrylock(u64* slot) {
    const int e = LockCommon(slot, 1);
    return e ? SCE_ERROR(e) : 0;
}
EXPORT u64 shad_scePthreadMutexUnlock(u64* slot) {
    const int e = UnlockCommon(slot);
    return e ? SCE_ERROR(e) : 0;
}
EXPORT u64 shad_pthread_self(void) { return Self(); }
