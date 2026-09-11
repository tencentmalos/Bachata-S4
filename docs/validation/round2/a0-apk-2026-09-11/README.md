# A0 APK 真机证据 — 2026-09-11

配套记录：[../a0-apk-2026-09-11.md](../a0-apk-2026-09-11.md)。设备 AYN Thor / API33 / 4KiB / va_bits=39。

- `device-crash-logcat.txt`：**未修复时**首次启动的崩溃复现,含 `FexCore: [A] Couldn't allocate object
  allocator!` 与 `ForcedAssert → SetupHooks → CreateFexContext → runGuestIncrement` 的 SIGILL(ILL_ILLOPC)链。
- `device-run-delay0-fixed-pass.txt`：**修复后** delay=0 冷启动,首次 CreateContext 约 425ms 静默等待后
  guest 执行 10/10 PASS(input+1 正确)。
- `device-run-3s-delay-pass.txt`：延迟 3s 的对照成功(证明"等 ART 静默"即可)。

## 根因与修复(见配套记录 §4/§5)

根因是 **ART 冷启动期与 FEX 64-bit 对象分配器抢占 `[4GiB,1<<39)` VA 窗口的时序竞争**,不是 VA 布局不足
(app 自测报告该区间有 4 个 ≥64MiB 空洞)。对照:同 FEXCore 的 CLI 裸进程在同设备一次通过;app 进程 delay=0 约 80% 崩、
delay=2s 12/12 通过。修复为适配器侧 `WaitForClaimableAllocatorRegion()`(等地址空间持续静默后再 `SetupHooks`),
delay=0 冷启动 **45/45 无失败**。

## 复现命令

```
# 未修复复现需回退 fex_context.cpp 的守卫;当前构建默认已修复。
adb -s <serial> install -r app/build/outputs/apk/debug/app-debug.apk
adb -s <serial> shell am start -n com.shadps4.fexvalidation/.MainActivity
adb -s <serial> logcat -d -s FexValidation FexCore   # 看 va_bits 与 10/10 summary
# 需要调试器 attach 时:adb ... shell setprop debug.fexval.startdelay 15
```
