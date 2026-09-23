# game-input-event.v1 trace

`pad trace` is a session-bound, fixed-size diagnostic ring at the
`OrbisPadAdapter::Publish` merge boundary. It is deliberately separate from
the existing `pad state`/`pad release_all` protocol, so existing debugbus
clients keep their wire shape.

Commands are issued through the existing `pad` debugbus command:

```text
pad trace start PID GENERATION RUN_UUID GAME_ID
pad trace read PID GENERATION RUN_UUID TRACE_ID AFTER_SEQ LIMIT
pad trace stop PID GENERATION RUN_UUID TRACE_ID
```

`read` never consumes entries. A cursor older than `oldest_seq - 1` returns
`status=gap`, while a stale PID/generation/run UUID returns
`status=wrong_session`; reconnects therefore cannot be mixed with an old
cursor. The response exposes `oldest_seq`, `next_seq`, `dropped`, `overflow`,
`active`, and a `CLOCK_MONOTONIC` `clock_sample_ns`. The ring is 4096 entries.

Each event is JSONL-compatible `game-input-event.v1` data. It carries the full
normalized state and edge lists for `physical`, `overlay`, `debugbus`, and
`merged`, plus PID, generation, run UUID, game ID, source ID, port, action ID,
monotonic timestamps, and `related_seq` for Guest polling. `initial_state`,
`command_requested`, `state_published`, `guest_polled`, `released`, and
`trace_stopped` are emitted at the merge boundary. Guest `scePadRead` and
`scePadReadState` pass `guest_read=true`, so a real Guest poll updates the
debugbus receipt and is visible in the trace; diagnostic reads do not.

The trace is bounded and in-memory. It is intended for short AYN captures and
host regression tests, not persistent logging or a replacement for logcat.
