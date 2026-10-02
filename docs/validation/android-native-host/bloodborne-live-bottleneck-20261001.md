# 血源当前运行瓶颈：AYN Thor，2026-10-01

## 结论

当前证据更指向 **guest 工作线程的完成/唤醒 → 主线程 → guest 提交线程 → 图形命令处理** 这条供给链，以及逐 draw 的 CPU 准备开销。整机 CPU/GPU 利用率低于 100%，不代表这条链上没有瓶颈。

当前是 AYN Thor（9c2841a4 / Adreno 740），不是 Swan。没有重启、安装、注入操作或更改渲染配置；用户在正常游玩。主采样前后是中央亚楠街道战斗，不能与最初楼梯处约 19.7 FPS 的截图作性能 A/B。本轮未作优化收益声明。

## 现场与可复现证据

- 进程 2736，session generation 1，run_uuid `14a81dcd9c2d1b7a1e0f6e4932c09ed0`。
- APK SHA-256 `34a5176ff0088318d5a1fb716f28b54e4899eaa145820529f01fe5021669ed2e`；host Build ID `f7f5cae710307905ff3e483198885efe26bffe77`，SHA `4c74de9f5bf3843c4225168a8dc2b64fce0f3ec2fe9654496331c7c1a920398c`。
- 实际加载 Turnip Mesa 26.3.0-devel git-351a4847a0，SHA `01a3548fbd2695f3b27ad896bfbbbf0561e28c49ad1de8a19b05745acd01ba19`。**不把当前 checkout 85a39824d / Mesa 9b8a3567 当成装机包来源。**
- Bloodborne CUSA03023，render scale 50%，texture medium；gnm_fastpath=1、async_submit=1、software_interp=1、lower_int64=0，均为采样前已有状态。
- 原始文件、索引、脚本、截图与所有 JSON：`/Users/bytedance/workspace/emulations/ps4/shadps4/build/validation/bloodborne-bottleneck-20261001-222806`。
- 主采样：`coarse2/9c2841a4-2736-file-1790865410541714-1.prof`，SHA `2462d839c1bfbf8a590f51116ffe0d37993dd3268ba076eff9791c87724a3a78`；约 9.976 s、145.5 万事件。
- 细采样：`detail/9c2841a4-2736-file-1790865864362717-2.prof`，SHA `7653432bd72dcdd98f3169f225b4d9ce03c5f854abdf63f0937325171f41ac62`；约 3.987 s、211.2 万事件。打开 fine scopes 和 HLE sync detail，不能与主采样作直接提速比较。
- 两轮 KGSL/sched 均为独立 tracefs instance，0 overrun / 0 dropped，CPU 时钟对齐不确定度 1105 / 1080 ns，完整覆盖 PROF。两份 manifest 各 41 项 SHA 校验通过。
- PROF 容器完整、chunks_skipped=0；模式边界有未配对/未闭合 scope，decoder 标记 truncated=true，保留原诊断。以 140 / 58 个完整相邻 SDK marker 区间分析；未闭合 scope 不强行补齐。
- 初次分析调用的 scene_id 曾误写 stationary；截图已纠正为 live gameplay/combat。该 caller assertion 不构成静止场景证明，报告不使用静止/A/B 结论。

## 主采样的帧预算

独立的 `VideoOut.PresentedGuestFrames` 计数在 9.966 s 内增加 141：**14.15 FPS**。GNM marker 140 个完整区间均值 71.06 ms，p50 70.95、p90 85.44、最大 108.25 ms。GNM 是提交 epoch，表格是按该区间归一，不把 marker 自动等同成功上屏。

| 线程 | 执行 on-CPU ms/epoch | 可运行但排队 | 睡眠/等待 |
|---|---:|---:|---:|
| Guest-1，游戏主线程 | 32.36 | 4.08 | 34.62 |
| GpuComm，图形命令处理 | 34.06 | 0.46 | 36.53 |
| VkRecorder，Vulkan 录制 | 15.92 | 9.57 | 45.57 |
| Guest-20，guest 提交 | 8.16 | 4.05 | 58.85 |
| PresentThread | 8.43 | 7.24 | 55.39 |

各行是并行线程，不能相加成帧长。HLE elapsed 包含睡眠，不能直接从 on-CPU 相减。实际 scheduler/HLE 区间交集显示 Guest-1 的 HLE on-CPU 约 4.10 ms/epoch，非 HLE 执行约 28.32 ms；后者包含 JIT/FEX 与未插桩宿主工作，未归因到某个 guest 函数。

### 等待沿谁传递

`waker-analysis.json` 使用同次 trace 的真实 sched_wakeup 发出线程，按睡眠区间计算：

- Guest-20 的睡眠中约 **8.037 / 8.266 s（97%）**由 Guest-1 的唤醒结束。
- GpuComm 的睡眠中约 **4.508 / 5.121 s（88%）**由 Guest-20 唤醒结束；约 0.367 s 由 VkRecorder 结束。
- Guest-1 的条件变量等待主要由 Guest-54..59 / 61..66 结束；信号量等待主要由 Guest-32..36 结束。后五个线程结束的主线程信号量睡眠合计约 1.432 s，约 10.23 ms/epoch。
- Guest-32..36 各自只有约 3.3–4.0 ms/epoch 执行，却各有约 **7.1–8.3 ms/epoch 调度排队**，75–92% 的执行时间在小核。Guest-54..59 也各有 4.6–5.7 ms 排队，约 78–90% 在小核。
- VkRecorder 77.4%、PresentThread 89.6% 的执行时间在小核。现有线程优先级确已应用：GpuComm / VkRecorder / VkSubmit / Present 为 nice -14，主线程为 -10。不能把“已经提高 nice”当作调度已优化。
- 主线程自身几乎不在小核；CPU3–6 大部分时间接近 2.7–2.8 GHz 上限。不能简单归因成“主线程被放到小核”。

这是 **内核唤醒来源证据**，不是完整 guest 条件对象/持锁者/所有前置任务 DAG。当前启动没有开启 profile_sync 的对象链记录；不伪造 selected-notify 边。

细采样进一步显示主线程 `HLE.Sync.Park` exclusive 约 25.35 ms/epoch，而 Guard 0.758、Lookup 0.153 ms。等待任务完成比查表/内部锁管理开销大得多。

### 最慢帧

SDK frame 30873：108.25 ms，邻帧中位数 71.24 ms。

- Guest-20 的 CondWait 98.55 ms，较邻帧多 36.21 ms，几乎全部睡眠。
- Guest-1 on-CPU 33.33 ms；GpuComm on-CPU 33.64 ms，均没有同比增加。
- 该区间 GPU busy 57.66%，也不支持 GPU 满载导致这次长帧。
- 因缺少条件对象完整因果链，不能仅凭重叠把某一工作线程命名为唯一阻塞者。

## 图形 CPU 与 GPU

主采样每 epoch 约 1955 draw；GpuComm 的 Rasterizer.Draw inclusive 29.33 ms，CopyShader 4.05 ms，RecorderSync 2.65 ms。细采样是另一段游玩、约 1405 draw，拆分为：

| CPU scope | inclusive ms/epoch | 含义 |
|---|---:|---|
| Rasterizer.Draw | 26.46 | 包含以下资源绑定，不可重复相加 |
| Bind.Textures | 5.52 | 每 draw 纹理查询/绑定 |
| Bind.Buffers | 3.99 | buffer 查询/准备 |
| Rasterizer.BindResources | 11.27 | 包含 texture/buffer 等子项 |
| HLE.CopyShader | 3.49 | HLE copy kernel，不等同着色器编译 |
| Vulkan.RecorderSync | 3.33 | 收尾等待录制线程 |
| Buffer.Upload | 0.045 | 本窗口实际上传调用开销很小 |

主采样 Texture.Stage 合计仅约 0.13 ms；未见 pipeline/shader 编译 scope。异步提交 Post→Worker 共 1019 条显式匹配边，平均排队 0.382 ms、p90 1.011 ms。当前常态低帧率不应首先归因于 PSO 冷编译、texture upload 或 vkQueueSubmit 同步阻塞；pipeline cache 查找/资源准备仍可能在 draw 的剩余 CPU 时间中，需后续采样栈细分。

GPU pwrstats 忙碌 **69.25%**（638 样本），gpubusy 独立读数 68.61%；频率 401 MHz 占39.66%、475占35.75%、550占18.30%、615占6.29%，硬件上限680 MHz。busy 口径包含其他 GPU 使用者，不是着色器 ALU occupancy。细采样 busy67.95%。

KGSL ctx16 的1017个完整批次执行 tick 合计6.672s；不能把 submitted→retired p90 42.32ms当作纯GPU耗时。全GPU busy折算约49ms/游戏epoch，与71ms帧周期相比存在空档，仍不能把这些空档直接当成可优化收益。

GPU thermal_pwrlevel=0 / throttling=0，CPU大核接近上限；温度快照中CPU热点约80–89°C、GPU约68–75°C。有温度信息，但没有证据证明本窗口主要被热限频；CPU7降频可能反映调度/负载，不作因果判断。

### HUD 重绘是额外开销项

每 epoch 平均1次游戏呈现、约2.13次旧画面重绘（约29.9次/秒）。PresentThread 的 RedrawFenceWait 约18.62ms/epoch、DriverCall约7.87ms/epoch（均elapsed，非纯CPU）。

它们不是新游戏帧，但要再次提交、合成并在同一GPU队列上执行。建议后续做“仅HUD更新/显示节流”同场景A/B；本轮没关闭HUD，不能宣称关闭能提升多少FPS。

## 新版 my_mcp_tools 的使用与限制

读取了用户指出的最近会话，使用已安装 `0.3.0-local.20261001.gpuevidence1` 的 LiteP 联合分析、长帧贡献、每帧预算和新 `analyze_gpu_coverage`。RenderDoc 的 `gpu_workload_analyze` 需要有效RDC；为保留当前运行现场，本轮未重启注入，也未拿旧MHR/TMNT回放冒充本次血源结果。

现场发现的工具/插桩问题：

1. collect_evidence 的 KGSL 脚本路径误指向 bin/litep/tools；临时目录软链接使采集完成，结束已删除，首次失败证据保留在 coarse/。
2. SDK 白名单仅认 Foundation b6797a0，生产者报 vendored SDK 0b467a8。Foundation provenance 明确两者使用 c1204a4a tree；逐个核对7个reader模块，除包内import前缀外内容一致。独立 `analyze_live.py` 只在私有进程增加准确版本映射，不改原始PROF、上报SDK号或全局安装文件。
3. collect_evidence 把 gpu_timing/pipeline_cache/upload_diag 非JSON输出按 hle_sync JSON解析，结果报错；这些字段没有用作前后计数证据，另留原始debugbus文本。
4. GPU guest query 已停止更新并填满32个租约，dropped_batches增长；Present/Overlay还继续更新。主采样有496对GPU区间，但没有DrawBatch/GuestCommands。新coverage工具正确返回 supported=false，不能据此声称guest GPU耗时为0；本报告GPU负载使用独立KGSL证据。源码 Collect 的最旧eNotReady会阻断后续回收，是需进一步核验的线索，未宣称已定位具体Vulkan/Turnip原因。
5. frame_budget 的 non_hle_on_cpu_estimate 用HLE elapsed减on-CPU，遇睡眠会失真。本报告不采用它，使用联合分析的真实区间交集。
6. KGSL submitted事件的发出者不总是context拥有者（可由kgsl dispatcher/其他进程触发）。ctx3有2285/875/2736等发出者，工具把它加入target_contexts并不可靠；本报告不引用“所有target context就是本进程”或据此推导GPU归属。ctx16/3等原始分组和全部发出者保留于waker-analysis.json。

## 下一步优化优先级

1. **优先做任务线程/录制线程调度A/B。** 同场景检查短任务高频唤醒、关键worker小核驻留与runnable时间；可用临时亲和性/优先级实验检验，再决定生产策略。不要全线程绑到一个大核，也不要把其他线程的排队时间直接累加当成可提速预算。
2. **削减逐draw资源准备。** 定向采样GetGraphicsPipeline、纹理/buffer查询、脏状态重复计算；先量化重复比例再做缓存。现有fine scope留下约15ms Rasterizer.Draw exclusive，不能全称为PSO查找。
3. **降低可选重绘与收尾同步。** HUD更新节流/缓存、RecorderSync末端排队分别测量，保留guest flip和vblank语义。
4. **修复GPU查询回收后再做GPU pass细分。** 当前不能可靠做guest draw覆盖率或ALU/带宽分类，不能以升级驱动或盲目改异步上传代替证据。

## 恢复与边界

- gpu_timing off、fine scopes off、hle_sync disabled/detail false；ring恢复原来的recording=1，generation因两次Streaming切换变为5。
- 两轮独立tracefs instance及设备临时目录均删除，global tracefs状态前后一致。
- 两份设备PROF及其sidecar在核对本地/设备SHA后删除，本地证据完整保留。临时工具软链接已删除。
- 无游戏重启/安装、无输入、无设置/存档文件编辑、无root策略/频率/亲和性修改，无生产源码修改或commit/push。
- 所有采集开关恢复与设备证据清理完成后，最后一次 pidof 已找不到原进程；末尾保留日志未说明退出原因，不声称游戏仍在运行或本轮验证了稳定性。原 PID 日志保留于 last-process-logcat.txt，未主动重启。
