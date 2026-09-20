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
  possible waiters) already validated on device by
  `tests/guest_cpu/compiled_guest` and used by Bionic's pthread_mutex.
- Host primitives: expected-value wait / wake-on-address, the shape of Wine
  `RtlWaitOnAddress` (`src/core/host_runtime/guest_sync_waiters.h`).

## Files

| File | Role |
|---|---|
| `mutex.c` | Guest payload: `pthread_mutex_lock/trylock/unlock`, `scePthreadMutexLock/Trylock/Unlock`, `pthread_self`/`scePthreadSelf`. |
| `src/core/host_runtime/guest_sync_abi.h` | Shared layout: prefix offsets, states, types, arena window, import slot order, symbol names. |
| `src/core/host_runtime/guest_mutex.h` | Host HLE on the same word protocol (slow paths, cond wait, destroy). |
| `scripts/android/build-guest-payload` | Compiles/links the payload with the NDK clang x86 backend, emits `guest_sync_payload.h` (image + symbol table). |
| `tests/host_runtime/guest_sync_fastpath_tests.cpp` | Real FEX execution of this payload against the production domain. |

## Protocol (guest and host identical)

```
lock:   CAS(state 0->1) ? owner=self,count=1 : (owner==self ? recursive/EDEADLK/deadlock
        : spin `spins` : while (xchg(state,2) != 0) wait(&state, 2)) ; owner=self,count=1
unlock: owner!=self ? EPERM : count>1 ? --count : count=0,owner=0, xchg(state,0)==2 ? wake(&state,1)
```

`self` is `fs:[0x10]` (`Tcb::tcb_thread`, the runtime thread handle, also what
`pthread_self` returns). Slot values below the arena window (0/1 static
initializers, 2 destroyed) or outside it take the HLE fallback imports, which
initialize lazily or report EINVAL exactly as before.

## Loading

`GuestRuntime::Impl::InstallSyncFastPath` (guest_runtime.cpp) runs on the
first `Bind()`: it resolves the five import slots (wait, wake, POSIX
lock/trylock/unlock HLE veneers), copies the image into a fresh RX mapping
near the HLE veneers, and points the eight game-visible symbol names (both
`libScePosix` and `libkernel` spellings) at the payload exports.
`debug.shadps4.sync_fastpath=0` disables it; `hle_sync status` reports
`"fast_path"`.

## Limits

- Only mutex lock/trylock/unlock/self. Timed lock, cond, rwlock, sem_t and
  kernel semaphores remain HLE (cond wait/reacquire uses the same word).
- `hle_sync` no longer sees uncontended Mutex.Lock/Unlock: their rows count
  slow paths only.
- Objects are never unmapped before Session teardown; retiring an arena
  mapping under parked waiters is unsupported.
