# DiagnosticsHub producer wiring — session backend + present count (Work Package A, step 6)

2026-09-14，主仓 `codex/android-fex-round2`,实施 [图形调试工具整包 spec](../../specs/android-graphics-debugging-toolkit.md) §3.1 的第一个真实 producer:把 session 生命周期与真实 present 计数接进 DiagnosticsHub。此前几步只是插座;这一步让 `debug_status` 读到真实身份与推进,而非常零。

## 交付

**`GuestRuntime::PresentCount()`**(guest_runtime.{h,cpp}):公有访问器,在 `impl->graphics_mutex` 下返回 `VideoOut().guest_presents`(graphics 未创建时 0),复用与 `Diagnostics()` 完全相同的读法。不把内部 `VideoOut` 驱动暴露给 backend。

**`session_backend_fex.cpp` producer 接线:**
- **Prepare 成功末尾**:`DiagnosticsHub::Instance().Register(generation, getpid())`,把发布器存到 `FexSessionRuntime::diag`;`MarkAvailable(GpuRetire, false)`(无 timestamp-query 后端,标 UNAVAILABLE 而非 0);`SetStage("ready")`。Register 是 Prepare 最后一个不抛异常的步骤,之前抛出则未注册、无需回收。
- **Run 返回处**(已有 `Diagnostics()` 调用旁):`PublishCount(HostPresent, PresentCount(), MonotonicNs())` + `SetStage("returned")`。`guest_presents` 仅在真实 `Presenter::Present` 成功时自增,故这是真实推进,非心跳。
- **Destroy 开头**:`Revoke(generation)`(generation-checked,更新的 session 已注册则不动),再 `diag.reset()`。Destroy 必对已 prepare 的 runtime 调用,故 Revoke 必发生。
- 所有 push 由 `if (rt.diag)` 守护;注册失败(理论上 generation==0)则全部 no-op。

## 验证

**真实 NDK 编译**(此前几步只在 macOS 本机编译;本步涉及仅 Android 才构建的 session backend,故用真实工具链):NDK r29 `aarch64-linux-android33`、`-std=c++23`(仓库标准)、`-Wall`、`-DSHADPS4_TYPED_HLE_HOST=1`、完整 host include 环境(bionic sysroot、FEX 头 `build/fexcore-android/include`、debugbus、foundation_input、fmt、全部 externals)。五个受影响 TU 全部 **exit 0**:
- `src/core/diagnostics/diagnostics_hub.cpp`
- `src/core/diagnostics/diagnostics_hub_registry.cpp`
- `src/core/diagnostics/diagnostics_commands.cpp`
- `src/core/host_runtime/session_backend_fex.cpp`(改动的 backend)
- `src/core/host_runtime/guest_runtime.cpp`(新 PresentCount)

前几步的 hub/registry/commands 单元测试(28/26/41)不受影响,逻辑未变。

## 边界(诚实记录,非 bug)

- **HostPresent 目前只在 Run 返回时发布一次**,非逐 present 实时。对「黑屏后是否曾成功 present 过」的排查有效(Stop 后读最终计数);但运行中 `debug_status` 的 HostPresent 会停在上次发布值。逐 present 实时 hook(在 present 路径调用 hub)是下一步。
- **phase 数字未接**:Phase 归 SessionCore 所有,本步未接 SessionCore→hub,故 `debug_status` 的 `phase` 字段仍为 0;真实生命周期由 backend 拥有的 `stage` 字符串("ready"/"returned")承载。SessionCore phase/run_uuid 接线是后续。
- 其余推进信号(guest_flip/pm4_consumed/host_draw/queue_submit/overlay_redraw)仍未接 producer,`debug_status` 显示 0/never——接线待办。
- `Service.dump` 尚未 `SetDumpsysRegistry`;命令入口的 app 接线是后续。
- 未做完整 Android host/APK 链接或真机运行验证(本机不可跑),仅真实 NDK 逐 TU 语法编译。未改 FEX/Foundation/Citron。
