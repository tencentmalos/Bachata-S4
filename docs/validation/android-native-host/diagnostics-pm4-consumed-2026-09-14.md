# DiagnosticsHub pm4_consumed producer + desktop-portable hub(Work Package A, step 11)

2026-09-14，主仓 `codex/android-fex-round2`。接入 pm4_consumed producer 并把 hub 核心下沉到跨 desktop/host 的 CORE 源列表;真机测试暴露并修复了两个真实问题。

## 交付

**hub 核心跨平台化(CMake)**:`diagnostics_hub.{h,cpp}`、`diagnostics_hub_registry.{h,cpp}`、`trace_identity.h` 从 `BUILD_HOST_CORE`-only 的 `target_sources` 移入始终构建的 `CORE` 源列表(desktop 与 host 共用)。原因:`liverpool.cpp` 属 `VIDEO_CORE`,desktop+host 都编;若 hub 仅 host 有,desktop 链接会失败。diagnostics 无 Android-only 依赖(`spatial/debugbus` 与 `video_core/renderdoc.h` desktop 也有),下沉安全。`diagnostics_commands.cpp`/`diagnostics_service.cpp`(命令/JNI 面)保持 host-only。

**pm4_consumed producer**(liverpool.cpp):在 `Liverpool::Process` 的 gfx/compute 提交任务 drain 处(`--num_submits`,`task.done()` 后)`Advance(Pm4Consumed)`。desktop 无 hub session 时 no-op。

## 真机测试暴露并修复的两个问题

1. **pm4_consumed 接错路径**:初版把 Advance 放在 `ProcessCommands()`(control `command_queue` 路径)。真机测 `pm4=0`——gfx 提交走 `mapped_queues[].submits` fiber task 路径(`SubmitGfx`→`++num_submits`→`task.resume()`→`--num_submits`),不经 `command_queue`。移到真正的 submit-task drain 处后 `pm4=8`。
2. **present 计数采样竞争**:flip HLE 发布 `host_present` 用 flip 时刻的 `guest_presents`,但 present 在 GPU 线程异步 flip 之后才完成,故 flip-HLE 时刻可能仍 0;快速合成 run 又可能在两次轮询间返回。修复:终止后再读一次 `debug_status`(hub 保留计数至 Destroy 的 Revoke),host_present 断言接受 hub live 计数或 terminal detail 的 guest_presents 任一。

## 真机证据(AYN Thor API33)

`graphicsProducersAdvanceInRenderingSession` 连跑 3 次 **BUILD SUCCESSFUL**,两测一起也 PASS。实机 logcat:`gpu-flip flip=4 submit=4 present=3 pm4=8`——四个图形 producer(guest_flip/queue_submit/host_present/pm4_consumed)在真实渲染 session 中全部非 0。

## 边界

- host_present 从 flip HLE 发布,滞后于异步 GPU present 完成一拍;终止后读取或 terminal detail 是权威值。逐 present 完成回调实时发布是可选后续优化。
- phase 仍为 0(SessionCore 拥有,未接;stage 字符串承载真实标签)。overlay_redraw 需 ImGui overlay(§3.4)才非 0。
- desktop 构建本机未跑(无 desktop 构建环境);CMake 下沉后 host 重链 HOST_LINK_PASS,liverpool 真实 NDK 编译 exit 0。未改 FEX/Foundation/Citron。
