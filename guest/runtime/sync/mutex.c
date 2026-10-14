/* SPDX-License-Identifier: GPL-2.0-or-later */
/* App-shipped guest fast path for Orbis/POSIX mutexes (and rwlocks, thread-specific
 * data and leaf calls further down). Freestanding x86-64 C:
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
enum { EPERM = 1, EINTR = 4, EDEADLK = 11, EBUSY = 16, EINVAL = 22, EAGAIN = 35 };
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
/* Host-filled arena window [base, limit) of real mutex objects; both zero until
 * the host reserved a window, which sends every object to the HLE path. */
__attribute__((section(".shad_imports"), used, visibility("default")))
const volatile u64 shad_sync_window[ShadSyncWindowCount];

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
    /* Static initializers (0/1), destroyed (2) and anything outside the
     * host-published arena window take the checked HLE path, which also
     * performs lazy initialization. Two RIP-relative loads; no HLE crossing. */
    const u64 base = shad_sync_window[ShadSyncWindowBase];
    if (address - base >= shad_sync_window[ShadSyncWindowLimit] - base)
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

/* pthread_getspecific / scePthreadGetspecific: same result as the host HLE
 * (0 for an out-of-range, unallocated or recreated key), read from the key
 * table and the calling thread's records below its handle. */
struct SpecificValue {
    u64 sequence, value;
};
_Static_assert(SHAD_SYNC_TLS_KEYS * sizeof(struct SpecificValue) ==
                   SHAD_SYNC_TLS_VALUES_BELOW_HANDLE, "values");
EXPORT u64 shad_pthread_getspecific(u64 key_arg) {
    const u32 key = (u32)key_arg; /* int key: the upper half of rdi is not defined */
    if (key >= SHAD_SYNC_TLS_KEYS)
        return 0;
    const volatile u64* keys = (const volatile u64*)shad_sync_window[ShadSyncWindowTlsKeys];
    const u64 word = keys[key];
    if (!(word & 1))
        return 0;
    const volatile struct SpecificValue* value =
        (const volatile struct SpecificValue*)(Self() - SHAD_SYNC_TLS_VALUES_BELOW_HANDLE) + key;
    return value->sequence == (word >> 1) ? value->value : 0;
}

EXPORT u64 shad_pthread_getthreadid(void) {
    return *(const volatile u64*)(Self() + SHAD_SYNC_HANDLE_THREAD_ID);
}

/* pthread_setspecific / scePthreadSetspecific: same result as the host HLE (EINVAL
 * for an out-of-range or unallocated key under both names). Only the calling thread
 * writes its own record, so the pair needs no ordering against other threads. */
EXPORT u64 shad_pthread_setspecific(u64 key_arg, u64 value) {
    const u32 key = (u32)key_arg;
    if (key >= SHAD_SYNC_TLS_KEYS)
        return EINVAL;
    const volatile u64* keys = (const volatile u64*)shad_sync_window[ShadSyncWindowTlsKeys];
    const u64 word = keys[key];
    if (!(word & 1))
        return EINVAL;
    volatile struct SpecificValue* record =
        (volatile struct SpecificValue*)(Self() - SHAD_SYNC_TLS_VALUES_BELOW_HANDLE) + key;
    record->sequence = word >> 1;
    record->value = value;
    return 0;
}

/* pthread_equal / scePthreadEqual: the HLE compares the full 64-bit handles. */
EXPORT u64 shad_pthread_equal(u64 a, u64 b) {
    return a == b;
}

static inline u64 Rdtsc(void) {
    u32 lo, hi;
    __asm__ volatile("rdtsc" : "=a"(lo), "=d"(hi));
    return ((u64)hi << 32) | lo;
}
EXPORT u64 shad_sceKernelGetProcessTimeCounter(void) {
    return Rdtsc() - shad_sync_window[ShadSyncWindowClockOrigin];
}
EXPORT u64 shad_sceKernelReadTsc(void) {
    return Rdtsc();
}
EXPORT u64 shad_sceKernelGetTscFrequency(void) {
    return shad_sync_window[ShadSyncWindowTscFrequency];
}

/* pthread_rwlock / scePthreadRwlock. UE4 takes a read lock on its pipeline-state
 * cache for every draw (KH3 render thread: ~19k rdlock/unlock pairs a second), so
 * the uncontended paths must not leave the guest.
 *
 * Word protocol (host guest_rwlock.h is the slow path on the same word):
 *   read:   not held by this thread, no writer, and (type 0 or no parked writer)
 *           -> CAS count+1 and record the hold in this thread's table; recursion
 *           only bumps the table depth (type 2 refuses it). Anything else -> HLE.
 *   write:  no writer and no readers -> CAS set WRITE_LOCKED (waiter bits kept),
 *           writer = self. Anything else -> HLE (blocking, EDEADLK, EBUSY).
 *   unlock: writer == self -> clear WRITE_LOCKED; else drop this thread's hold and
 *           decrement the count. A release that leaves waiter bits behind (writer
 *           released, or the last reader gone) wakes every waiter parked on the
 *           word; they re-check under the host's per-lock lock. Holds the table
 *           does not know -> HLE (EPERM, or a hold the host kept as overflow). */
struct Rwlock {
    u32 word, type;
    u64 writer;
};
_Static_assert(__builtin_offsetof(struct Rwlock, word) == SHAD_SYNC_RW_WORD, "rw word");
_Static_assert(__builtin_offsetof(struct Rwlock, type) == SHAD_SYNC_RW_TYPE, "rw type");
_Static_assert(__builtin_offsetof(struct Rwlock, writer) == SHAD_SYNC_RW_WRITER, "rw writer");
_Static_assert(sizeof(struct Rwlock) == SHAD_SYNC_RW_PREFIX_SIZE, "rw prefix");
struct RwHold {
    u64 lock, depth;
};
struct RwHolds {
    u64 overflow, reserved;
    struct RwHold entries[SHAD_SYNC_RW_HOLD_ENTRIES];
};
_Static_assert(sizeof(struct RwHolds) == SHAD_SYNC_RW_HOLDS_SIZE, "rw holds");
#define RW_WAITERS (SHAD_SYNC_RW_WRITE_WAITERS | SHAD_SYNC_RW_READ_WAITERS)
#define RW_WAKE_ALL (~(u64)0)

static inline struct Rwlock* RwObject(u64 address) {
    const u64 base = shad_sync_window[ShadSyncWindowRwlockBase];
    if (address - base >= shad_sync_window[ShadSyncWindowRwlockLimit] - base)
        return 0;
    return (struct Rwlock*)address;
}
static inline struct RwHolds* RwHoldTable(void) {
    return (struct RwHolds*)(Self() - SHAD_SYNC_RW_HOLDS_BELOW_HANDLE);
}
static int RwSlow(unsigned import, u64* slot) {
    return ((SlowFn)shad_sync_imports[import])(slot);
}
static void RwWake(struct Rwlock* l) {
    ((WakeFn)shad_sync_imports[ShadSyncImportWake])((u64)&l->word, RW_WAKE_ALL);
}

static int RwRead(u64* slot, int try_only) {
    const unsigned slow = try_only ? ShadSyncImportRwlockTryrdlock : ShadSyncImportRwlockRdlock;
    struct Rwlock* l = RwObject(*slot);
    if (!l)
        return RwSlow(slow, slot);
    struct RwHolds* holds = RwHoldTable();
    if (holds->overflow)
        return RwSlow(slow, slot);
    struct RwHold* free_hold = 0;
    /* A short loop, not 15 unrolled copies: less guest code for FEX to translate. */
#pragma clang loop unroll(disable)
    for (u32 i = 0; i < SHAD_SYNC_RW_HOLD_ENTRIES; ++i) {
        struct RwHold* hold = &holds->entries[i];
        if (hold->lock == (u64)l) {
            /* Same order as the host: type 2 refuses, otherwise one more level. */
            if (l->type == SHAD_SYNC_RW_TYPE_RECURSIVE_REFUSED)
                return try_only ? EBUSY : EDEADLK;
            if (hold->depth >= 0xffffffffu)
                return EAGAIN;
            ++hold->depth;
            return 0;
        }
        if (!hold->lock && !free_hold)
            free_hold = hold;
    }
    if (!free_hold)
        return RwSlow(slow, slot);
    u32 w = __atomic_load_n(&l->word, __ATOMIC_RELAXED);
    for (;;) {
        if ((w & SHAD_SYNC_RW_WRITE_LOCKED) || (l->type && (w & SHAD_SYNC_RW_WRITE_WAITERS)) ||
            (w & SHAD_SYNC_RW_READERS) == SHAD_SYNC_RW_READERS)
            return RwSlow(slow, slot);
        if (__atomic_compare_exchange_n(&l->word, &w, w + 1, 0, __ATOMIC_ACQUIRE,
                                        __ATOMIC_RELAXED))
            break;
    }
    free_hold->depth = 1;
    free_hold->lock = (u64)l;
    return 0;
}

static int RwWrite(u64* slot, int try_only) {
    struct Rwlock* l = RwObject(*slot);
    if (l) {
        u32 w = __atomic_load_n(&l->word, __ATOMIC_RELAXED);
        if (!(w & (SHAD_SYNC_RW_WRITE_LOCKED | SHAD_SYNC_RW_READERS)) &&
            __atomic_compare_exchange_n(&l->word, &w, w | SHAD_SYNC_RW_WRITE_LOCKED, 0,
                                        __ATOMIC_ACQUIRE, __ATOMIC_RELAXED)) {
            __atomic_store_n(&l->writer, Self(), __ATOMIC_RELAXED);
            return 0;
        }
    }
    return RwSlow(try_only ? ShadSyncImportRwlockTrywrlock : ShadSyncImportRwlockWrlock, slot);
}

static int RwUnlock(u64* slot) {
    struct Rwlock* l = RwObject(*slot);
    if (!l)
        return RwSlow(ShadSyncImportRwlockUnlock, slot);
    /* Only this thread stores its own handle in `writer`, and clears it before
     * releasing, so equality means this thread holds the write lock. */
    if (__atomic_load_n(&l->writer, __ATOMIC_RELAXED) == Self()) {
        __atomic_store_n(&l->writer, 0, __ATOMIC_RELAXED);
        const u32 old = __atomic_fetch_and(&l->word, ~SHAD_SYNC_RW_WRITE_LOCKED, __ATOMIC_RELEASE);
        if (old & RW_WAITERS)
            RwWake(l);
        return 0;
    }
    struct RwHolds* holds = RwHoldTable();
    if (!holds->overflow) {
#pragma clang loop unroll(disable)
        for (u32 i = 0; i < SHAD_SYNC_RW_HOLD_ENTRIES; ++i) {
            struct RwHold* hold = &holds->entries[i];
            if (hold->lock != (u64)l)
                continue;
            if (--hold->depth)
                return 0;
            hold->lock = 0;
            const u32 old = __atomic_fetch_sub(&l->word, 1, __ATOMIC_RELEASE);
            if ((old & SHAD_SYNC_RW_READERS) == 1 && (old & RW_WAITERS))
                RwWake(l);
            return 0;
        }
    }
    return RwSlow(ShadSyncImportRwlockUnlock, slot);
}

EXPORT int shad_pthread_rwlock_rdlock(u64* slot) { return RwRead(slot, 0); }
EXPORT int shad_pthread_rwlock_tryrdlock(u64* slot) { return RwRead(slot, 1); }
EXPORT int shad_pthread_rwlock_wrlock(u64* slot) { return RwWrite(slot, 0); }
EXPORT int shad_pthread_rwlock_trywrlock(u64* slot) { return RwWrite(slot, 1); }
EXPORT int shad_pthread_rwlock_unlock(u64* slot) { return RwUnlock(slot); }
EXPORT u64 shad_scePthreadRwlockRdlock(u64* slot) {
    const int e = RwRead(slot, 0);
    return e ? SCE_ERROR(e) : 0;
}
EXPORT u64 shad_scePthreadRwlockTryrdlock(u64* slot) {
    const int e = RwRead(slot, 1);
    return e ? SCE_ERROR(e) : 0;
}
EXPORT u64 shad_scePthreadRwlockWrlock(u64* slot) {
    const int e = RwWrite(slot, 0);
    return e ? SCE_ERROR(e) : 0;
}
EXPORT u64 shad_scePthreadRwlockTrywrlock(u64* slot) {
    const int e = RwWrite(slot, 1);
    return e ? SCE_ERROR(e) : 0;
}
EXPORT u64 shad_scePthreadRwlockUnlock(u64* slot) {
    const int e = RwUnlock(slot);
    return e ? SCE_ERROR(e) : 0;
}
