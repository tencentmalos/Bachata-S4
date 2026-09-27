# MHW desktop stalls, blocking-wait primitives and the desktop sync core (2026-09-27)

Branch `feature/malos/mhw_fix` (base `malos/main` `c7b02fc9`), uncommitted. Desktop: Windows,
clang-cl RelWithDebInfo, RX 7600M XT. Android: AYANEO Pocket DS `01108YHE01017563`, host probes
from `build/android-host-api33/native`. Architecture: [sync-core.md](../../sync-core.md).

## 1. Root cause of the 2–4 FPS title/menu/particle scene

Profiles (desktop DebugBus `profiler_capture file`, aggregated with the SDK reader
`trace_dump --json` because the instrumented traces exceeded Litep's 2 M event limit):

- Particle scene, 12 s: GPU command thread `Bind.ObtainBuffer` self 7.16 s (p99 4.8 ms,
  max 254 ms). Title, 7.7 s: `Obtain.StreamCopy` 6.56 s — the small constant copies into the
  stream buffer (`MemoryManager::CopySparseMemory`, shared lock).
- Menu, 5.7 s: `Memory.LockWait` on the GPU thread 4.16 s (2205 waits, median 2.7 ms) while
  every instrumented writer (`Memory.WriteHold.*`) held the lock only ~230 ms in total; typical
  waits overlapped no writer at all.
- The hidden writer: `MemoryManager::Allocate` (`sceKernelAllocateDirectMemory`, ~1000 calls/s
  of 16 KiB by the game's loader/streaming threads) took `scoped_lock{mutex, unmap_mutex}` and
  walked `dmem_map` first-fit from `search_start` = 0 over thousands of mapped areas, ~3 ms per
  call under the writer lock. It was the only exclusive path not wrapped in a hold scope.

Fix (`src/core/memory.cpp/.h`):

- `dmem_free_hint`: no free direct-memory area starts below it. `Allocate` and `PoolExpand`
  search from `max(search_start, hint)` (same lowest fitting result) and raise it to the first
  free area they saw; `Free` lowers it; reset at init.
- The four dual locks (`PoolExpand`, `Allocate`, `SetDirectMemoryType`, `NameVirtualRange`) take
  `unmap_mutex` then `mutex` like every other writer, instead of `std::lock` back-off against
  readers.
- Every exclusive section is wrapped in `WriteHold` (`Memory.WriteHold.<op>` scope over exactly
  the hold); `Common::SharedFirstMutex` became one atomic word (writer bit + reader count, waiter
  count, sleeps via `std::atomic::wait`) and records `Memory.LockWait` /
  `Rasterizer.MappedRangesWait` only when a thread really waits.

| | before | after |
|---|---|---|
| Title FPS | 3 | 29 (32–39 after §3) |
| Menu FPS | 2 | 31 (42 after §3) |
| Particle scene FPS | ~4 | 20 |
| GPU thread lock wait | 4.16 s / 5.7 s (menu) | 0.45 s / 6 s (title), 37 ms / 10 s (particle) |

Remaining writer: `Free` (unmap syscalls under the writer lock, ~0.44 ms per call, 112–472 ms
per profile). The particle scene is now bound by the host GPU (`Present.AcquireImage` 8.0 s and
`Present.FrameFenceWait` 5.4 s per 10 s); the broken full-screen particle triangles are the
likely cost (§5).

## 2. Blocking and spinning primitives (desktop, part shared with Android)

- Region/page tracking locks (`region_manager.h`, `page_manager.cpp`) were pure `SpinLock`s on
  Windows, held across memcpy and `VirtualProtect`: now `Common::FutexMutex`
  (`std::atomic::wait` → WaitOnAddress on Windows, raw futex on Linux; Android's PageManager
  also switches from SpinLock).
- `TimedMutex` (every guest `pthread_mutex`) was a Win32 kernel mutex: each lock/unlock a
  system call, the adaptive spin up to 2000 `WaitForSingleObjectEx` calls. Now the shared
  three-state `Sync::LockWord` (futex / WaitOnAddress; timed locks supported).
- Win32 `Semaphore<max>` (thread wake semaphores, formerly one kernel object per blocking kernel
  semaphore wait): user-space count, kernel semaphore created lazily and only used to hand a
  release to a sleeping thread; waits stay alertable.
- `AccurateSleep` (usleep/nanosleep): one high-resolution waitable timer per thread instead of
  create/set/wait/close per call (a nested timer for APC re-entry).
- Liverpool: when every queue with work only yields on `WaitRegMem`/`MemSemaphore`/`Rewind`, it
  pauses, then yields, then waits on `submit_cv` (≤ 500 µs) instead of resuming them in a tight
  loop; submission/command notifications became `notify_all` because `WaitGpuIdle` callers share
  the condition.
- ObtainBuffer instrumentation added for the diagnosis was trimmed back to low-rate scopes
  (`Memory.LockWait`, `Memory.WriteHold.*`, `Bind.*`).

## 3. Shared sync core (desktop and Android)

Superseded on Android (§10): the Android host runtime went back to its original implementation;
the core below is the desktop implementation only.

See [sync-core.md](../../sync-core.md). New shared headers: `parker.h`, `wait_slot.h`,
`object_table.h`, `kernel_semaphore.h`, `counting_semaphore.h`, `rw_lock.h`,
`condition_variable.h`, `lock_word.h`, `event_flags.h`; desktop wait hooks
`threads/wait_platform.h`. The desktop `sleepq` (512 hashed spin locks, only used by condvar) is
removed.

Behaviour changes on desktop (now matching the Android-validated semantics): kernel semaphore
argument checks and delete safety, one wake per `sem_post`, rwlock types/EDEADLK/EPERM/EBUSY,
condition destroy `EBUSY` while waiters are in flight, event flag `EINVAL` instead of
`UNREACHABLE`, event flag and kernel semaphore handles as ids with shared ownership. Android
gains per-object locks (kernel semaphores, sem_t, rwlocks were one domain lock per family; sem_t
waited on the domain lock itself) and selective wakes (rwlock unlock was `notify_all`).

### Results

| Suite | Desktop | AYANEO |
|---|---|---|
| `sync_core_tests` (new) | 20758/0 | 20758/0 |
| `guest_kernel_semaphore_tests` | — | 588/0 (unrelated switches 1–2 of 32 signals) |
| `guest_condition_tests` / observer variant | — | 67/0 / 92/0 |
| `guest_native_mutex_tests` | — | 65499/0 |
| `guest_rwlock_tests` / diagnostics | — | 73/0 / 1233/0 (6000 contention iterations) |
| `host_library_smoke` (includes sem_t) | — | 511/0 |
| `guest_event_flag_tests` (`host_event_flag_tests` on Android) | 1205/0 | 1205/0 |

These runs exited 134 after printing their result: a static destructor
(`SaveDialogUi::~SaveDialogUi` → `ImGui::Layer::RemoveLayer`) locked the layer registry mutex of
`imgui_core.cpp` after that translation unit's statics were destroyed (FORTIFY abort,
pre-existing). Fixed: the layer registry is created on first use and never destroyed, and the
invitation dialog's status/state are declared before its UI (which points at them). Afterwards
all probes exit 0 with the same results (kernel semaphore 588/0, mutex 65499/0, condition 67/0,
rwlock 73/0, diagnostics 1233/0, `host_library_smoke` 511/0), also with stdout redirected to a
file.

Desktop MHW with the full set: startup, dialogs, title (32–39 FPS), menu (42 FPS), save load
and the opening particle scene (14 FPS at that view) without hangs. Seconds later the process
ended with a GPU TDR (Windows LiveKernelEvent 0x141, amdkmdag) — the known MHW TDR in the
particle chain (`cs 0xcefc8276`, see the MHW wave-reduction record), not a sync failure.

## 4. Earlier in this session (same uncommitted set)

MHW rendering fixes found with RDC tags and the GCN disassembler: GDS buffers are registered as
written shader resources (indirect args copied out of GDS were read before the counting
dispatch finished → zero args, dark title; fixed); GDS `ds_*` addresses add the M0 base
(shader binary version 25); raw dword copy kernels between two same-layout images become image
copies (8 per frame, `upload_diag raw_copy`); raw buffer reads of GPU-written images tile the
image back into the arena once per content version, skipping images whose memory was overwritten
by buffer writes (`upload_diag raw_sync`).

## 5. MHW particle triangles and TDR: GPU results lost to page-granular CPU uploads

Root cause (desktop, RX 7600M XT, readbacks off):

- The particle chain on the async compute queue sorts particles and `cs 0x9656f1f1` writes a
  per-bucket start/end table (`0x22708aa0e0+0x4000`); `cs 0xcefc8276` loops `start..end` and
  `cs 0x54b2ebc6` expands particles into vertex records and the draw's index buffer.
- Every frame the game CPU writes constants right beside that table (`0x22708aa000`,
  `0x22708ae140`, same 4 KiB tracking pages; also `0x22708ea000` for a second table). On PS4 the
  bytes are independent. Here a CPU write marked the whole page CPU modified and the next GPU use
  uploaded the page from guest memory, which never holds GPU results with readbacks off: the
  table was replaced by stale bytes. `end < start` wraps the `cefc8276` loop to ~4e9 iterations
  (the TDR) and the expansion reads wrong segments (quads whose corners come from different
  particles or garbage, the full-screen triangles and most of the GPU load).
- Evidence: the new `gpu_memory status` field `recent:` (last 16 lost-GPU-data addresses) cycled
  over those six addresses at ~28/s in the scene; with `readbacks_mode=1` (iGPU, the dGPU was lost
  after a TDR) the picture was correct but the CPU waited for GPU downloads (`Buffer.CpuReadback`
  2.7 s and `Buffer.WaitCpuReadback` 2.0 s per 6 s).

Fix, `GpuByteKeeper` (`src/video_core/buffer_cache/gpu_byte_keeper.h`, used by `MemoryTracker`
and `BufferCache::SynchronizeMemory`): byte ranges written by GPU bindings are recorded. A CPU
write fault inside them takes the page over as before; a fault beside them snapshots the page
(fixed pool of 64 slots, no allocation on the fault path) and the upload sends the bytes outside
the GPU ranges plus GPU bytes that differ from the snapshot (compared per dword). Counters in
`gpu_memory status` (`gpu_byte_keeper ...`), runtime switch `upload_diag keep_gpu on|off`.
Result on the dGPU: no wrong triangles, no TDR in three runs through the prologue; 13805 pages
kept, 896 taken over, 0 GPU bytes rewritten by the CPU after a fault; lost-GPU-data events fell
from ~2000 to 74 in the same phase (the rest are CPU rewrites of whole pages at load).

## 6. `MemoryManager::Free`

- Host view removals of direct mappings run after the writer lock (still under `unmap_mutex`):
  writer hold ~0.44 ms → ~6 µs per call.
- The walk over every VMA to find mappings of the freed block (3.3 s of the game main thread per
  8 s while entering the menu) is replaced by per-dmem-area map counts and the VA of a single
  mapping (`PhysicalMemoryArea::map_count` / `va_delta`); aliased or unknown areas fall back to
  the walk. DebugBus `vm_free_index status | verify on|off` (verify walks too and compares).
  MHW load: 94688 lookups, 0 walks; verify on for ~3300 frees: 0 mismatches. The walk also
  unmapped too much when a mapping extended past the freed range; both paths now use the exact
  intersection.

## 7. Event queues

Android part superseded (§10): the Android HLE polls `GetTriggeredEvents` every 1 ms again; the
wait below is the desktop `WaitForEvents`.

`EqueueInternal::WaitReady` (no consumption, stop token, deadline) and `TakeTriggered` are shared
by the desktop `WaitForEvents` and the Android `sceKernelWaitEqueue` HLE. Small HR timers
(< 1.2 ms) sleep on the condition variable until 200 µs before expiry, then yield; triggered
events are no longer ignored while a small timer is pending (desktop), Android no longer polls
every 1 ms nor reads the event list unlocked, and deleting a queue wakes its waiters. Probe
`guest_equeue_tests` (Android host).

## 8. GPU command thread: streamed buffers in host memory on discrete GPUs

After the particle fix the prologue ship cabin ran 15 FPS, bound by the GPU command thread
(~2700–3300 draws per frame, ~19 µs per draw). Per draw: vertex/index binding 6.4 µs, shader
buffer binding 5.3 µs, reset/barrier tracking 1.7 µs, textures 1.6 µs, pipeline lookup 1.6 µs
(temporary per-phase scopes, removed again). Most binding time was the copy of small read-only
buffers (≤16 KiB) into the stream buffer: title screen ~240k copies/s, average ~1.1 KiB, 59 %
≤256 bytes, ~2 µs each.

- Counters per copy step: map 32 ns, commit 51 ns, the copy 2.1 µs — almost all of it.
- A cached mapping lookup (`GuestReadCache`, `upload_diag read_cache`) saved ~5 %: not the lookup.
- Non-temporal AVX2 stores through a cached scratch made the copy itself 0.4 µs, but the time
  moved to the caller (`VI.Obtain` self 0.3 → 8.5 µs): the CPU writes into device memory through
  the PCIe BAR, and on this laptop (RX 7600M XT, hybrid graphics) that runs at only a few hundred
  MB/s. The experiment was removed.
- A second stream buffer in host memory (`MemoryType::HostUncached`), same session A/B with
  `upload_diag stream_host on|off`, title screen, four alternating 4 s windows:
  device 33–50 ms / 33 ms frames (GPU thread saturated, `Obtain.Stream` 1.4–2.0 s),
  host **16.7 ms / 16.7 ms** (60 FPS, `Obtain.Stream` 0.5 s, GPU thread not saturated).

Now: discrete GPUs (`Instance::IsDiscrete`) get the host stream buffer (64 MiB) and use it by
default for everything `BufferCache::GetStreamBuffer()` serves; integrated GPUs and Android
(unified memory) keep the single device stream buffer and allocate nothing extra.
`gpu_memory status` prints `stream copies/bytes/sizes/host_memory`; `upload_diag stream_max
<bytes>` lowers the stream threshold for experiments.

Also found: during loading some map/unmap calls hold the VM writer lock for ~5 ms
(`Memory.WriteHold.Map`), stalling the GPU thread; the Windows placeholder operations are now
instrumented (`AddressSpace.*`), cause not yet identified.

## 9. Open

- Cabin-scene FPS with the host stream buffer still to be measured (title: 27 → 60 FPS).
- Load-time map/unmap writer holds of ~5 ms.
- Android: done, see §10.

## 10. Android freeze with the shared core; Android back on its original implementation

Symptom (Pocket DS, Turnip mainline, Bloodborne, APK built from `793e4e24..a515631e`): the game
stops presenting frames (FPS 0) — twice on the loading screen after Continue, once at the start-up
logos after 70 flips. The previous APK (before the shared core) did not.

Evidence (root `debuggerd -b` via the ayaneo-root adapter, two freezes): the GPU command thread
idle in `Liverpool::Process` with nothing submitted; the main guest thread and 23–36 others parked
in the condition variable (`CondPlatform::Park`, untimed), 15 in kernel semaphores, 9–10 polling
with `usleep`; no host lock held, no thread in a fault handler. `hle_sync` during the freeze: no
condition signals at all, semaphores and rwlocks busy and balanced, no non-zero results.
`upload_diag keep_gpu off` and `read_cache off` applied in the first seconds of a run did not
prevent it (frozen at 4872 flips for 95 s after Continue): not the §5/§8 video_core changes.

Root cause, a lost wakeup in the shared `ConditionVariable::Wait`: the waiter released the
caller's mutex first and counted itself in `queued` afterwards, while `Signal`/`Broadcast` return
without taking the lock when `queued == 0`. A thread that takes the just-released mutex, changes
the predicate and signals inside that window finds no waiter; the waiter then sleeps forever. The
original Android condition had no such shortcut (the notifier always took the condition guard
that the waiter held across unlock and enqueue). Fixed in the core: queue and count before
`ReleaseMutex` (undone on its error); the mutex release orders the count before the signaller's
acquire. APK with the fixed core: after Continue 1311 → 2138 flips in 60 s, loading screen at
24 FPS.

Second defect found in the same review: `Parker` never cleared `woken`, so a rwlock or `sem_t`
waiter that was woken, lost the race and parked again returned at once — a busy spin until it
won. `Park` now consumes the wake. Also the `sem_t` count re-read after registering a waiter is
sequentially consistent (same instruction on x86-64 and ARM64; closes the memory-model gap of
the post/wait handshake).

Regression tests in `tests/sync/sync_core_tests.cpp`: a waiter whose `ReleaseMutex` lets the next
mutex holder signal inside the window; `Parker` wake consumption; a woken rwlock writer overtaken
by a reader. With the previous headers two checks fail (the waiter times out; a second `Park`
returns `Woken`); with the fixes 20769 checks / 0 failures, three desktop runs.

Old Android implementation compared with the shared core:

| Primitive | Original Android | Shared core |
|---|---|---|
| Kernel semaphore | one mutex for every semaphore of the session; per-waiter park | per-semaphore lock, locked id table; same token, queue and timeout rules |
| `sem_t` | domain guard held for every wait and post; waiters wait on the domain guard | atomic count (no lock uncontended), FIFO, one wake per post, pass-on on timeout/stop |
| rwlock | one mutex for every rwlock; `notify_all` wakes every waiter of the lock | per-lock mutex, wakes only admitted waiters; same owner/type rules |
| Condition | per-condition guard, the notifier always locks | per-waiter slot, lock-free empty check (the lost wakeup) |
| Event flag | map + mutex over `EventFlagState` | shared id table, same decode rules |
| Equeue wait | polls every 1 ms | `WaitReady` on the queue's condition variable |

The shared core scales better (per-object locks, targeted wakes), but the platforms differ in
principle (guest pointers pinned through `GuestAddressSpace`, guest-memory mutex word with a
guest fast path, session stop tokens) and a change for one side silently changed the other. By
decision of the user the two are maintained separately with the same interface
([sync-core.md](../../sync-core.md)): the Android host runtime is back on its original files
(`guest_kernel_semaphore.h`, `guest_semaphore.h`, `guest_rwlock.h`, `guest_mutex.h`, the event
flag handlers in `guest_runtime.cpp`, the equeue HLE in `guest_graphics_hle.cpp` and
`guest_graphics.cpp`, all as of `c7b02fc9`); `EqueueInternal::GetTriggeredEvents` (events only,
no small timers) is back for that HLE. The shared core stays as the desktop implementation, with
the fixes above; the Android host build no longer builds `sync_core_tests`.

Android with the original implementation (Pocket DS):

| Check | Result |
|---|---|
| `guest_kernel_semaphore_tests` | 588/0 |
| `guest_condition_tests` | 67/0 |
| `guest_native_mutex_tests` | 65499/0 |
| `guest_rwlock_tests` | 73/0 |
| `guest_equeue_tests` | 43/0 |
| `host_event_flag_tests` | 1205/0 |
| `host_library_smoke` (includes `sem_t`) | 511/0 |
| Bloodborne, Continue, run 1 | 1488 → 2473 flips in 60 s, Central Yharnam at ~15 FPS, rendering correct |
| Bloodborne, Continue, run 2 | 1494 → 2614 flips in 60 s |

The GPU byte keeper, read cache and other video_core changes stay on both platforms.
