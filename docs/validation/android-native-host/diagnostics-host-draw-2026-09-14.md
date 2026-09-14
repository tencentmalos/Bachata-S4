# DiagnosticsHub host_draw producer + present settle(Work Package A, step 12)

2026-09-14，主仓 `codex/android-fex-round2`。接入 host_draw producer,完成七个推进信号中所有当前可测的 producer;真机测试再次暴露真实语义并修正。

## 交付

**host_draw producer**(vk_rasterizer.cpp):在 5 个真实已记录 draw/dispatch 处(3× `DebugState.IncDrawCall()` at Draw/DrawIndirect,2× `DebugState.IncDispatch()` at DispatchDirect/DispatchIndirect)`Advance(HostDraw)`。仅在实际 `cmdbuf.draw*`/dispatch 记录后计,filter/无 pipeline 早退不计。desktop 无 hub session 时 no-op(hub 已在 CORE,desktop 可用)。

## 真机语义澄清

`graphicsProducersAdvanceInRenderingSession` 测 gpu-flip 合成得 `draw=0`。核实非 bug:`runtime_gpu_flip.S` 的 DCB 仅一条 `PrepareFlip`(0xc03e1000),**不含 draw packet**,故 `Rasterizer::Draw` 从不被调。当前无合成 fixture 发真实 draw packet。host_draw producer 位置正确(NDK 编译验证 + 位于真实 IncDrawCall/IncDispatch 处),只是 flip-only fixture 不触发。测试改为**断言 `host_draw==0` 并注释说明**,不伪装成非 0。

**present 竞争修正**:host_present 由异步 GPU present 发布,可能在 guest 返回后才落地;快速合成 run 的轮询/终止会错过。加 3s settle 窗口(session 到 Destroy 才 Revoke,终止后仍有窗口)。两测连跑 2 次 BUILD SUCCESSFUL,present=2/3。

## 真机证据(AYN Thor API33)

`gpu-flip flip=4 submit=4 present=3 pm4=8 draw=0`——guest_flip/queue_submit/host_present/pm4_consumed 四个 producer 真实渲染 session 非 0;host_draw=0 是 flip-only fixture 的正确行为。两 DiagnosticsInstrumentedTest 用例(debugStatus + graphicsProducers)一起 PASS。

## 边界

- host_draw producer 已接但当前合成 fixture 不发 draw packet,故真机未见其非 0;需一个发 DRAW_INDEX 的 fixture 或真实游戏才能观测。已如实断言=0。
- overlay_redraw 需 §3.4 ImGui overlay;gpu_retire 标 UNAVAILABLE(无 timestamp query 后端);phase 未接(SessionCore 拥有)。
- 未做完整 desktop 构建(本机无环境);host 重链 HOST_LINK_PASS,vk_rasterizer 真实 NDK 编译 exit 0。未改 FEX/Foundation/Citron。
