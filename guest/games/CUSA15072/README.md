# KINGDOM HEARTS III (CUSA15072)

Guest function packages for the installed build: `param.sfo` APP_VER 01.00, VERSION 01.10
(single merged package). Offsets are eboot module offsets; the IDA database is built from the
analysis ELF (`tools/ps4-guest-code`, loaded as ET_DYN), kept outside the repository.

Both waits below are in one package, `01.10/cpu_waits.recipe.json` (id `kh3_cpu_waits_01_10`),
because both sites call the eboot's `sceKernelUsleep` and two packages may not call the same
code. Each site can be switched on its own: `guest-patch SERIAL disable kh3_cpu_waits/<site>`.

## Render thread: sleep while waiting for GPU queries (`query_wait.cpp`, site `query_wait_back_edge`)

The render thread (Guest-57 on Odin3, UE4 `FDrawSceneCommand` at eboot+0x44752a0) reads
occlusion and timestamp query results through GnmRHI `RHIGetRenderQueryResult`
(eboot+0x3955d0, RHI vtable +728). With `bWait` set and the result missing, it calls
eboot+0x3a0d10, which polls without ever sleeping: process the render thread's queued tasks,
test the query words (the begin/end counters of all 8, on Pro 16, render backends carry the
valid bit 63; a timestamp is non-zero), check a timeout (constants at eboot+0x7955178/80),
repeat. On success it adds the wait to a per-frame idle statistic (eboot+0xbe0e20c count,
eboot+0xc02ba50 time).

The emulator writes those words when the GPU command processor thread parses the query's
`EVENT_WRITE ZPASS_DONE` (`liverpool.cpp`) or end-of-pipe packet. In a GPU-bound scene that
thread is throttled by the host GPU, so the render thread spins for a large part of a frame.
The package puts a `before` site on the loop's back edge (eboot+0x3a0eee,
`cmp byte [r15], 0; jz eboot+0x3a0d70`): when the result is still missing it calls the game's
own `sceKernelUsleep(100)` (PLT eboot+0x77499e0) before the next round. Everything else in the
loop, and its timeout, is unchanged. `guest-patch.log` gets the running number of sleeps every
16384 sleeps (tag `query_sleeps`).

Callers: eboot+0x44919e0 (occlusion test, always waits) and eboot+0x455f0f0 (query-backed
fence/timer object, waits when asked).

## Game thread: sleep through the frame-rate cap (`tick_wait.cpp`, site `tick_wait_round`)

UE4 `UEngine::UpdateTimeAndHandleMaxTickRate` (eboot+0x5d8dfc0) holds each frame to the maximum
tick rate: with more than 5 ms left it sleeps all but 2 ms (`sceKernelUsleep`), then loops on
`scePthreadYield` until the end time (double at rbp-0x40, process time counter times the
seconds-per-cycle global at eboot+0xa7065f8) has passed; with 5 ms or less it yields the whole
wait. In the Olympus scene that is about 26 thousand yields a second on the game thread (Guest-1),
each an HLE round trip plus a system call. The site replaces the loop's `call scePthreadYield`
(eboot+0x5d8e2ea): with more than 0.4 ms left it sleeps all but 0.2 ms, closer to the end it
yields as before. The loop's own end check is unchanged. Tag `tick_sleeps` every 4096 sleeps.

## Version check

The build is named by `module_signatures` (sdk_version 3): the query wait loop's entry, its
timeout check and back edge, the entry of `RHIGetRenderQueryResult`, the entry of
`UpdateTimeAndHandleMaxTickRate` and its wait loop.

```bat
python tools\guest-functions\build.py --clang <NDK>\toolchains\llvm\prebuilt\windows-x86_64\bin\clang.exe ^
  --recipe guest\games\CUSA15072\01.10\cpu_waits.recipe.json --output build\guest-patches\CUSA15072-cpu-waits
python scripts\android\guest-patch SERIAL install build\guest-patches\CUSA15072-cpu-waits\patch.json --name kh3_cpu_waits --select
```

Measurements: [KH3 loading/TLS/rwlock record](../../../docs/validation/android-native-host/kh3-loading-tls-memlock-20261013.md).
