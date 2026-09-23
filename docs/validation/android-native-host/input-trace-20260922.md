# InputHub merge trace — bounded AYN validation

This change adds a fixed 4096-entry trace ring at the OrbisPadAdapter merge
boundary. The command remains under the existing `pad` debugbus namespace:
`trace start`, `trace read`, and `trace stop`. Reads are cursor based and do
not consume entries. A stale lifecycle identity is rejected and an old cursor
returns `gap` with the retained range metadata.

Offline/device checks:

- Android host `debug_pad_tests`: **2,588 checks, 0 failures**. This covers
  initial state, source merge, JSON event shape, guest poll correlation,
  release/focus/timeout paths, duplicate IDs, reconnect lifecycle, and a
  direct overlay submission overflow/gap exercise.
- Final AYN APK was installed from
  `build/validation/debug-pad-trace-20260922/shadps4-debug-pad-trace.apk`
  (APK SHA-256
  `44a5975eac6979020507d3403bb25c6f25ac5852a982dfc757ec3385f5b1d7eb`),
  with `libshadps4_host.so` SHA-256
  `5149cde1d520e371857cbf4efe5c71f30f05561118c647077b27fec06031171e`.

AYN `9c2841a4` was running the current MHW session for the final APK (PID
32217, generation 1, run UUID `61d7a711765ed565869bab4bc7435a16`). A trace was
started with game ID `MHW-CUSA09554`, a debugbus Cross press was issued, and a
real checked Guest poll was observed. The returned events included all four
initial source records (`physical`, `overlay`, `debugbus`, `merged`), a
`command_requested`, the debugbus `state_published` event with the Cross edge,
and `guest_polled` events with `related_seq=19`. The trace was then stopped
and the held input was released with `release_all`.

Raw evidence is kept under:
`build/validation/input-trace-20260922/runs/ayn-input-trace/`:
`trace-start.txt`, `state-press.txt`, `trace-read.txt`, `release.txt`,
`trace-stop.txt`, `pre-install-logcat.txt`, and `post-logcat.txt`.
The final APK rerun is in `runs/ayn-input-trace-final/`.

The implementation is intentionally in-memory and bounded. It does not write
continuous JSONL to logcat; `trace read` returns both structured `events` and a
newline-delimited `jsonl` field for capture tooling.
