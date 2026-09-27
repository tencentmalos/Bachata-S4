# Shared kernel synchronization core

Guest synchronization (Orbis kernel semaphores, POSIX `sem_t`, `pthread_rwlock_t`,
`pthread_cond_t`, event flags and the mutex word) used to be implemented twice: once in the
desktop kernel (`src/core/libraries/kernel/threads/`) and once in the Android host runtime
(`src/core/host_runtime/guest_*.h`). The two copies drifted apart: fixes, performance work and
bugs landed on one side only. Since 2026-09-27 the algorithms live once, in
`src/core/libraries/kernel/sync/`, and both platforms are thin layers over them.

## Layers

| Layer | Where | Owns |
|---|---|---|
| Shared core | `src/core/libraries/kernel/sync/*.h` (header-only, no `<windows.h>`) | Algorithms and semantics: token accounting, waiter queues and order, which waiter a wake selects, wake-after-unlock, timeout/stop races, error codes |
| Platform wait hooks | `sync/wait_slot.h` (`ParkerWait`), desktop `threads/wait_platform.h` (`CancellationPointWait`), Android `GuestMutexDomain::CondPlatform` | How a waiting thread sleeps and is woken, cancellation, what must be held around object locks |
| Handle binding | desktop `threads/*.cpp`, Android `host_runtime/guest_*.h` | Guest ABI: handle → object lookup, guest pointers (direct on desktop, pinned on Android), argument decoding |

| Primitive | Shared core | Desktop binding | Android binding |
|---|---|---|---|
| Kernel semaphore | `kernel_semaphore.h` + `object_table.h` | `threads/semaphore.cpp` | `guest_kernel_semaphore.h` |
| POSIX `sem_t` | `counting_semaphore.h` | `threads/semaphore.cpp` (cancellation point) | `guest_semaphore.h` |
| rwlock | `rw_lock.h` | `threads/rwlock.cpp` | `guest_rwlock.h` |
| Condition variable | `condition_variable.h` | `threads/condvar.cpp` (cancellation point, deferred wake) | `guest_mutex.h` (`CondPlatform`) |
| Event flag | `event_flags.h` (table + attribute/mode rules) over `threads/event_flag_state.h` | `threads/event_flag.cpp` | `guest_runtime.cpp` |
| Mutex word | `lock_word.h` (`LockWordState`, `LockWord`) | `sync/mutex.h` (`TimedMutex = LockWord`) | guest prefix word in `guest_mutex.h` (static_asserts the same states) and `guest/runtime/sync/mutex.c` |

## Wait hooks

A shared `Wait(platform)` calls:

- `Slot()` — the `WaitSlot` queued for this wait. Wakers call `Unpark()` on it after releasing
  the object lock, through shared ownership, so a waiter that already timed out is safe.
- `Critical()` — RAII held around every section under an object lock. Desktop defers
  asynchronous pthread cancellation there (`ScopedPthreadCritical`).
- `BeforePark()` — under the lock, once queued; false ends the wait as interrupted (desktop: a
  pending cancellation at a cancellation point; Android: the session stop token).
- `Park()` — sleep until unparked, a deadline or an interruption.
- `AfterPark()`.
- Condition variables add `Owner()`, `Context()`, `ReleaseMutex()` (under the condition lock,
  atomic with the enqueue) and `ReacquireMutex()`.

`ParkerWait` (mutex + condition variable + optional `std::stop_token`, slot allocated only when
the wait actually queues) serves Android and every desktop wait that is not a cancellation
point. Desktop cancellation points (`sem_wait`, `pthread_cond_wait`) park on the thread's
`wake_sema`, which `pthread_cancel` also releases.

## Semantics chosen

Where the two copies disagreed, the core follows the Android behaviour that the device suites
validate:

- Kernel semaphores: `need` must be in 1..max, a signal count in 1..(max - value), cancel's
  count at most max, else `EINVAL`; delete wakes waiters with `EACCES` and the ids are a locked
  table with shared ownership (the desktop `SlotVector` was unlocked and deleted objects under
  waiters).
- `sem_t`: a post wakes exactly one queued waiter; a selected waiter that times out or is
  stopped without taking its token passes the wake on.
- rwlock: types 0 (readers enter while writers wait), 1 (waiting writers block new readers
  except existing readers), 2 (as 1, recursive read is `EDEADLK`); owners are tracked, so
  self-deadlock is `EDEADLK` and an unlock by a non-holder `EPERM`; destroy of a held or waited
  lock is `EBUSY`. The desktop rwlock used to ignore the type, hang on a recursive write lock and
  delete busy locks.
- Condition variables: FIFO waiters, targeted signals, a waiter counts against the condition
  (destroy `EBUSY`) until it has reacquired its mutex. Desktop keeps libthr's deferred wake
  (a signaller holding the waiter's mutex wakes it at unlock) through the `defer` hook.
- Event flags: invalid attributes or wait modes are `EINVAL` (the desktop used to hit
  `UNREACHABLE`); delete releases waiters before the object goes away.

## Tests

- `tests/sync/sync_core_tests.cpp` — platform-neutral tests of every core type (races between
  signals and stops, token/wake ownership, stress). Android: host probe target
  `sync_core_tests`; desktop: compile the file with clang-cl and `-I src` (plus
  `src/core/libraries/kernel/sync/win32_wait.cpp` on Windows).
- Android device suites keep covering the guest bindings: `guest_kernel_semaphore_tests`,
  `guest_condition_tests`, `guest_native_mutex_tests`, `guest_rwlock_tests`,
  `guest_rwlock_diagnostics_tests`, `host_library_smoke` (sem_t) and `host_event_flag_tests`
  (the event flag state machine, also runs on desktop).

## Not shared (on purpose)

- Android mutex acquisition (`GuestMutexDomain::AcquireWord`) wins the guest word, writes the
  owner and confirms retirement inside one pin; routing it through a generic word interface
  would add a pin per lock. It shares the word states and protocol only.
- Desktop mutex types, protocols and spin counts stay in `threads/mutex.cpp`; Android keeps its
  lock-free mutex directory and retirement.
- Kernel event queues keep their own class, `EqueueInternal` (`kernel/equeue.h`), but its wait is
  shared now: `WaitReady(deadline, stop_token)` waits without consuming (triggered events and
  small HR timers, which sleep until 200 µs before expiry and then yield) and `TakeTriggered`
  consumes under the queue lock. The desktop `WaitForEvents` and the Android
  `sceKernelWaitEqueue` HLE (which pins its output between the two) both use them; `Close` wakes
  waiters when a queue is deleted. Probe: `guest_equeue_tests` (Android host).
