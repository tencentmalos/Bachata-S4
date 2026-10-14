# PROF streaming integrity and HLE name registration — 2026-10-13

This integration changes only the profiler adapter, its Foundation pin, and
this publication record. It is based on `malos/main` at `550fcc702`; the other
committed and uncommitted Swan performance changes are excluded.

## Source ownership

- Foundation `c24ffeff51ebfd47f8da5bcd918ef7f1dbdc76b4` is published on
  `tencentmalos/foundation` branch `codex/prof-shad-pin-20261013`. It applies
  the profiler-only change `b977fef` to this integration branch's existing
  Foundation pin `45f3d258`. It does not import the newer Summary UI change.
- Foundation `main` separately contains the same change at `fee991e`, with
  the two-file non-frame activation prerequisite at `269af92`. The core,
  profiler SDK and capture-check sources match the validated `b977fef` tree.
- LiteP readers and queries are in `tencentmalos/my_mcp_tools` commit
  `cadcb72`. Its `dev_tools/mcp/litep/docs/validation/prof-streaming-20261013.md`
  contains the complete measurements and regression limitations.
- The child branch and commit were verified with `git ls-remote` before
  advancing this parent gitlink. `.gitmodules` names that published branch.

## Behavior and validation

HLE adapters register their permanent profiler names when bindings are built.
Invocations use the registered pointer, avoiding dynamic name encoding on each
call. Foundation emits self-describing PROF v4 records, actual capture endpoints,
and a CPU/Unix clock anchor. LiteP consumes them directly with streaming
aggregation and explicit integrity diagnostics.

The earlier validation compiled the production Android profiler and guest
runtime objects, Foundation core and SDK. Before publication, the two adapter
translation units were also checked with the NDK 29 API 33 compiler against
this clean `malos/main` integration and the exact published Foundation pin.
This targeted check reuses existing external dependencies and generated build
headers; it is not a new complete APK build.

The real Foundation capture check on Odin3 passed four alternating dynamic /
registered sessions: 400,002 events each, two HLE names at 100,000 calls each,
no loss, and over 99.9% declared-window coverage. Registered captures were
about 22% smaller. Python regressions passed 301 tests across LiteP and both
reader trees. Native framing and encoding checks passed; the broader native
suite retains the failures documented in the LiteP validation record.

No new game APK installation, KH3 loading simpleperf measurement, or game FPS
improvement is claimed. Original dirty workspaces and generated evidence are
preserved outside this commit.
