/* SPDX-License-Identifier: GPL-2.0-or-later */
#ifndef SHADPS4_GUEST_SYNC_ABI_H
#define SHADPS4_GUEST_SYNC_ABI_H
/* Shared wire layout between the host runtime (ARM64 C++) and the app-shipped
 * guest synchronization payload (freestanding x86-64 C). No host types, no STL.
 *
 * Prototype basis: FreeBSD libthr pthread_mutex (owner/count/spin/yield fields,
 * kernel umtx wait/wake by address) as ported by src/core/libraries/kernel/
 * threads/mutex.cpp, and the three-state futex word already validated on
 * device by tests/guest_cpu/compiled_guest (0 free, 1 held, 2 held with
 * possible waiters), which is also Bionic's uncontended/contended split. */

/* Guest-visible prefix of every arena mutex object (GuestSyncArena, 0x100
 * bytes). libc only touches flags at +0x20; everything else belongs to the
 * runtime protocol below and is shared by the guest fast path and host HLE. */
#define SHAD_SYNC_MUTEX_OWNER 0x00    /* u64: owning thread handle (fs:[0x10]) or 0 */
#define SHAD_SYNC_MUTEX_COUNT 0x08    /* u32: recursion depth while owned */
#define SHAD_SYNC_MUTEX_SPINS 0x0c    /* u32: bounded spin iterations before parking */
#define SHAD_SYNC_MUTEX_YIELDS 0x10   /* u32: reserved (libthr m_yieldloops) */
#define SHAD_SYNC_MUTEX_PROTOCOL 0x14 /* u32: 0 none, 1 inherit, 2 protect */
#define SHAD_SYNC_MUTEX_STATE 0x18    /* u32: 0 free, 1 held, 2 held + possible waiters */
#define SHAD_SYNC_MUTEX_FLAGS 0x20    /* u32: type in bits 0..7 (libc may set 0x100/0x200) */
#define SHAD_SYNC_MUTEX_PREFIX_SIZE 0x28 /* padded struct size; flags end at 0x24 */

#define SHAD_SYNC_STATE_FREE 0u
#define SHAD_SYNC_STATE_HELD 1u
#define SHAD_SYNC_STATE_CONTENDED 2u

/* Mutex types written by the attribute HLE (desktop PthreadMutexType). */
#define SHAD_SYNC_TYPE_MASK 0xffu
#define SHAD_SYNC_TYPE_ERRORCHECK 1u
#define SHAD_SYNC_TYPE_RECURSIVE 2u
#define SHAD_SYNC_TYPE_NORMAL 3u
#define SHAD_SYNC_TYPE_ADAPTIVE 4u

/* Slot values 0/1 are static initializers and 2 is destroyed; real objects live
 * inside the arena window [base, limit) that the host reserves and writes into
 * the payload's `shad_sync_window` table at publication. The window is NOT a
 * compile-time address: the service allocation base already moved once
 * (64 GiB -> 112 GiB) and silently sent every mutex back to HLE. base == limit
 * == 0 means no window: every object takes the HLE path. */
#define SHAD_SYNC_ARENA_WINDOW_SIZE 0x10000000ull /* 256 MiB reserved by the host */
enum ShadSyncWindow {
    ShadSyncWindowBase = 0,    /* u64: first arena object address */
    ShadSyncWindowLimit = 1,   /* u64: one past the last arena object address */
    ShadSyncWindowTlsKeys = 2, /* u64: address of the thread-specific key table */
    /* u64: guest TSC value at process-counter zero. Guest rdtsc (FEX SMALLTSCSCALE) and the
     * host GuestClock both read the host counter shifted left by the same scale, so
     * rdtsc - origin equals the HLE sceKernelGetProcessTimeCounter bit for bit. */
    ShadSyncWindowClockOrigin = 3,
    ShadSyncWindowTscFrequency = 4, /* u64: scaled TSC frequency the HLE reports */
    /* u64 pair: the rwlock objects' own window [base, limit); zero sends every rwlock to HLE. */
    ShadSyncWindowRwlockBase = 5,
    ShadSyncWindowRwlockLimit = 6,
    ShadSyncWindowCount = 7
};

/* Rwlock object prefix (rwlock arena objects, 0x100 bytes, in their own window so a
 * mutex fast path never treats an rwlock as a mutex or vice versa). Shared by the
 * guest fast path and the host HLE (guest_rwlock.h). */
#define SHAD_SYNC_RW_WORD 0x00   /* u32: state word, bits below */
#define SHAD_SYNC_RW_TYPE 0x04   /* u32: Orbis rwlock type 0/1/2, immutable after init */
#define SHAD_SYNC_RW_WRITER 0x08 /* u64: write owner handle (fs:[0x10]) while write-locked */
#define SHAD_SYNC_RW_PREFIX_SIZE 0x10
#define SHAD_SYNC_RW_WRITE_LOCKED 0x80000000u
#define SHAD_SYNC_RW_WRITE_WAITERS 0x40000000u /* writers parked in the host */
#define SHAD_SYNC_RW_READ_WAITERS 0x20000000u  /* readers parked in the host */
#define SHAD_SYNC_RW_READERS 0x1fffffffu       /* distinct (thread, lock) read holds */
#define SHAD_SYNC_RW_DESTROYED 0xffffffffu     /* destroyed: every fast path falls back */
/* Types: 0 readers enter whenever no writer holds the lock; 1 parked writers keep new
 * readers out, a thread already reading may read again; 2 as 1, and a recursive read
 * is refused (EDEADLK). */
#define SHAD_SYNC_RW_TYPE_RECURSIVE_REFUSED 2u

/* Thread-specific data (pthread_key_*), shared by the guest fast path and the
 * host HLE. UE4 titles read a key on every allocation, hundreds of thousands of
 * times a second, so the read must not leave the guest.
 *
 * Key table (host-owned page, address in shad_sync_window[ShadSyncWindowTlsKeys]):
 * one u64 per key, (sequence << 1) | allocated, written by the host under its
 * key lock. A value is current only while its stored sequence equals the key's.
 *
 * Per-thread values: SHAD_SYNC_TLS_KEYS records of { u64 sequence, u64 value },
 * ending exactly at the thread handle (fs:[0x10]), so record i starts at
 * handle - SHAD_SYNC_TLS_VALUES_BELOW_HANDLE + 16 * i. Only the owning thread
 * writes its records (set, and the destructor pass at exit). */
#define SHAD_SYNC_TLS_KEYS 256u
#define SHAD_SYNC_TLS_VALUES_BELOW_HANDLE 0x1000u /* SHAD_SYNC_TLS_KEYS * 16 */

/* Per-thread rwlock read holds, directly below the thread-specific records:
 *   u64 overflow   holds of this thread the host keeps outside the table (nonzero
 *                  sends every rwlock read/unlock of the thread to the HLE)
 *   u64 reserved
 *   SHAD_SYNC_RW_HOLD_ENTRIES x { u64 lock (object address, 0 = free), u64 depth }
 * The shared word counts each (thread, lock) once; recursion lives here. Only the
 * owning thread reads or writes its table (guest fast path and host HLE alike). */
#define SHAD_SYNC_RW_HOLD_ENTRIES 15u
#define SHAD_SYNC_RW_HOLDS_SIZE 0x100u
#define SHAD_SYNC_RW_HOLDS_BELOW_HANDLE (SHAD_SYNC_TLS_VALUES_BELOW_HANDLE + SHAD_SYNC_RW_HOLDS_SIZE)

/* Thread handle object (64 bytes at fs:[0x10]). */
#define SHAD_SYNC_HANDLE_ERRNO 0x08     /* s32: guest errno */
#define SHAD_SYNC_HANDLE_THREAD_ID 0x10 /* u64: id returned by scePthreadGetthreadid */

/* Host-filled 8-byte slots in the payload's .shad_imports table, in order. */
enum ShadSyncImport {
    ShadSyncImportWait = 0,        /* u64 (*)(u64 address, u64 expected, u64 width) -> posix error */
    ShadSyncImportWake = 1,        /* u64 (*)(u64 address, u64 count) -> woken */
    ShadSyncImportMutexLock = 2,   /* int (*)(u64* slot): posix HLE fallback, initializes */
    ShadSyncImportMutexTrylock = 3,
    ShadSyncImportMutexUnlock = 4,
    /* int (*)(u64* slot): POSIX rwlock HLE fallbacks (initialize, block, errors). */
    ShadSyncImportRwlockRdlock = 5,
    ShadSyncImportRwlockTryrdlock = 6,
    ShadSyncImportRwlockWrlock = 7,
    ShadSyncImportRwlockTrywrlock = 8,
    ShadSyncImportRwlockUnlock = 9,
    ShadSyncImportCount = 10
};

/* Symbol names the host resolves in the payload image. */
#define SHAD_SYNC_IMPORT_TABLE "shad_sync_imports"
#define SHAD_SYNC_WINDOW_TABLE "shad_sync_window"
#define SHAD_SYNC_EXPORT_POSIX_LOCK "shad_pthread_mutex_lock"
#define SHAD_SYNC_EXPORT_POSIX_TRYLOCK "shad_pthread_mutex_trylock"
#define SHAD_SYNC_EXPORT_POSIX_UNLOCK "shad_pthread_mutex_unlock"
#define SHAD_SYNC_EXPORT_SCE_LOCK "shad_scePthreadMutexLock"
#define SHAD_SYNC_EXPORT_SCE_TRYLOCK "shad_scePthreadMutexTrylock"
#define SHAD_SYNC_EXPORT_SCE_UNLOCK "shad_scePthreadMutexUnlock"
#define SHAD_SYNC_EXPORT_SELF "shad_pthread_self"
#define SHAD_SYNC_EXPORT_GETSPECIFIC "shad_pthread_getspecific"
#define SHAD_SYNC_EXPORT_GETTHREADID "shad_pthread_getthreadid"
/* Leaf calls (debug.shadps4.leaf_fastpath): no blocking, no host state beyond the
 * tables above. */
#define SHAD_SYNC_EXPORT_SETSPECIFIC "shad_pthread_setspecific"
#define SHAD_SYNC_EXPORT_EQUAL "shad_pthread_equal"
#define SHAD_SYNC_EXPORT_PROCESS_TIME "shad_sceKernelGetProcessTimeCounter"
#define SHAD_SYNC_EXPORT_READ_TSC "shad_sceKernelReadTsc"
#define SHAD_SYNC_EXPORT_TSC_FREQUENCY "shad_sceKernelGetTscFrequency"
/* Rwlocks (debug.shadps4.rwlock_fastpath). */
#define SHAD_SYNC_EXPORT_RW_RDLOCK "shad_pthread_rwlock_rdlock"
#define SHAD_SYNC_EXPORT_RW_TRYRDLOCK "shad_pthread_rwlock_tryrdlock"
#define SHAD_SYNC_EXPORT_RW_WRLOCK "shad_pthread_rwlock_wrlock"
#define SHAD_SYNC_EXPORT_RW_TRYWRLOCK "shad_pthread_rwlock_trywrlock"
#define SHAD_SYNC_EXPORT_RW_UNLOCK "shad_pthread_rwlock_unlock"
#define SHAD_SYNC_EXPORT_SCE_RW_RDLOCK "shad_scePthreadRwlockRdlock"
#define SHAD_SYNC_EXPORT_SCE_RW_TRYRDLOCK "shad_scePthreadRwlockTryrdlock"
#define SHAD_SYNC_EXPORT_SCE_RW_WRLOCK "shad_scePthreadRwlockWrlock"
#define SHAD_SYNC_EXPORT_SCE_RW_TRYWRLOCK "shad_scePthreadRwlockTrywrlock"
#define SHAD_SYNC_EXPORT_SCE_RW_UNLOCK "shad_scePthreadRwlockUnlock"

/* Synthetic NIDs for the two runtime primitives; never produced by a game ELF. */
#define SHAD_SYNC_NID_WAIT "shadSyncWait"
#define SHAD_SYNC_NID_WAKE "shadSyncWake"
#define SHAD_SYNC_LIBRARY_SUFFIX "#shadSync#1#shadSync#Function"

#endif
