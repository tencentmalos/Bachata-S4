# HN0.2 — FEX allocator ownership audit and provider feasibility

Date: 2026-09-11. Source pins: main repo `codex/android-fex-round2`; FEX gitlink
`385a0cc4d81cd456c8d5c26b4f09cb5a7d8d5842` (`references/FEX`). This is the HN0.2
deliverable from [the host-native v1 spec](../../specs/android-native-host-v1.md):
who owns the FEX allocator VA, when it is taken, how failure/occupancy is handled,
and whether a provider that hands FEX a pre-owned region is possible through the
public API. Findings are read from the pinned FEX source; no FEX child was
modified (its AGENTS/CLAUDE forbid AI-generated contributions — inspection only).

## 1. The two allocators are different objects

FEX has two independent allocators; conflating them is how the earlier probe
ended up "proving" the wrong thing.

- **Small-object allocator** (`AllocatorHooks.cpp`): rpmalloc/jemalloc for host
  C++ allocations, initialised by `InitializeAllocator(PageSize)`. On the
  **bionic** build path this is a no-op and `malloc`/`free` forward straight to
  bionic (`AllocatorHooks.cpp` `#else` branch). It does **not** reserve a large
  VA window and is not the ART-race hazard.
- **64-bit guest VMA host-allocator** (`Allocator/64BitAllocator.cpp`,
  `OSAllocator_64Bit`): the object that reserves guest VA in
  `[LOWER_BOUND=0x1_0000_0000, UPPER_BOUND)` where `UPPER_BOUND` comes from
  `GetHostVABits()`. **This** is what races ART and aborts the process.

`FEXCore::Allocator::SetupHooks(size_t)` (public, Linux) wires both:
`Allocator.cpp:91` `SetupHooks` → `Alloc64 = Create64BitAllocator()` then
`AssignHookOverrides` → `InitializeAllocator`.

## 2. How the guest VMA allocator takes its region (the abort path)

`OSAllocator_64Bit::OSAllocator_64Bit()` (`64BitAllocator.cpp:544`):

1. `DetermineVASize()` → `GetHostVABits()` probes the top page at 57/52/48/47/42/
   39/36 bits with `MAP_FIXED_NOREPLACE` and takes the highest that maps or
   returns `EEXIST`.
2. `StealMemoryRegion(LOWER_BOUND, UPPER_BOUND)` (`Allocator.cpp:217`):
   - `CollectMemoryGaps` parses `/proc/self/maps` for gaps between existing
     mappings in the range;
   - it then `mmap`s **every** gap `PROT_NONE | MAP_FIXED_NOREPLACE`, and on any
     failure calls `LogMan::Throw::AFmt(...)` → `ERROR_AND_DIE` → a trapping
     instruction → **SIGILL, whole process dies**. It never returns an error.
3. `AllocateMemoryRegions(Ranges)` (`64BitAllocator.cpp:495`): needs one gap
   `>= ObjectAllocSize = 64 MiB` for the object arena; if none,
   `ERROR_AND_DIE_FMT("Couldn't allocate object allocator!")` — again a fatal
   abort, not a recoverable error.

Consequence: **there is no in-band error from SetupHooks.** Either it succeeds, or
the process is already dead. Any safety must therefore be a *pre-check that
releases what it maps before* SetupHooks runs, plus a retry policy for the only
failure that can return (the pre-check itself giving up).

## 3. The ART race, precisely

In an app process the ART runtime (JIT threads, GC heap, class-loader mappings)
keeps mapping/unmapping while the first guest context is created. Two distinct
ways that aborts FEX:

- A gap `CollectMemoryGaps` observed becomes occupied before FEX's
  `MAP_FIXED_NOREPLACE` reaches it → `EEXIST` → abort (step 2).
- ART fragments the range so no single `>= 64 MiB` gap remains → abort (step 3).

The window is real but transient; empirically it closes seconds after startup.
This is not true VA exhaustion (the device reports gaps far larger than 64 MiB).

## 4. Can a provider hand FEX a pre-owned region? (the clean fix)

FEX **does** have a regions-taking path internally:

- `OSAllocator_64Bit::OSAllocator_64Bit(fextl::vector<MemoryRegion>& Regions)`
  (`64BitAllocator.cpp:552`) builds the allocator from **caller-supplied**
  regions instead of self-stealing.
- `Create64BitAllocatorWithRegions(Regions)` (`64BitAllocator.cpp:620`) is the
  factory, and `Allocator.cpp:286` uses it inside a regions-taking init.
- `StealMemoryRegion` / `ReclaimMemoryRegion` / `CollectMemoryGaps` /
  `Setup48BitAllocatorIfExists` are all `FEX_DEFAULT_VISIBILITY` (public).

But the **public** `AllocatorHooks.h` exposes only `SetupHooks(size_t)` (Linux)
and `SetupHooks(size_t, HookPtrs)` (`_WIN32` only). `Create64BitAllocatorWithRegions`
and the regions-taking `SetupHooks`/init are **not in a public header**.

This kills the naive public-API provider: if we pre-`StealMemoryRegion` the range
ourselves and then call the public `SetupHooks`, FEX's constructor re-runs
`StealMemoryRegion` on the **same** range, now sees our PROT_NONE maps as
occupied (not gaps), finds no `>= 64 MiB` gap, and aborts. Pre-stealing and the
public entry point **do not compose**.

**Verdict for the fully-atomic-ownership provider: BLOCKED.** A correct provider
needs the region handed to FEX so it does not re-steal, i.e. one of:

- a version-pinned adapter that calls the currently-internal
  `Create64BitAllocatorWithRegions` (requires internal symbols/ABI lock, and a
  matching internal `SetupHooks` that installs `Alloc64` from those regions), or
- an upstream public API, e.g. `SetupHooks(size_t PageSize, std::span<MemoryRegion>)`
  that takes pre-owned regions.

Either is a change to the FEX child, which this task does not make. The gap is
recorded here; the minimal-interface proposal is the span-taking public
`SetupHooks` above.

## 5. What is safe to fix now (implemented)

Two defects in `src/core/guest_cpu/fex/fex_context.cpp` are independent of the
BLOCKED provider and are fixed in this change:

- **`call_once` permanently swallows a transient timeout.** The old code ran the
  probe+SetupHooks inside `std::call_once(allocator_once, ...)`. If the probe
  timed out, the lambda returned normally, `allocator_once` was consumed, and
  `allocator_ready` stayed false **forever** — every later `CreateContext`
  returned OutOfMemory even after ART quiesced. Replaced with a retriable
  mutex-guarded guard: SetupHooks is still called **at most once successfully**
  (it leaks on ClearHooks, so it must never run twice), but a probe timeout now
  leaves the guard un-set so the next `CreateContext` retries. Because SetupHooks
  cannot return on failure (§2), "SetupHooks was called" == "it succeeded", so
  the retry can never double-install.
- **The probe's abort risk is bounded, but it is a mitigation, not a proof.**
  `WaitForClaimableAllocatorRegion` (unchanged in this pass) already does a real
  check, not a blind sleep: it detects the VA ceiling with the same top-page
  `MAP_FIXED_NOREPLACE` probe FEX's `GetHostVABits` uses, requires a genuinely
  claimable contiguous 256 MiB run at the low end of `[LOWER_BOUND, UPPER_BOUND)`
  to stay held across a 60 ms dwell for 5 consecutive cycles, and `munmap`s every
  page it mapped before returning — so it never poisons the gaps SetupHooks is
  about to steal. Its known limits, left as-is because they cannot be device-
  verified here and the mitigation works: it samples a 256 MiB low-end run while
  FEX will accept any 64 MiB gap anywhere in the range, and it does not re-check
  the run's identity after the dwell (it relies on the consecutive-cycle count).
  It remains a *mitigation*: the race in §3 is only fully closed by the §4
  provider, because nothing can guarantee the region survives between the probe's
  release and FEX's steal. The retriable guard above is what makes a lost race
  recoverable instead of fatal-on-first-try-then-permanently-broken.

## 6. Acceptance status (HN-A01)

- Public-API atomic-ownership provider: **BLOCKED** (needs FEX internal
  `Create64BitAllocatorWithRegions` exposure or a public span-taking SetupHooks).
- `call_once` permanent-swallow: **FIXED** (retriable guard).
- Blind probe: the probe was already a real gap/VA-ceiling check that releases
  before SetupHooks (not the blind sleep first assumed); left unchanged as a
  sound mitigation, with its limits recorded in §5. Still a mitigation, not proof
  of quiescence.
- 100 cold-start / 100 same-process context-rebuild device runs under normal app
  UID with Java allocation/thread/Surface churn: **NOT_RUN** here (device-side;
  belongs to the HN0 device acceptance window on `9c2841a4`). The retry fix is
  what makes the same-process rebuild case meaningful — previously a single early
  timeout would have failed every subsequent rebuild.

Per the spec: A01 being BLOCKED means this does **not** claim "a stable native
host allocator base is complete." It delivers the evidence and the exact minimal
interface gap, and the two safe repairs, and continues the HN0/HN1 work that does
not depend on the clean provider.
