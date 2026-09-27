# Kernel synchronization: one interface, separate desktop and Android implementations

Guest synchronization (Orbis kernel semaphores, POSIX `sem_t`, `pthread_rwlock_t`,
`pthread_cond_t`, event flags, the mutex word, event queues) is implemented twice on purpose:
once for the desktop kernel and once for the Android host runtime. Both implement the same
interface — the Orbis functions and their semantics below — but share no implementation code, so
tuning or fixing one side cannot change the other.

On 2026-09-27 the two were briefly merged into one shared core (`src/core/libraries/kernel/sync/`).
On Android that core froze Bloodborne (a lost condition-variable wakeup, see
[the validation record](validation/android-native-host/hle-sync-core-20260927.md) §10); the
Android side went back to its original implementation and the core became the desktop
implementation only.

## Why separate

The two platforms work on different principles:

| | Desktop | Android host runtime |
|---|---|---|
| Guest code | runs natively in the emulator process | runs under FEX |
| Guest pointers | dereferenced directly | validated and pinned through `GuestAddressSpace` (no pin survives a wait) |
| Mutex | host `LockWord` (futex / `WaitOnAddress`) behind the desktop pthread mutex | three-state word in guest memory, locked by an app-shipped guest fast path (`guest/runtime/sync/mutex.c`); the host only parks on the word |
| Cancellation | pthread cancellation points park on the thread's `wake_sema`; signals are APCs | session stop token on every wait |
| Wake policy | libthr-style deferred wakes (a signaller holding the waiter's mutex wakes it at unlock) | wake after releasing the object lock |

## The interface

| Primitive | Functions | Desktop | Android |
|---|---|---|---|
| Kernel semaphore | `sceKernelCreateSema/WaitSema/PollSema/SignalSema/CancelSema/DeleteSema` | `threads/semaphore.cpp` over `sync/kernel_semaphore.h`, `sync/object_table.h` | `host_runtime/guest_kernel_semaphore.h` |
| POSIX `sem_t` | `sem_init/destroy/wait/trywait/timedwait/post/getvalue` | `threads/semaphore.cpp` over `sync/counting_semaphore.h` | `host_runtime/guest_semaphore.h` |
| rwlock | `pthread_rwlock_*`, `scePthreadRwlock*` | `threads/rwlock.cpp` over `sync/rw_lock.h` | `host_runtime/guest_rwlock.h` |
| Condition variable | `pthread_cond_*`, `scePthreadCond*` | `threads/condvar.cpp` over `sync/condition_variable.h` | `host_runtime/guest_mutex.h` (`CondWait`/`CondNotify`) |
| Mutex | `pthread_mutex_*`, `scePthreadMutex*` | `threads/mutex.cpp` over `sync/mutex.h`, `sync/lock_word.h` | `host_runtime/guest_mutex.h` + `guest/runtime/sync/mutex.c` |
| Event flag | `sceKernelCreateEventFlag/Wait/Poll/Set/Clear/Cancel/Delete` | `threads/event_flag.cpp` over `sync/event_flags.h` | `host_runtime/guest_runtime.cpp` |
| Event queue wait | `sceKernelWaitEqueue` | `EqueueInternal::WaitForEvents` (`WaitReady` + `TakeTriggered`) | `host_runtime/guest_graphics_hle.cpp`, polls `GetTriggeredEvents` every 1 ms |

The two event flag implementations both drive the older state machine
`threads/event_flag_state.h`, and both platforms trigger the one `EqueueInternal` object per queue
(GNM, VideoOut); only the waits are platform code.

Semantics both implementations follow (checked by each side's own tests):

- Kernel semaphores: `need` in 1..max, a signal count in 1..(max - value), cancel's count at most
  max, else `EINVAL`; FIFO or priority queue; a signal completes every queued waiter whose need
  fits; delete wakes waiters with `EACCES`, cancel with `ECANCELED`; the timeout is updated only
  when the thread actually waited.
- `sem_t`: a post makes one token available and wakes a waiter; timed and stopped waits return
  `ETIMEDOUT`/`EINTR` without losing a posted token.
- rwlock: types 0 (readers enter while writers wait), 1 (waiting writers block new readers except
  existing readers), 2 (as 1, recursive read is `EDEADLK`); self-deadlock `EDEADLK`, unlock by a
  non-holder `EPERM`, destroy of a held or waited lock `EBUSY`.
- Condition variables: releasing the mutex and queueing are atomic with respect to notification
  (a thread that takes the mutex next and signals wakes the waiter); targeted signals; a waiter
  counts against the condition (destroy `EBUSY`) until it has its mutex back.
- Event flags: invalid attributes or wait modes are `EINVAL`; delete releases waiters.

## Desktop implementation (`src/core/libraries/kernel/sync/`)

Header-only algorithms with platform wait hooks (`sync/wait_slot.h`: `ParkerWait` for plain waits,
`threads/wait_platform.h` `CancellationPointWait` for cancellation points). A shared wait calls
`Slot()`, `Critical()` (defers asynchronous cancellation around object-lock sections),
`BeforePark()`, `Park()`, `AfterPark()`; condition variables add `Owner()`, `Context()`,
`ReleaseMutex()` and `ReacquireMutex()`. Per-object locks, wakes issued after the object lock is
released, a lock-free empty check in condition `Signal`/`Broadcast` and a lock-free `sem_t` count.
A `Parker` wake is consumed by the `Park` that returns `Woken`.

Tests: `tests/sync/sync_core_tests.cpp` — compile with clang-cl and `-I src` plus
`src/core/libraries/kernel/sync/win32_wait.cpp` (a runner is in the validation record).

## Android implementation (`src/core/host_runtime/guest_*.h`)

Each primitive family has one domain object per session. Kernel semaphores, `sem_t` and rwlocks
serialize on their domain mutex: kernel semaphore waiters park on their own lock and are notified
after the domain mutex is released; `sem_t` and rwlock waiters wait on a per-object condition
variable under the domain mutex. Conditions have a per-condition guard that the waiter holds
across unlock and enqueue and that the notifier always takes. Guest memory is never pinned across
a wait.

Device tests: `guest_kernel_semaphore_tests`, `guest_condition_tests`, `guest_native_mutex_tests`,
`guest_rwlock_tests`, `guest_rwlock_diagnostics_tests`, `host_library_smoke` (`sem_t`),
`host_event_flag_tests`, `guest_equeue_tests` (the `EqueueInternal` object).

## Rule

Change one side at a time. A performance or correctness change to the desktop core is not applied
to Android (or the reverse) unless it is ported deliberately and validated with that platform's
tests and a game run.
