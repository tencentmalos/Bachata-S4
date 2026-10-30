# App-shipped guest synchronization fast path

Freestanding x86-64 guest code that the Android host runtime publishes for
**every** title at import-resolution time, so uncontended Orbis/POSIX mutex
operations never leave the guest. This is runtime code, not a per-game patch
(`guest/games/...` recipes are unrelated).

## Prototype basis

- Algorithm: FreeBSD libthr `mutex_lock_common` as ported by desktop
  `src/core/libraries/kernel/threads/mutex.cpp` (self-ownership by type,
  bounded spin, then block). libthr blocks through the kernel umtx
  "wait/wake by address"; here the same two primitives are HLE calls.
- Lock word: the three-state futex protocol (0 free, 1 held, 2 held with
  possible waiters, with `UINT32_MAX` reserved for retired objects) validated on device by
  `tests/guest_cpu/compiled_guest` and used by Bionic's pthread_mutex.
- Host primitives: expected-value wait / wake-on-address, the shape of Wine
  `RtlWaitOnAddress` (`src/core/host_runtime/guest_sync_waiters.h`).

## Files

| File | Role |
|---|---|
| `mutex.c` | Guest payload: `pthread_mutex_lock/trylock/unlock`, `scePthreadMutexLock/Trylock/Unlock`, `pthread_self`/`scePthreadSelf`, rwlocks, thread-specific data and leaf calls. |
| `src/core/host_runtime/guest_sync_abi.h` | Shared layout: prefix offsets, states, types, window/import table slot order, symbol names. |
| `src/core/host_runtime/guest_mutex.h` | Host HLE on the same word protocol (slow paths, cond wait, destroy). |
| `src/core/host_runtime/guest_rwlock.h` | Host rwlock HLE on the rwlock word and the per-thread hold tables. |
| `scripts/android/build-guest-payload` | Compiles/links the payload with the NDK clang x86 backend, emits `guest_sync_payload.h` (image + symbol table). |
| `tests/host_runtime/guest_sync_fastpath_tests.cpp` | Real FEX execution of this payload against the production domain. |

## Protocol (guest and host identical)

```
lock:   CAS(state 0->1) ? owner=self,count=1 : (destroyed ? HLE EINVAL
        : owner==self ? recursive/EDEADLK/deadlock : spin `spins`
        : CAS live state -> 2, retry races, wait if previously held) ; owner=self,count=1
unlock: owner!=self ? EPERM : count>1 ? --count : count=0,owner=0, xchg(state,0)==2 ? wake(&state,1)
destroy: refuse held/waited-on objects; CAS 0->destroyed, retire identity, slot=2
```

`self` is `fs:[0x10]` (`Tcb::tcb_thread`, the runtime thread handle, also what
`pthread_self` returns). Slot values outside all arena windows (0/1 static
initializers and 2 destroyed always are) take the HLE fallback imports, which
initialize lazily or report EINVAL exactly as before. The window is not a
compile-time address: the host reserves 256 MiB (`GuestSyncObjects`,
`SHAD_SYNC_ARENA_WINDOW_SIZE`) next to its other service allocations, carves the
16 KiB arena blocks out of it, and writes `[base, limit)` into the payload's
`shad_sync_window` table before the image becomes executable. A payload that
hard-coded the window went inert when the service base moved from 64 GiB to
112 GiB (every mutex silently returned to HLE, ~2600 lock/unlock crossings per
frame in Bloodborne).

`GuestSyncWindows` grows beyond that first window as needed. Each extension has
an immutable `{next, base, limit}` record in its first 16 KiB block, outside the
object range. A separate RW directory has independent mutex/rwlock heads; the
host release-publishes a head before it can return an object in that window,
and the guest acquire-loads it after missing the initial-window test. RX code
and import tables remain unchanged. Smaller allocations are retried down to
32 KiB if a preferred 256 MiB extension cannot fit. Failure leaves the directory
unchanged and returns ENOMEM; the next allocation can retry normally.

There is no fixed window-count or lifetime-object quota. Addresses and range
nodes remain unique and mapped until session teardown, including retired
objects, so old handles cannot resolve to new locks. The destroyed word also
rejects copied handles that still point into a valid arena window. Both host
and guest contended acquisition use CAS instead of overwriting that tombstone.

## Loading

`GuestRuntime::Impl::InstallSyncFastPath` (guest_runtime.cpp) runs on the
first `Bind()`: it resolves the five import slots (wait, wake, POSIX
lock/trylock/unlock HLE veneers), copies the image into a fresh RX mapping
near the HLE veneers, and points the eight game-visible symbol names (both
`libScePosix` and `libkernel` spellings) at the payload exports.
`debug.shadps4.sync_fastpath=0` disables it; `hle_sync status` reports
`"fast_path"`.

## Rwlocks (2026-10-13)

`pthread_rwlock_{rd,tryrd,wr,trywr}lock/unlock` and the `scePthreadRwlock*`
spellings. UE4 read-locks its pipeline-state cache once per draw (KH3 render
thread: ~19k rdlock/unlock pairs a second, all through HLE before).

- Objects initially live in a second 256 MiB window (`GuestRwlockObjects`, window table
  slots 5/6) so neither fast path ever takes the other kind of object.
  Its extensions use the rwlock head in the same directory, separate from mutex
  windows. Attribute objects stay outside them.
- Prefix (`SHAD_SYNC_RW_*`): u32 word (bit 31 write-locked, bit 30 parked
  writers, bit 29 parked readers, bits 0..28 the number of (thread, lock) read
  holds), u32 type (0/1/2), u64 write owner (`fs:[0x10]`).
- Each thread keeps its read holds in a 256-byte table directly below its
  thread-specific records (`SHAD_SYNC_RW_HOLDS_BELOW_HANDLE`): 15 entries of
  {lock, depth} plus an overflow count. Recursion only changes the depth, so the
  word counts each holder once. The table is what keeps EDEADLK (a reader asking
  for the write lock, a type 2 recursive read), EPERM (unlock by a non-holder)
  and "a thread already reading may read again" under writer preference
  (types 1/2) exactly as the HLE had them. A 16th distinct hold is kept by the
  host, and the overflow count sends that thread's rwlock reads and unlocks to
  the HLE until it drops back to zero.
- Guest: read = CAS count+1 when no writer and (type 0 or no parked writer);
  write = CAS set bit 31 when no writer and no readers; unlock clears or
  decrements and calls the wake import (wake all) when waiter bits remain and
  the lock became free for them. Everything else (blocking, timeouts, errors,
  static initializers, destroyed handles) goes to the HLE fallback imports 5..9.
- Host (`guest_rwlock.h`): the same word under a per-lock mutex (FreeBSD umtx's
  busy lock) that owns the parked-waiter counts and bits; waiters park on the
  word through the mutex domain's `GuestAddressWaiters`, the instance the wake
  import reaches. Destroy refuses a held or waited-on lock and stores
  `SHAD_SYNC_RW_DESTROYED` in the word.
- `debug.shadps4.rwlock_fastpath=0` (or `SHADPS4_RWLOCK_FASTPATH=0`) keeps the
  rwlock entries on the HLE, which runs the same protocol; `rwlock_log` does the
  same so its trace sees every call.

## Limits

- Mutex timed lock, cond, sem_t and kernel semaphores remain HLE (cond
  wait/reacquire uses the same word); so do the timed rwlock locks.
- `hle_sync` no longer sees uncontended Mutex.Lock/Unlock or rwlock calls: their
  rows count slow paths only.
- Objects are never unmapped before Session teardown; retiring an arena
  mapping under parked waiters is unsupported.
