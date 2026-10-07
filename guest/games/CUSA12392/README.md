# ASTRO BOT Rescue Mission (CUSA12392)

Guest function packages for the two builds AstroQuest knows from inside: 1.00 (disc) and
1.04 (last update). Neither build is on this machine; the packages are built and unit-tested
for format only, not run.

## Real-time game speed (`time_step.cpp`)

The title advances its world by 1/60 s for every frame it draws, however long the frame took,
so below 60 FPS the whole game runs in slow motion. Its engine keeps the step in three globals
(frame rate as a double, seconds as a float, microseconds as a u64). The package hooks the
eboot's import of `sceGnmSubmitDone` (NID `yvZ73uQUqrk`, sdk_version 3 import hook) and, before
every frame is handed in, writes those globals with a smoothed measure of real frame time:

- follows 15% of each frame's change, repays 10% per frame of up to 100 ms of accumulated drift
  (keeps picture and sound together), clamped to 1/60–1/20 s, snaps to exactly 1/60 s at 60 FPS;
- frames longer than 250 ms (loading, breakpoints) do not count.

Algorithm and addresses: AstroQuest v0.18 `9ff3e43`,
`shadps4-arm64-main/src/core/known_title.cpp` (`TimeStep`, `OnFrameSubmitted`) and
`known_title_builds.h` (GPL-2.0-or-later). The addresses of 1.04 were found by Clodo76
(AstroQuest issue #1).

Each recipe names its build by `module_signatures` (sdk_version 3): the tracking manager's
recentre setter code, the three time-step constants as the console has them, and the scene size
tables, all at the build's own offsets (AstroQuest `Builds::Is`). A different build fails the
check and the game runs without the package.

Counters `step_us` / `frame_us` go to the litep ring; `guest-patch.log` gets the step every 600
frames.

```bat
python tools\guest-functions\build.py --clang <NDK>\toolchains\llvm\prebuilt\windows-x86_64\bin\clang.exe ^
  --recipe guest\games\CUSA12392\01.00\time_step.recipe.json --output build\guest-patches\CUSA12392-01.00
```

Not ported: AstroQuest's resolution governor and larger render sizes (`SizeChanges`), which
change sizes in the executable's data and code and depend on extra graphics memory.
