# Measurement scripts of this report

Copied from the session scratchpad; they ran from one directory against the AYANEO Pocket DS (`SERIAL`, default `01108YHE01017563`) with `MSYS_NO_PATHCONV=1` in Git Bash on Windows.

- `ab_content.py <window_s> <label> "<DebugBus prefix>" v1 v2 ...`: alternates a DebugBus switch in the running session, settles (`SETTLE`), samples per-thread CPU (`athreads.sh` / `athreads_parse.py`, from `/proc/<pid>/task/*/stat` and `debug_status` guest flips) and the write-watch counters of `upload_diag status`, and prints one line per window. Prefix `-` takes `cmd1;cmd2` values; `@aff` applies `aff.sh` masks. Stops with exit code 3 when the battery is below `MIN_BATTERY` (default 6).
- `aff.sh <mask> [regex]`: `taskset` of guest threads through `run-as` (default every `Guest-*` but Guest-1).
- `android_nav.py [timeout_s]`: presses Circle (`apad.sh`) on stable low-draw screens until the world draws more than 600 per flip.
- `run_exp.sh` (round 2), `run_exp2.sh` (round 3), `run_exp3.sh` (round 4; wakes and unlocks the device before `am start`, see report 7.8), `run_exp3b.sh` (round 4 against an already running session): the A/B sequences; `wait_and_run2.sh` waits for a battery level (`TARGET`) and runs `RUN`; `stop_game.sh` stops the session through the UI and turns the screen off.
- `athreads.sh` names its device scratch files per window label, so two runs cannot overwrite each other's snapshots (they did in the contaminated round 4). After stopping a background waiter, check that no `bash`/`sleep` process is left (PowerShell `Get-CimInstance Win32_Process`).
- `r3_capture.sh`: one 10 s Perfetto sched trace (`perfetto-sched.cfg`) and one 10 s simpleperf recording.
- `tpq.py <trace> <sql>`: runs `-- @@`-separated queries (`tp-r3.sql`, `tp-mainwait.sql`) with LiteP's bundled `trace_processor_shell.exe` (machine-local path inside).
- `perfagg.py`, `perfincl.py`, `kcallers.py`: simpleperf aggregation through the NDK `simpleperf_report_lib.py` (machine-local NDK path inside); samples at 2000 Hz count 0.5 ms each.
