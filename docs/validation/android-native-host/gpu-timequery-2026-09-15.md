# GPU 异步边界、litep timequery 与系统驱动对照

2026-09-15。本轮接在主仓 `4da582b7` 的现有大量 dirty work 上；不是一个干净提交的性能对照。
保留原有 FEX、音频和其他修改，仅做相关验证，没有完整回归或新 spec。
最终设备 AYN Thor `9c2841a4` / API33 / ARM64 / 4 KiB；Swan 未触碰。

## 当前可见结果

最终 APK `2ddba5ca…`、host Build ID `6e6b8a2ffd9880a2b576b28098eff70aa8f4f490`。
PID17372 / generation1 / UUID `dc3add49f59e4b67716301959ec74d43` 通过真实游戏屋顶
MOVE → 摇杆移动/镜头跟随 → ATTACK；warmup 正式退出 0 / GAMEPLAY_REVIEWED。
最后保留游戏运行供用户查看，自动输入已关闭，ring 和粗粒度 GPU query 开启，file capture 已停止，
本轮没有挂 debugger。截图：[实际游戏与曲线](gpu-timequery-20260915/final-screen.png)。

StatusLayer 展示 FPS、240 帧 CPU 呈现间隔曲线、120 帧 GPU guest 区间曲线、prepare/present/redraw、
query pending/drop/error 以及 CPU/GPU 校准误差。复用上一画面的 overlay redraw 不计新 guest 帧；
完整 guest GPU frame 超过 1 秒未更新时显示 stale/partial，避免把旧数据当成当前 GPU 活动。

| 同一 PID/gen 普通 APK | FPS | GPU.GuestFrame 均值 | 结果 |
| --- | ---: | ---: | --- |
| 首次 coarse 20 秒采集 | 13.086 | 65.527 ms | CPU/计数器可用；GPU 只解出 draw/prepare，两个 context 定义缺失 |
| detail 15 秒采集 | 13.238 | 65.465 ms | 三个 GPU context，8,595 个 render-pass 区间 |
| 恢复 coarse 20 秒采集 | 13.098 | 66.045 ms | 三个 GPU context，GPU 配对和时间范围检查通过 |

detail 的 HostPrepare 约 0.387 ms、PostProcess 0.385 ms、Present 0.581 ms、OverlayRedraw 0.591 ms。
guest GPU 区间占主要时长，但这些 device elapsed 区间可能含 GPU semaphore/内存依赖等待，
不是 shader busy time，也不证明是哪条指令最慢。不能把 66.6 ms 解释为 30 FPS：30 FPS 周期是 33.3 ms。
目前并未达到 30 FPS。

## CPU/GPU 提交边界的实际修补

- 当前并非 Citron 的独立 command recording/submission worker 架构：Liverpool 有独立 CPU GPU
  命令线程，VideoOut 有 PresentThread，但 Vulkan scheduler 仍在调用线程录制/提交，共用 queue0。
- 基线 `getSemaphoreCounterValue` 在该 Turnip/KGSL 栈上可阻塞一整帧。Android MasterSemaphore
  改为 coalesced submitted watermark + 完成线程，50 ms stop slice 等待 timeline，原子发布完成 tick。
  热路径 Refresh 读缓存与错误状态，显式资源 Wait 仍等待真实完成；桌面行为保留。
- IRQ Signal 处理一次性订阅时，不再持订阅表 mutex 调用旧 flip 的 Prepare/Flush。
  delivery mutex 保持投递串行，persistent 回调保留原有 unregister/drain 行为。
  旧实现反例 22 checks / 1 FAIL，新实现 22 / 0。
- Presenter 原来将刚呈现的 frame 放回 free pool，却继续保存为 last frame 用于 redraw。
  现在持有 last frame 的独占使用权，池多一个 frame，仅在替换 last 时回收前一个，避免 producer
  与 redraw 同用 image/fence。
- 增补 Prepare/Flush/SubmitLock/QueueSubmit/Refresh/NextCommandBuffer/Retire 等 CPU scopes，
  以及 GNM SubmissionGate。它们不会被当作 GPU timestamp。

先前两个阶段的定位数据（场景不完全相同，不能当严格 FPS A/B）：

| CPU 均值 | 基线 | 异步完成/IRQ 修补后 |
| --- | ---: | ---: |
| VideoOut.Prepare | 66.410 ms | 18.671 ms |
| Vulkan.RefreshTimeline | 33.262 ms/次 | 约 0.0004 ms |
| Guest GnmSubmitAndFlip HLE | 49.606 ms | 44.512 ms |

等待转移到了 SubmitLock/Submit 与 GNM gate，未证明等待总量消失。
一次中间 ownership APK 在 Guest-1 FEX JIT SIGSEGV；归档保留 `ownership-crash.txt`，
不能据此归因于 IRQ/worker/frame ownership，也未取得精确异步 guest PC。
后续独立运行和最终游戏运行持续推进；这不是对该一次性 JIT fault 的修复或稳定性证明。

## litep GPU 接入

实现位于 `src/video_core/renderer_vulkan/vk_gpu_profiler.*`、`src/common/gpu_timing.h`、
`src/common/profiler.cpp` 和 Scheduler/Presenter，使用 profiler SDK 的 GPU context/zone/time 协议。
参考 spruntime `feature/zhangjian/vendor-bookmark-region-sdk` 的 `95ec7c810f`，核对
`SdkVulkanGpuBridge` 与 SDK `VulkanGpuContext`，没有使用 Tracy 数据冒充 litep GPU 数据。

每个 Scheduler 有固定 query lease；仅已成功提交、timeline 完成且结果 available 才回收。
无 query WAIT / queueWaitIdle。失败 begin 保留无效索引；关采集仍正确关闭已开始的 zone。
generation 变化丢弃旧结果但等 GPU 完成后才复用；过滤无效/部分 timestamp，最多重试30次后记错误。
支持实际48位 wrap与ns/tick换算、半周期/溢出拒绝。粗粒度默认关闭，detail 另开 render-pass 区间。

当前 Turnip 缺 calibrated timestamp 扩展，采用 submit 前到异步完成观察的 CPU 界估计对齐；
最终显示保守误差约 ±8.34 ms。GPU anchor 使用 begin，避免 begin 早于 end anchor 造成无符号巨大回绕。
这允许看 GPU 区间时长；不能拿两条独立 Scheduler 轨道的亚毫秒相对位置证明先后。

首次 coarse 文件确认缺两个 context 定义（分析尝试延迟解码也无法恢复），不能称三轨完整。
detail 与恢复 coarse 都有三轨，GPU 负时长/孤立端点/超出 CPU 区间1秒的事件均为0。
全部 capture 都保留 CPU 边界 incomplete/truncated 诊断，文件正常结束不代表全链路无损。
录制切换早期的 context 定义缺失仍是限制；不能让后一次通过覆盖首次失败。

Foundation 可复用文档：[litep GPU timequery](../../../foundation/docs/guides/litep-gpu-timequery.md)。
SDK 独立子仓 `368177d` 已发布在 `codex/shadps4-gpu-timequery`，保留35字节 GPU context definition
到 cold metadata，ring 原始页被覆盖后可供后续 file/socket capture 重放；不增加 zone/time 热路径锁。
Foundation 更新了 SDK pin、SdkRevision、独立 profiler_ring CMake target、smoke 链接与接入文档。
最终已测 APK 的 status 仍打印原基线107a620（编译于提交和 revision 常量更新之前），实际含该修复；
精确 APK/DSO/source 哈希见 manifest，不能仅凭这个旧 status 字符串判断运行源码。

## 系统驱动：已实际尝试，渲染未启动

新增显式 `debug.shadps4.vulkan_driver=system|turnip`，默认 Turnip，按 session 参数快照选择。
system 用 `dlopen(libvulkan.so)`，不调用 adrenotools；选择 driver kind 后同进程禁止换 kind，需重启。
不做失败自动 fallback。日志记录实际 device/driver/features；必须看到 source=system 才算切换成立。

普通 APK PID16544 确认实际 Qualcomm Adreno740 / Vulkan1.3.128 / shaderInt64=0。
在 sceVideoOutOpen/op156 创建 renderer 时明确拒绝，未产生游戏帧；[现场黑底手柄图](gpu-timequery-20260915/system-screen.png)。
桌面共享 SPIR-V emitter 仍无条件声明 Int64，profile 的 support_int64 没有完整 lowering 分支，
不能去掉检查后声称可用。系统/Turnip 游戏性能 A/B 尚不存在。已恢复默认方向 Turnip。

另外本轮发现并修了 host DSO 的独立链接问题：liblinkernsbypass 静态库的数据指针
`android_get_exported_namespace` 等被导出，可能 interpose Android 同名函数并跳进不可执行 BSS。
增加 `--exclude-libs,liblinkernsbypass.a` 后动态符号消失，真实 Vulkan probe 越过原失败点。
随后 probe 自身的 `cmd.begin({})` 误选空指针重载已改为显式 CommandBufferBeginInfo；测试失败历史保留。

## 验证与归档

Android GPU timing27/0、async/IRQ22/0、真实 private Turnip collector26/0；SDK wire5/5、clock4/4，
macOS对应GPU测试通过，Android encoding2/2。SDK metadata 反例原先0events、修复后2events；
独立SDK GPU smoke4events含2GPU，无truncation/gap。Foundation main 独立轻量目标构建及ring/capture smoke通过。
没有跑全量回归、Swan、十分钟主动游戏操作或新的性能优化验收。

有界报告、原始关键日志、源码/二进制哈希与截图在 [归档](gpu-timequery-20260915/manifest.json)。
原始 PROF/APK 留在 `build/gpu-async-audit-20260915/`，大文件没有入库。当前主仓生产改动尚未 commit；
Foundation 的已提交内容与其仍未提交的 audio 模块分开保留。

Foundation 发布结果：`main` 已 fast-forward 至 `e8fc0313374b2b723e2c11d80b4c92f09bd73d48`（保留远端并发新增的 `0a7707b`）；当前 owned 分支 `codex/shadps4-android-fex-v0` 已推到 `d41ade4`；SDK `codex/shadps4-gpu-timequery` 已推到 `368177d`。没有强推。
