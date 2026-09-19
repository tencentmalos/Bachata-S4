# Mixed-stack / watchpoint evidence

Read `../guest-debugger-mixed-watch-2026-09-15.md` and `artifacts.json` first.
`final/` is the final source/client/binary validation. Top-level logs preserve
intermediate results and failures and must not be combined as one deployment.
FEX is based on `385a0cc4` with the exact local six-file patch in this directory.
Main and prior audio/Foundation work remain uncommitted; this is not a fetchable
new main/FEX pin. Native MCP source changes have focused local commits.

Targeted native validation:

```sh
cmake --build build/fexcore-android-api33 --target FEXCore -j6
cmake --build build/v0-fex-test --target guest_execution_tests -j6
adb -s 9c2841a4 push build/v0-fex-test/guest_execution_tests /data/local/tmp/shad-guest-debug-tests
adb -s 9c2841a4 shell 'LD_LIBRARY_PATH=/data/local/tmp/shad-audio-profile timeout 35 /data/local/tmp/shad-guest-debug-tests --focused-debugger'
```

For the real MCP bridge, start the same binary with `--serve-debugger-watch` or
`--serve-debugger-mixed`, then run `python3 mcp_accept.py watch` or `mixed` in
another terminal. It uses the installed native-debugger wrapper and fixture
addresses, owns forwarding 24681 → 24680, always calls stop_session, and writes
a fresh temporary evidence directory (or `FEX_EVIDENCE_DIR`). The fixture server
cancels itself after 25 seconds. Do not overlap servers with the APK listener.
The shell fixture is auxiliary evidence; ordinary APK evidence is separate.

APK selector: `com.shadps4.android.GuestDebuggerInstrumentedTest`; opt-in
properties are `debug.shadps4.guest_debug_port=24680` and
`debug.shadps4.guest_debug_wait=1`. Clear both afterward. With them cleared, run
the same selector with `-e debuggerDisabled true` to verify no listener and a
cancellable production Run. Native/MCP checks never constitute game acceptance.
