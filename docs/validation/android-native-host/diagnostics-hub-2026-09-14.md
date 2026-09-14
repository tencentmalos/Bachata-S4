# DiagnosticsHub published snapshot (Work Package A, step 3)

2026-09-14，主仓 `codex/android-fex-round2`,实施 [图形调试工具整包 spec](../../specs/android-graphics-debugging-toolkit.md) §3.1 的状态发布层。这是 DiagnosticsHub 的**状态半边**;命令 registry(`debug_status`/`renderdoc_*`/`profiler_*` …)是后续独立增量,会读同一个 hub,使 ImGui 与 adb 共用一套类型化后端。**不接入 producer、不产生 trace、不代表任何工具可用。**

## 为什么需要

`GuestRuntime::Diagnostics()`([guest_runtime.cpp:2749](../../../src/core/host_runtime/guest_runtime.cpp))取 `impl->threads_mutex` 与 `impl->graphics_mutex`。从 Android `Service.dump`(主线程)调用它,会让一次状态查询阻塞在 Session mutex / VM drain / GPU fence 后,正是 §3.1 禁止的、直通 ANR 的路径。

## 交付

新增 `src/core/diagnostics/diagnostics_hub.{h,cpp}`(namespace `Core::Diagnostics`):

- **`DiagnosticsPublisher`** —— session generation 写入的发布器。数值字段是无锁原子;少量身份短字符串由一个**专用 mutex** 保护,该锁**只**用于字符串拷贝,绝不跨 Session mutex / VM drain / GPU fence / 导出,且 hub 持锁期间从不回调 runtime,故不可能与 runtime 锁死锁。
- **`DiagnosticsSnapshot`** —— 返回给 reader 的纯值快照(无原子、无指针),可安全跨线程拷贝/格式化。
- **`AdvanceSignal` / `AdvanceCounter`** —— §3.1 的七个推进信号分开计数:guest_flip、pm4_consumed、host_draw、queue_submit、gpu_retire、host_present、overlay_redraw,各带独立 last-advance 时间戳。

强制两条 §3.1 规则:

1. **读快照绝不阻塞 runtime 工作。** `CopyInto` 数值字段从原子拷贝,字符串在专用微秒级锁下拷贝,不碰任何 guest 指针。
2. **不可测的信号是 UNAVAILABLE,绝非 0。** `available=false`(如无 timestamp 支持的 GPU retire)与 `count=0 && available=true`(可测且确未推进)是两个不同结论,由类型区分。

`Advance` 记增量+时间戳;`PublishCount` 发布绝对值(镜像既有原子如 `VideoOutDriver::guest_presents`,[driver.h:88](../../../src/core/libraries/videoout/driver.h)),仅在前进时更新时间戳。

## 验证

新增 `tests/host_runtime/diagnostics_hub_tests.cpp`(28 checks),注册进 CMake `HOST_BUILD_PROBES`(`.cpp` 编入 host DSO,test 链接 `shadps4_host`)。`diagnostics_hub.cpp` 加入 `if(BUILD_HOST_CORE)` 的 `target_sources`。

- 本机 macOS 直编直跑 `clang++ -std=c++20 -Wall -Wextra`:**28 checks / 0 failures**,三次运行确定性一致。
- 覆盖:空发布器(no session);identity/phase/strings;unavailable≠0;`Advance` 增量+时间戳;`PublishCount` 绝对值 + 仅前进时更新时间戳;**并发**:8 线程 × 20000 次 `Advance` 与一个持续 reader 同时跑,最终计数精确 = 160000(无丢失),reader 完成 15330 次无锁读且始终看到一致 generation。
- clangd:`.cpp` 0 diagnostics;header 完整结构解析。
- **Sanitizer 环境限制(非本代码缺陷)**:本机 Apple clang 17 的 TSan 对一个 trivial 单线程 `atomic.fetch_add` 都崩溃(exit 139),ASan run 挂起;已用 trivial 探针确认是工具链环境问题,非 hub 代码。正确性由「无丢失计数的并发测试 + 各字段独立原子 + 字符串专用锁只用于拷贝」保证。CMake if/endif 平衡未变(59/59)。

## 边界

- 未接任何 producer:runtime 尚未 store 这些计数,`Service.dump` 尚未读 hub。producer 接线(在真实 present/submit/PM4 消费点调用 `Advance`/`PublishCount`)、命令 registry、每 generation 的可撤销注册与忙时 `busy/retryable` 语义是后续增量。
- `diagnostics_hub.cpp` 目前只编入 host DSO(`BUILD_HOST_CORE`);当后续把 hub 接入桌面也构建的共享 Presenter/Liverpool 时需并入桌面构建。当前无桌面引用,桌面不受影响。
- 未改 FEX/Foundation/Citron。
