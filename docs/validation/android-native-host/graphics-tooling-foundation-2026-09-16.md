# Foundation main、GPU ring 与 RenderDoc / GPU Reshape 验证

2026-09-16，主仓 `codex/android-fex-round2` / `4da582b7` + 工作区修改。设备 AYN Thor
`9c2841a4`，API 33 / 4 KiB / Adreno 740，使用 Qualcomm 系统驱动 512.676.53。
本轮不做画面错误 A/B，也不把工具可用等同于画面正确或性能问题已经修复。

## 当前结论

- Foundation 已合并最新核对的 `origin/main` **7474e30**，本地 merge **bb52b26**。
  SDK 对齐 **9325b81**；原有 `modules/audio/` 和 `modules/CMakeLists.txt` 修改保留。
  merge 尚未推送，主仓引用和其他既有工作区修改均未提交。
- ring 确实包含 Vulkan timestamp：生产 Scheduler 探针 489 个配对区间，实际 APK
  7,421 个配对区间。修正并安装 litep MCP，让未精确校准的 GPU **时长**也能被标准查询读取。
- RenderDoc 新增 guest flip 边界，实际 APK 的新捕获返回 **Ready**，文件约 619 MB。
  更早的屋顶 RDC 已在同一 Android 系统驱动上回放，含 433 draw / 66 dispatch；当时
  coordinator 误报写入超时，已将捕获等待和写入 deadline 分开。
- GPU Reshape 在系统驱动下可连接、记录并真正替换探针 shader；13,059 项计算结果检查通过。
  **资源诊断仍有 overflow 和 token=0 的无法关联记录，尚不能用于确认游戏越界。**
- 屋顶重复抓帧仍遭低内存杀进程。此前约 42.9 ms 的 VideoOut label 复用等待仍未改语义，
  不宣称本轮已消除它。异步提交的独立结果见 [前一阶段报告](async-submission-2026-09-16.md)。

## Foundation 与 GPU 时间的唯一所有者

shadPS4 继续链接 `modules/profiler_ring`，由自己的 `vk_gpu_profiler` 持有 query pool、
Scheduler timeline tick 和回收协议。main 的完整 ProfilerModule / RingVulkanGpuBridge 已同步，
但不再启动第二个 GPU collector。`profiler_ring_gpu` 是本仓 `gpu_timing` 的别名。
共用契约见 [Foundation GPU timequery 指南](../../../foundation/docs/guides/litep-gpu-timequery.md)。

SDK 9325b81 修复 ring 窗口裁剪后的孤儿 GpuTime 污染复用 slot：没有匹配 begin 的时间样本
计入 `orphan_times` 并丢弃，不可与后续 slot 的新区间拼接。Foundation ring/capture smoke、
SDK GPU reader 11 项和真机 Scheduler 330 项通过；后者覆盖三代设备/query、录制切换和
跨 Flush marker 作用域。最终 marker 小修后再次运行仍为 **330/0**。

实际 APK `PID25941 / generation1 / UUID484bb36…` 的 ring：

| 指标 | 保留窗口的实测结果 |
|---|---:|
| GPU begin/end 完整配对 | 7,421 |
| 时间样本 | 15,655 |
| 安全丢弃的 orphan 时间样本 | 813 |
| reader skipped chunks / truncated | 0 / false |
| GPU.DrawBatch | 193 区间，平均 32.481 ms |
| GPU.GuestCommands | 193 区间，平均 32.123 ms |
| GPU.GuestRenderPass | 6,624 区间，平均约 0.528 ms |
| GPU.HostPrepare | 97 区间，平均约 0.719 ms |
| GPU.Present | 120 区间，平均约 0.445 ms |

这些是 GPU 时间戳区间，可能包含 GPU 等待；嵌套区间不能相加，也不是 guest 帧耗时。
请求 120 帧但高频 CPU ring 最终仅保留 13 个 FrameMark；17 条线程窗口未闭合 span 诊断保留。
不能声称无损 120 帧。当前估计时钟误差约 ±77 ms，不能做精确 CPU/GPU 先后或逐帧归因。

## litep MCP 的读取缺口已修复

原安装版不接受新的 SDK provenance；更新后又发现 shared `list_gpu_zones` 在未校准时
直接报告不支持，尽管原始 GPU records 完整存在。已在
`spatial_mcp_publish/dev_tools/mcp/litep/gpu_queries.py` 和 `facade.py` 补齐：

- 按 context/slot 解码，使用各 GPU context 的独立时间原点，支持配对区间汇总、时间线和分页。
- 未校准时明确标记 `cpuGpuAlignment=unknown`，拒绝 CPU frame/time filter；允许 GPU 内部时长分析。
- 保留 orphan、窗口边界、skipped/truncated 信息，不以零行伪装无 GPU 工作。

新增单测 4 项、facade 7 项、query 10 项、SDK GPU reader 11 项通过。已安装
`litep_mcp-0.2.0-local.20260916.gpuring2-osx-arm64`，更新 Codex 配置中的 litep 路径并重建
Doubao wrappers；11 个 wrapper 的 initialize/tools/list 通过。新进程实测标准查询分别返回
489 / 7,421 个区间。**已连接的旧 MCP 进程仍需重连**；未强杀其他会话的 MCP。

## RenderDoc：guest 提交边界与可复用标签

新增命令 `renderdoc_guest_capture 1`（范围 1–8）。开始和结束以连续真实 guest flip 为界，
排除 StatusLayer 旧帧重绘作为边界。只在捕获已 arm 时调用 `DrainSubmissions`，确认 worker
已调用实际 vkQueueSubmit，不等待 GPU 完成；未挂接日常路径不新增该 drain。

标签携带 `generation / guest_flip_id / submission_id / queue_id / packet_va`。
HLE 在复制 DCB 前保存 guest 源地址，避免把 host staging 地址当 guest 地址；未知源明确为 unknown。
compute 没有确定 guest 帧归属时 frame=0；跨 ring span 的拼接 packet 原始 VA 未保存时也标未知。
旧语义文字中的 `gfx:0xb400…` 仍是 host copy 位置，**真正 guest 地址只看 `packet_va` 字段**。
Scheduler 在物理 command buffer Flush 时关闭并在下一 buffer 重开逻辑 marker 栈，保持配平。

捕获等待预算仍为 5 s，写入/hash 使用独立 60 s 预算。此前 1.17 GB RDC 写入期间被旧总预算
误判失败；新增阻塞 EndCapture、写入超时和恢复测试，设备 coordinator **65/0**。
写文件 SHA256 沿用流式 64 KiB 读取，没有改成整文件入内存。

| 捕获 | 证据与接受范围 |
|---|---|
| 屋顶旧 deadline 捕获 | PID18891/gen1，flip1479→1480，1,167,676,491 bytes；SHA256 `4e1e908e4a1ecb1cdba9c16ed7ad34bb5701bc200904795911b2e8a32b8bd455`；旧 receipt Failed 保留，但实际文件完整且 Android remote replay 成功 |
| 修复后较小场景捕获 | PID7315/gen1/UUID `ae55df790741fe1ce0a4a42ce674bd57`，flip13507→13508，618,663,592 bytes；SHA256 `9ea641e2cdf82da907e6d1e21ff7a7cf2fa1c6a11a2c28253f2826eb79ab23cc`；receipt Ready/cleanup_pending=false，host pull SHA 一致 |

屋顶文件通过配对 RenderDoc MCP 在 Android 系统驱动回放一次后物化至 `guest-captures.rdc.db`：
1,635 events / 1,385 resources / 286 textures；433 draw、66 dispatch、34 copy、908 marker、
190 other、4 个 present 类型节点（其中一条是合成 End of Capture，实际 vkQueuePresentKHR 三条）。标签实例：event103 / DispatchDirect / flip1480 / submit4442 / queue0 /
`packet_va=0x238697128`。一个 guest flip 区间包含多个 host present，仍保持
`guest_frame_equivalence=unverified`，不能称其等于一张最终显示帧。

屋顶重复捕获在 RenderDoc+GRS 和 RenderDoc 单独启用时均遭 LMK。最后独立复现 PID3184
于 20:06:55 被杀，RSS 4,756,128 KiB，系统报告 critical memory pressure；无 footer 的文件
不进入通过样本。配对的 Android RenderDoc 源码基线 `6ae929af` 尚无 SoftMemoryLimit 选项，
不能通过调用新 header 的 option13 假装已减内存。另有一次 vkQueueSubmit 错误触发 assert；
现改为进入 submission worker 的异常/poison 路径，**驱动错误本身的原因未确定**。

## GPU Reshape：可用部分与真实缺口

沿用 Cemu Android SDK 的独立源码仓，不复制 SDK 进 Foundation：
`/Users/bytedance/workspace/gpu_reshape/GPU-Reshape`，基线 `0441175a` + 既有本地修改。
重新构建 `GRS.Backends.Vulkan.Android`，APK DSO SHA256
`1242e1b8ea282f072aa983cc7b56ce5bff40a1b7665620d6edc85cec1195b24f`。

普通 APK observe-only 模式在 Qualcomm 上产生 pipeline/shader/dispatch/present 记录，MCP
transport dropped=0。独立生产 Instance 探针等待 MCP 连接后启用资源 instrumentation：
实际替换 1 module，无 compiler/driver fallback，PRMT updates3/misses0，collector complete1，
13,059 项输出检查全通过。结束后保留 2 s drain，避免退出时传输仅留下初始化事件。

但该探针有 collector_overflows=1，收到 64 条 overflowed bounds 记录，token/type/puid=0，
资源尺寸和 descriptor 归属缺失。因此这些记录既不是 clean report，也不能当游戏 shader OOB
结论。shad 的 buffer_lifetime 仍为 `not_annotated`，Cemu 的新版 lifetime API 尚未迁移。
下一次推进应先修这一层资源身份/容量证据，再用选定游戏 shader 验证，避免盲目开启全 shader。

## 复现入口、产物与清理

```sh
adb -s 9c2841a4 shell dumpsys activity service com.shadps4.android/.service.FexSessionService profiler_ring_gpu status
adb -s 9c2841a4 shell dumpsys activity service com.shadps4.android/.service.FexSessionService profiler_ring_gpu detail
adb -s 9c2841a4 shell dumpsys activity service com.shadps4.android/.service.FexSessionService profiler_ring dump 120
adb -s 9c2841a4 shell dumpsys activity service com.shadps4.android/.service.FexSessionService renderdoc_guest_capture 1
adb -s 9c2841a4 shell dumpsys activity service com.shadps4.android/.service.FexSessionService renderdoc_capture_status 1
```

RenderDoc 必须在冷启动前配置配对 layer app；停止 app 后再进行 Android remote replay，不并发跑游戏、
独立 GPU 探针和 replay。GPU Reshape 默认关闭，仅诊断窗口按需开启。

原始大文件留在 `build/graphics-tooling-20260916/`；可入库的小型记录在
[证据目录](graphics-tooling-20260916/README.md)，包括精确源码/产物身份、测试、捕获 receipt、
MCP 查询、失败退出和 cleanup。捕获 APK 为 `2126d946…`，host `232c80bf…`；其后的唯一 native
小修是 compute split packet 地址标 unknown，已完成 host/APK 编译与330/0定向测试，尚未重新安装；
设备仍为2126d946，不能冒称旧捕获来自新构建。

**APK variant 是 playstoreDebug；host/JNI 是 RelWithDebInfo（-O2 -g -DNDEBUG），FEXCore 是 Release。**
实际 Ready 后正常 UI Stop 在 0.81 s 完成 Stopped/user_stop。Android layer 设置与 GRS 属性已恢复
本轮开始的空值；owned GRS MCP bridge 已关闭，无自动输入/调试器/活跃捕获。保留系统驱动和 async
配置。未进行全量回归、画面 A/B、新的 FPS 收益验收或主仓 commit/push。

用户随后要求停止新增抓帧。后续已仅基于现有屋顶文件完成 native shader / resource usage /
GPU Duration 分析，见 [已有帧分析与归因修正](existing-rooftop-gpu-analysis-2026-09-16.md)。
