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
    ShadSyncWindowBase = 0,  /* u64: first arena object address */
    ShadSyncWindowLimit = 1, /* u64: one past the last arena object address */
    ShadSyncWindowCount = 2
};

/* Host-filled 8-byte slots in the payload's .shad_imports table, in order. */
enum ShadSyncImport {
    ShadSyncImportWait = 0,        /* u64 (*)(u64 address, u64 expected, u64 width) -> posix error */
    ShadSyncImportWake = 1,        /* u64 (*)(u64 address, u64 count) -> woken */
    ShadSyncImportMutexLock = 2,   /* int (*)(u64* slot): posix HLE fallback, initializes */
    ShadSyncImportMutexTrylock = 3,
    ShadSyncImportMutexUnlock = 4,
    ShadSyncImportCount = 5
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

/* Synthetic NIDs for the two runtime primitives; never produced by a game ELF. */
#define SHAD_SYNC_NID_WAIT "shadSyncWait"
#define SHAD_SYNC_NID_WAKE "shadSyncWake"
#define SHAD_SYNC_LIBRARY_SUFFIX "#shadSync#1#shadSync#Function"

#endif
