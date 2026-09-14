# DiagnosticsHub queue_submit producer + run_uuid (Work Package A, step 9)

2026-09-14，主仓 `codex/android-fex-round2`,继续 [图形调试工具整包 spec](../../specs/android-graphics-debugging-toolkit.md) §3.1 producer 接线,并让 `debug_status` 的 run_uuid 不再是 unknown。

## 交付

**queue_submit producer**(guest_graphics_hle.cpp):
- 非 flip 的 `sceGnmSubmitCommandBuffers` 成功(返回 0)时 `Advance(QueueSubmit)`。
- flip 的 `sceGnmSubmitAndFlipCommandBuffers` 成功时**同时** `Advance(QueueSubmit)` + `Advance(GuestFlip)`——一次 flip 本身也是一次 submit,语义准确。
- 三个信号的组合是分层黑屏判据:submit 推进但 GuestFlip 平(在提交但从不 flip)≠ GuestFlip 推进但 HostPresent 平(flip 了但没呈现)。

**run_uuid 生成**(trace_identity.h `MakeRunUuid()`):随机 128-bit → 32 位小写 hex。诊断关联 id(run 域),非安全 token,故用 `std::mt19937_64` 无依赖实现。backend 在 Register 处 `SetRunUuid(MakeRunUuid())`,`debug_status` 的 `run_uuid` 现为真实值而非 unknown。

## 验证

- `trace_identity_tests` 扩到 **27 checks**(新增 MakeRunUuid:32 位、全 hex、每次不同),本机 `clang++ -std=c++23 -Wall -Wextra` 通过。
- **真实 NDK r29** `aarch64-linux-android33 -std=c++23 -Wall`:改动的 `guest_graphics_hle.cpp` 与 `session_backend_fex.cpp` 均 exit 0。
- 既有 hub/registry/commands/service 单测逻辑未变。

## producer 现状

已接:host_present(Run 返回 + 逐 flip 实时)、guest_flip、queue_submit。未接:pm4_consumed、host_draw、overlay_redraw、gpu_retire(标 UNAVAILABLE)。SessionCore phase/run_uuid 中的 phase 仍未接(run_uuid 本步已接)。

## 边界

- QueueSubmit 只在 submit HLE 成功分支计数;校验失败(0x80d11000)不计,语义正确。
- 未做完整 host/APK 链接或真机运行;真实 NDK 逐 TU 编译 + 本机单测。未改 FEX/Foundation/Citron。
