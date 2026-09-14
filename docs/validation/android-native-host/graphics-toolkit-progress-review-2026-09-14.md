# 图形调试工具进展复核：30e3b21f

审核范围：`ddf02b5e..30e3b21f`，14 个提交、44 个文件。结论：**控制入口与部分 producer 已落地，但“§3.1、§3.2 完成且真机验证”不成立。§3.2 当前仍是未完成生产闭环的框架，并存在需要先修复的功能错误。** 原 [整包 spec](../../specs/android-graphics-debugging-toolkit.md) 继续有效，不另拆微型 spec。auto tag 仍延期。

## 实际完成程度

| 范围 | 可承认的进展 | 尚未满足 |
| --- | --- | --- |
| §3.1 | Foundation registry、app JNI/Service.dump 路由、hub/publisher、guest GNM/部分 PM4/draw 计数入口；已有普通 APK 合成测试 | 正确的 Vulkan submit 与实时 present 计数、Preparing/Running/Stopping/fault/Stop 来源、build/driver 身份、未实现信号的 unavailable、调试构建门控及忙时响应 |
| §3.2 | NOLOAD 分支 GetAPI/空符号/API 失败处理修复；backend 可注入；命令与 coordinator 接线 | 单帧区间、timeout/cancel/Stop 所有权、非阻塞状态、实际 Android API discovery/Turnip interception、显式实例、RDC/sidecar、remote replay、截图 receipt |
| §3.3 / §3.4 | 有既有 Foundation/Citron 来源可复用 | PROF/litep、ImGui Layer 尚未实施；没有新的实测依赖失败证据 |
| WP-B | `trace_identity.h` 定义部分字段与序列化 | 原始 PM4、action trace、实际提交身份传递、decoder、marker 关联和证据包均未实施；类型头不能证明身份链已经固定正确 |

正向变化应保留：NOLOAD handshake 修复、不把缺失 RenderDoc 伪报成功、GPU retire 显示 unavailable、撤销时校验 publisher generation、host DSO/JNI 共享 hub、真实 Draw/Dispatch 点的计数接线。无需回退整个批次。

## P1：必须先关闭的问题

### F1. 默认单帧请求在同一边界 Start/End，没有覆盖下一帧

位置：[renderdoc_capture.cpp](../../../src/video_core/renderdoc_capture.cpp) `OnFrameBoundary`，60–80 行；[vk_presenter.cpp](../../../src/video_core/renderer_vulkan/vk_presenter.cpp) 1120 行。

从 Armed 进入 Capturing 后直接 fall through，`frames_seen_` 当场从 0 变 1。默认 `frames=1` 随即 End；调用点已在该次 `scheduler.Flush` 和 swapchain present 之后。因而同线程顺序场景中没有任何目标帧 Vulkan 工作落在 Start/End 之间，多帧则少一个完整间隔；并发线程偶然提交也不能证明属于目标帧。

反例 R1：present 前模拟记录工作，回调后 receipt=Ready，但 captured_work=0。现有单测第 60–63 行反而断言“一次边界 starts==1 && ends==1”，把错误实现写成正确预期。

修复不能只加一个 return 就宣布完整帧：首边界仅启动；此后的完整边界才计数，同时携带 accepted flip / submission / host present 身份，排除 `Present(frame,true)` 重画及 blank frame。与异步 compute、已排队下一帧、Presenter 合成建立明确覆盖范围。超出完整帧能力的捕获必须标 partial/time-window。

### F2. 超时只改状态，backend 继续捕获；取消也没有 discard

位置：[renderdoc_capture.cpp](../../../src/video_core/renderdoc_capture.cpp) 106–138 行及 60 行。

- `CheckTimeoutLocked` 只把 Capturing 改成 Failed，不关闭 backend；之后 `Cancel()` 因状态已 Failed 而跳过清理，下一次 Arm 又被允许。R4/R5 复现超时后仍 active，以及在旧 capture 未释放时再次 Start。
- `OnFrameBoundary` 实际没有调用超时检查；不查询状态时，迟到边界仍 Start/Ready。R3 复现 arm=0、timeout=10、boundary=100 仍 Ready，与执行反馈“OnFrameBoundary 也检查超时”不符。
- `Cancel()` 调用的是 `EndFrameCapture`，不是 `DiscardFrameCapture`，会走正常写文件路径。R6 复现 Cancelled 同时产生 capture。仓库自己的 [RenderDoc API](../../../externals/renderdoc/renderdoc_app.h) 540–550 行明确区分 End 与 Discard。

统一 owner 在期限、Cancel、Stop、surface/device 失效时清理。清理失败保留占用和错误，不许先变终态再重入；区分请求期限和无推进超时，使用真实后台期限处理而非依赖用户不断 Query。backend 接口须暴露捕获状态和 discard 结果。

### F3. 所谓非阻塞 Query 实际等待 RenderDoc End；Cancel 同步跑 backend

位置：[renderdoc_capture.cpp](../../../src/video_core/renderdoc_capture.cpp) 61–80、107–110、130–138 行；[diagnostics_commands.cpp](../../../src/core/diagnostics/diagnostics_commands.cpp) 149–165 行；Service.dump 同步调用该 JNI。

OnFrameBoundary 持 coordinator mutex 调 End/GetCapture，Query 获取同一 mutex。RenderDoc 写 RDC、等待 GPU 或卡住时，状态查询一并卡住。Cancel 命令直接在调用线程执行 End，亦非入队。Presenter 调用还处在 `VideoOutDriver::Flip` 的 lifecycle_mutex 内，不能通过状态锁把导出等待传播到控制面。

R7 用有握手的 fake End 暂停 backend，确认 Query 在释放 End 前不能返回；这是锁依赖反例，不是实测 RenderDoc 性能。必须由单一 GPU/捕获 owner 执行实际 API，控制线程只发有界命令和读取不可阻塞快照；导出/写侧车有独立状态。若 API 本身需要同步完成，要如实隔离并报告，不能让 status/cancel 等它。

### F4. 捕获不属于 Session，旧请求可操作新会话

位置：[diagnostics_commands.cpp](../../../src/core/diagnostics/diagnostics_commands.cpp) 138–165 行；[renderdoc_capture.h](../../../src/video_core/renderdoc_capture.h) receipt/coordinator 接口；[session_backend_fex.cpp](../../../src/core/host_runtime/session_backend_fex.cpp) 393–403 行。

进程单例无 session generation、实例或窗口身份；Destroy 只撤销 hub，不撤销 capture。新的 present 可以推进旧请求。Cancel 忽略全部参数，旧 `cancel <request_id>` 会取消当前新请求。每个请求的 capture_uuid 都是固定 `run_uuid + ":cap"`；同 session 连续采集共用一个 UUID。无 session 也可 Arm。

R8/R9/R10/R12 分别复现以上行为；跨 generation 反例按生产已有的 hub Revoke/Register 流程模拟，不冒充真机跨 Session 捕获验证。需把 run UUID 固定为进程启动身份、session generation 独立、capture UUID 每次唯一；request ID + generation 校验所有变更。Stop 必须在 Vulkan/窗口销毁前终止或拒绝遗留 capture。另有旧 `TriggerCapture/StartCapture/EndCapture` 状态机仍在 Liverpool 使用，**尚未被新 coordinator 替换**；两个入口需归同一 owner。

### F5. Android 从未调用 loader，真实抓帧仍无可达入口

位置：[renderdoc.cpp](../../../src/video_core/renderdoc.cpp) 67–121、176–194 行；全仓调用搜索仅找到 [emulator.cpp](../../../src/emulator.cpp) 557 行的 desktop `LoadRenderDoc()`。

Production Session/JNI 没有 discovery 调用；`rdoc_api` 初值为空，即便 Android layer 已注入，该指针也不会自行赋值。当前设备失败回执因此最多证明“应用 API 指针为空”，不能独立证明 layer 确实未注入。loader 补丁有价值，但仍未接入本轮目标。

明确在实际 Android driver/instance 生命周期中接入 discovery 与 layer 配置，保留 Turnip dispatch/namespace 和库寿命；使用正确 VkInstance device pointer、实例/窗口选择以及 API 状态验证。当前真实 build 有 `-DANDROID`，故不能把这次问题错误归因于 Android 宏未定义。取得有 draw/dispatch 的普通 APK synthetic RDC 并在配对 Android/Turnip replay，再抓一份 TMNT。GetCapture count/path 不代替文件/sidecar/hash和覆盖完整性（R2 仅证明 coordinator 没做这些检查）。

### F6. 关键推进信号归属错误，会把 host 未执行误读为 GPU 不推进

位置：[guest_graphics_hle.cpp](../../../src/core/host_runtime/guest_graphics_hle.cpp) 311–333 行；[vk_scheduler.cpp](../../../src/video_core/renderer_vulkan/vk_scheduler.cpp) 193 行；[driver.cpp](../../../src/core/libraries/videoout/driver.cpp) 279 行。

`QueueSubmit` 在 `sceGnmSubmit*` 接受 guest 队列时累加，尚未到实际 Vulkan queue.submit。一个 GNM 提交可以对应多个 host submit，接受成功时 GPU 线程甚至还未执行；这个数不能用来判断 host 是否提交给驱动。应另命名 guest submission，真正 queue submit 在 Scheduler 的实际成功点发布，保留 queue/submit 身份。

`HostPresent` 只在下一次 GNM submit+flip 和 Run 返回时同步；实际异步 present 发生时没有发布。最后一帧已呈现而 guest 随后一直等待时，hub 可一直显示 0/旧值，last_advance_ns 也是读取时刻而非呈现时刻。3 秒 settle 无法制造缺失的 producer。应在实际 present 成功点直接发布，并区分 guest present 和 overlay/复用重画。

现有 XML 所对应的实机摘要是 `flip=4 submit=4 present=3 pm4=8 draw=0`，证明 guest 入队及部分计数可见，不能证明 host submit 正确。`pm4_consumed` 当前计的是完成的任务数，并在 promise.error 后仍计；不是 packet 数或部分推进。需要明确单位，且不能把故障任务当完整消费。生产 convenience `hub.Advance()` 只选当前 active publisher，后续异步 trace 应携带 generation lease，不要以当前全局 publisher 推定来源。

## P2：控制状态与测试还需补齐

1. **phase/Stop 原因没有生产写入。** `SetPhase` / `SetStopReason` 仅在声明、实现和单测出现。现有真机 RUNNING 日志仍 `phase: 0`（Idle）。hub 在 production Prepare 完成后才注册，故卡在 Prepare 时会报 none；Run 各种结局都标 returned，Destroy 丢掉最后状态。将 SessionCore 的 Preparing/Running/Stopping/terminal、发起者/错误/期限发布到独立快照，保留最近终态。不要让 status 反调 Session 的阻塞查询。
2. **未知信号和时间格式。** 未实现 overlay_redraw 默认 available=true、输出0；`FormatCounter` 把绝对 steady_clock 时间写成 `last +...ns`，不是“距今多久”。未接测量输出 unavailable；显示真实时间域及 age。
3. **参数不严格。** `strtoul` 不检查结束符/errno/范围，无 frames 上限；`-1` 变 4294967295（R11），垃圾输入也变合法单帧。拒绝非法/多余参数，限定帧数/时间/字节预算；busy 要显式返回，不要仅把旧 Armed receipt 当新请求结果。生产 debug 命令目前也没有 debug 构建门控；保持应用 Service 的现有非导出属性，并在正确构建层明确启用条件。
4. **测试吞掉实际语义。** `DiagnosticsInstrumentedTest` 的 `signal()` 只匹配冒号，terminal detail 实际是 `guest_presents=4`，因此已有日志 `detailPresents=0`；测试的 OR 回退既没生效，也不应替代 hub 实时 producer 断言。为真实 draw/dispatch 加正例；phase、最后一帧后 guest 持续等待、Stop/fault、严格取消归属均应有针对性断言。
5. **证据身份未闭环。** 本批文档没有完整归档所测 APK/host/JNI Build IDs 与 source manifest。此次能核对本机现存 XML/log，但不能把“有构建文件”反推为确定部署身份；后续采集一起补齐。不要改写旧记录填入当前 HEAD。

## 复核验证与范围

本次重编译原六组诊断 native 测试：**173 checks / 0 failures**（27+28+32+34+44+8）。随后针对原生产 coordinator/command 源码注入 backend，**12 个反例全部复现**，见 [输出](2026-09-14-graphics-toolkit-review/host-results.txt)、[代码](2026-09-14-graphics-toolkit-review/reproduce.cpp) 和 [运行脚本](2026-09-14-graphics-toolkit-review/run.py)。这些是分组问题的反例场景，不代表12个独立真机故障，更不是12项功能通过。原单测全绿不能覆盖遗漏的契约。

复现命令（仓库根目录）:

```sh
python3 docs/validation/android-native-host/2026-09-14-graphics-toolkit-review/run.py
```

脚本输出到 `build/graphics-toolkit-review`，不连接设备。修复后部分 REPRODUCED 应消失，此审核脚本的非零出口不等于新实现回归；把对应正确契约转成正式测试。

另核对并保存原有 [JUnit XML](2026-09-14-graphics-toolkit-review/existing-device-results.xml)（2/0）和 [仅本 app 的诊断摘录](2026-09-14-graphics-toolkit-review/existing-device-diagnostics-excerpt.txt)。所审源文件 hash 与范围见 [manifest](2026-09-14-graphics-toolkit-review/manifest.json)。**本次没有安装 APK、启动游戏、注入 layer、抓 RDC、replay 或运行完整回归。** 没有修改生产代码或子仓。

## 后续推进方式

继续同一个整包，先把 **F1–F6 和控制状态一起修到一份真实可 replay 的 synthetic RDC**：generation owner/取消/timeout/非阻塞状态、真实 Vulkan/present producer、Android discovery 与捕获链路共同验收。以修改点负例和一次实际抓帧验证；不要又用 absent 分支把 §3.2 关闭。

随后在同一工作包内继续 Foundation PROF/litep 和 ImGui Layer，再完成 PM4/action/关联包。可以先设计跨提交 envelope，但不能把一个 header 当作身份传递已完成，或以“WP-B 更容易单测”长期绕过实际抓帧、性能和 Layer。

- **Foundation 是已有授权范围内的实现工作。** 当前 owned 分支干净；参考 Foundation 和 profiler SDK 本机可读，尚无本轮实际访问失败证据。选择性迁移、保持独立 SDK 来源、先推 child 再推 parent 正是原 spec 的要求，不是新的许可障碍。若真实遇到访问/构建失败，给出错误与最小阻塞证据。
- **ImGui 不必先做全 UI 版本迁移。** 优先按原 spec 注入单一 external ImGui target、兼容 Layer/backend；主仓原有游戏 `ImGui::Image` 合成保留。临时 NDK 命令漏 imgui_config 引起 ImVec2 报错不能证明版本迁移被阻塞；真实 build 通过已经排除了该条 ad-hoc 编译论据。
- 只有真实出现设备/配对 layer/replay 依赖阻塞，才记录明确阻塞并并行推进独立部分；不可把未尝试的常规子仓修改归为不可执行，再更换交付目标。

本报告修正的是完成度和实现质量。下一位实施者应修复这些问题并继续原整包，不重新开始已有 JNI/host/input/runtime，也不恢复 auto tag。
