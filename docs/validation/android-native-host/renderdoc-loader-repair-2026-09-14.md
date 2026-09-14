# RenderDoc loader repair (Work Package A, step 1)

2026-09-14，主仓 `codex/android-fex-round2`，实施 [图形调试工具整包 spec](../../specs/android-graphics-debugging-toolkit.md) §3.2 的第一条明确指令「先修当前 loader」。这是 Opus 4.8 实施轮的第一个落地增量,单文件、依赖已在树内。**不是抓帧成功证据,也不代表 RenderDoc 完整路径(捕获 coordinator、Session receipt、remote replay)已接入。**

## 修复的确证缺陷

审核 [graphics-debug-tooling-audit-2026-09-14.md](graphics-debug-tooling-audit-2026-09-14.md) 记录的 loader 三个问题,均在源码中确认并修复:

1. **NOLOAD 命中后不取 GetAPI(POSIX 路径)。** `src/video_core/renderdoc.cpp` 原逻辑:`dlopen(RENDERDOC_LIB, RTLD_NOW|RTLD_NOLOAD)` 成功(RenderDoc layer 已注入——Android 抓帧的正常情形)时,`mod` 非空,导致整个 `if (!mod && enabled)` 块被跳过,`RENDERDOC_GetAPI` **从不被调用**,`rdoc_api` 保持 null,之后每次 `StartCapture`/`EndCapture` 静默 no-op。现抽出 `ResolveRenderDocApi(mod)`,对**任何**取得的 handle(resident 或 offline load)都执行握手。

2. **握手失败 `ASSERT(ret==1)` 直接 abort 进程。** 用户设备上一次良性的 API 版本不匹配会变成硬崩溃。现改为失败安全:缺 `RENDERDOC_GetAPI` 符号或 `GetAPI(1.6.0)` 返回非 1,记录 `LOG_ERROR` 并让 `rdoc_api` 保持 null,不中止。

3. **`capture_state` 跨线程数据竞争。** 原为普通 `static CaptureState`,却由 input/UI 线程(`TriggerCapture`,[sdl_window.cpp:328](../../../src/sdl_window.cpp))与 GPU 线程(`StartCapture`/`EndCapture`,[liverpool.cpp:145,181](../../../src/video_core/amdgpu/liverpool.cpp))并发读写(UB)。现改为 `std::atomic<CaptureState>`,三个转移均用 `compare_exchange_strong`:Idle→Triggered、Triggered→InProgress、InProgress→Idle。每个 armed 请求的 frame-capture 调用恰好触发一次,即使两线程竞争。

附带修正:原 Windows 分支用无前缀 `#ifdef WIN32`,与文件其余处及顶部 include 块统一使用的 `_WIN32` 不一致(MSVC/clang-cl 定义 `_WIN32` 而非裸 `WIN32`),该离线加载路径此前很可能是死代码。现全文件统一 `_WIN32`。移除不再使用的 `common/assert.h` include。

## 验证

- clangd 完整解析该 TU(15 个函数结构正确),`lsp_diagnostics` 0 error/warning。
- 独立语法编译:以真实 `externals/renderdoc/renderdoc_app.h` ABI + 桩接内部头(types/logging/settings),`clang++ -std=c++20 -fsyntax-only -Wall`:
  - `-DANDROID`(POSIX/dlopen 路径):exit 0
  - 通用 POSIX(`librenderdoc.so` 路径):exit 0
  - `_WIN32` 分支在 macOS 无法编译;它是 POSIX 分支经同一 `ResolveRenderDocApi` 的机械镜像,并修正了 `WIN32`/`_WIN32` 不一致。
- 未跑完整 Android host/APK 链接或真机抓帧——那是后续 capture coordinator/Session receipt 增量的验收,不在本步。

## 边界(未做,不得据此声称完成)

- 未实现 §3.2 的统一捕获 coordinator、显式 device/window(`RENDERDOC_DEVICEPOINTER_FROM_VKINSTANCE`)、flip/present 边界对齐、Session 绑定生命周期、capture receipt(文件/hash/UUID)、remote replay。
- 未验证 Vulkan interception 实际生效(API 可见 ≠ layer 注入生效)。
- `capture_launch` MCP 能力此前为 ABI unsupported;app-owned capture + 配对 helper 仍待实现。
- 未改 FEX/Foundation/Citron,未装 RenderDoc Server,未启用采集。
