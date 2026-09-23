# MHW follow-up after input trace

The final trace APK was left running on AYN after the bounded input capture so
the MHW session was not reinstalled or interrupted again. The live session is
PID 32217, generation 1, run UUID
`61d7a711765ed565869bab4bc7435a16`, Turnip, phase Running.

A 45-second log window is saved at
`build/validation/input-trace-20260922/runs/mhw-followup-final/logcat-45s.txt`.
It contains a stable, high-frequency sequence for
`+MCXJlWdi+s` (`RemoteService no_provider`, result `0x810e0004`, roughly every
16 ms) and no new `DeviceLost`, `BackendFailed`, or GPU snapshot in this
bounded window. The current status is in `status.txt`.

This narrows the next MHW investigation to the SharePlay connection-info
no-provider path and its guest caller's handling of the repeated offline
error. The existing ABI deliberately returns the firmware-style
not-initialized error and does not fabricate an output object; no speculative
success return was added in this pass.
