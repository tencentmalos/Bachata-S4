# DiagnosticsHub command registry (Work Package A, step 5)

2026-09-14，主仓 `codex/android-fex-round2`,实施 [图形调试工具整包 spec](../../specs/android-graphics-debugging-toolkit.md) §3.1 的命令入口。Android `Service.dump`(经 DumpsysBridge)与后续 ImGui 面板都走同一个 registry,即「命令内部共用类型化后端,ImGui 只调用相同后端」。

## 交付

新增 `src/core/diagnostics/diagnostics_commands.{h,cpp}`:`RegisterDiagnosticsCommands(registry, hub, clock)` 把 §3.1 的命令注册进**复用的 Foundation `spatial::debugbus::DebugCommandRegistry`**(不另造 litep 私有协议,不新建并行 registry)。clock 以 `std::function` 注入,测试确定、无隐藏时钟依赖。

**已有后端、真实实现的命令:**
- `debug_status` —— 调 `DiagnosticsHub::QuerySnapshot`(无锁),格式化 session/generation/pid/phase/run_uuid/stage/stop_reason + 七个推进信号。**UNAVAILABLE 与 0 分开渲染**:`gpu_retire: unavailable` vs `guest_flip: 0 (never)`。
- `renderdoc_status` —— `VideoCore::IsRenderDocLoaded()`(即上文 loader 修复的成果),并明示 `capture_backend: not-implemented`。
- `overlay status` —— hub 的 overlay_redraw 计数(overlay 本体是后续 Layer 步)。

**已注册但后端未接、统一返回诚实 `not-implemented` 的命令**(§3.1 禁止伪造成功/receipt):`renderdoc_capture[_status|_cancel]`、`guest_screenshot[_status]`、`profiler_ring`、`profiler_capture`、`performance_capture[_status|_cancel]`、`guest_command_trace`、`gpu_command_trace`、`overlay show|hide`。命令名与 help 稳定,后端落地时替换 handler。

## 验证

新增 `tests/host_runtime/diagnostics_commands_tests.cpp`(41 checks),注册进 CMake `HOST_BUILD_PROBES`,链接 `shadps4_host` + `spatial::foundation_debugbus`;`diagnostics_commands.cpp` 加入 `if(BUILD_HOST_CORE)` 的 `target_sources`(经 `shadps4::foundation` 传递 debugbus include)。

- 本机 `clang++ -std=c++20 -Wall -Wextra`,链接真实 `DebugCommandRegistry.cpp`(`IsRenderDocLoaded` 用最小 stub 避免拖入整个 video_core TU):**41 checks / 0 failures**,run exit 0。
- 覆盖:无 session 的 `debug_status`;active session 下 count、`gpu_retire: unavailable`(非 0)、`guest_flip: 0 (never)`;`renderdoc_status`;`overlay status`/`overlay show`;7 个 pending 命令均 `status: not-implemented` 且不含 `ready`;`HelpText` 列出真实命令;未知命令由 registry 优雅处理。
- clangd 0 diagnostics;CMake if/endif 59/59。

## 边界

- registry 尚未在 app init 注册到 `SetDumpsysRegistry`;真正的 `Service.dump` 接线、异步 request/capture ID 与 armed/capturing/writing/ready/cancelled/failed 状态机、忙时 `busy/retryable` 属于后续增量(需要各能力后端)。
- producer 仍未接:runtime 尚未向 hub 推进计数,故 `debug_status` 在真实运行中目前会显示计数为 0/never——这是接线待办,不是最终语义。
- 未改 FEX/Foundation/Citron。
