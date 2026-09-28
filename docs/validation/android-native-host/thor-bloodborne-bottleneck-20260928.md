# 血源在 AYN Thor 上的瓶颈归因：为什么 Render Scale 0.25 不提帧（2026-09-28）

用户在另一台设备上用最新源码把 Render Scale 改成 0.25，血源帧率基本不变，要求深入分析当前瓶颈。§1–§7 只做测量与归因，没有改代码；§8 为做 multiblock 实验，加了一个默认关闭的测量开关。§10 是 `GpuComm` 的第一批改动；§11 让 multiblock 区域内的循环可以暂停，并修了调试单步越过 RET 的问题。§8、§10、§11 的代码与本报告一起提交（本地提交，未推送）。数据全部来自当时唯一连着的 AYN Thor，结论只适用于 Thor 这一台设备。§6 说明它和用户那台设备、以及 Swan 之间能否直接对照。

## 1. 身份与环境

| 项 | 值 |
|---|---|
| 设备 | AYN Thor `9c2841a4`，QCS8550，Android 13；GPU Adreno 740（Turnip），`max_gpuclk` 680 MHz；CPU 分三簇：cpu0-2 小核（最高 2.0 GHz）、cpu3-6 中核（最高 2.8 GHz）、cpu7 超大核（最高 3.19 GHz） |
| 源码 | `malos/main` = `feature/malos/swan_performance` @ `60ec46d0`。本地仅有子模块指针和未跟踪文档的改动，未改源码 |
| 装机版 APK | 2026-09-27 22:09 安装，来源不在本机，**未做身份核对**。只用于 §2 的 0.5 / 0.25 对照 |
| 本轮 APK | `f8e32c05…`；host `libshadps4_host.so` SHA `1cb14563…`（Build ID `4db1ae28`）；FEX 会话库 Build ID `2288974b`。设备上的身份已由 Litep `collect_capture` 回读确认一致 |
| 设置 | Render Scale 0.5，Texture medium，Force Disable MSAA 开（用户原有设置）。0.25 只是临时改了 `global.json`，结束后已按原始字节恢复（SHA `3a9bb11d…`） |
| 场景 | 读档"继续"后的出生点，人物静止，在教堂外墙旁（截图见 evidence） |
| 运行状态 | cpuset `/top-app`，录制线程、SRT 批量读、同步快路径均为开启 |

## 2. Render Scale 0.5 与 0.25 对照（装机版 APK，同一场景，各约 20 秒）

| | 0.5 | 0.25 |
|---|---|---|
| FPS | 16.82 | 17.24 |
| GPU busy / 平均频率 | 70.3% / 498 MHz | 65.8% / 401 MHz（最低档） |
| 每帧 GPU 工作量（busy × 频率 ÷ FPS） | ≈20.8 M 周期 | ≈15.3 M 周期（−26%） |
| 按 680 MHz 折算的每帧 GPU 时间 | ≈31 ms | ≈23 ms |
| `Guest-1` on-CPU | 41.1 ms/帧 | 39.9 ms/帧 |
| `GpuComm` on-CPU | 40.3 ms/帧 | 38.5 ms/帧 |

0.25 确实让 GPU 工作量减少了约四分之一，画面也明显变糊，但每帧约 58 ms 的帧长几乎没变。GPU 远没有饱和，DVFS 一直在降频。本轮 APK 在 0.5 下复测为 16.25 FPS、`Guest-1` 42.4 ms/帧、`GpuComm` 42.1 ms/帧，和装机版一致。

## 3. CPU 调度：谁在关键路径上

### 3.1 schedstat（按线程，每帧 63.2 ms）

| 线程 | on-CPU | 排队等 CPU | 睡眠 | 主动 / 被动切换（每帧） |
|---|---|---|---|---|
| `Guest-1`（游戏主线程） | 43.5 | **9.0** | 10.7 | 137 / 11.9 |
| `GpuComm` | 43.2 | 1.5 | 18.5 | 9.4 / 6.2 |
| `Guest-21`…`Guest-25`（游戏任务线程，每个） | ≈19 | ≈7.8 | ≈36 | ≈78 / ≈24 |
| `Guest-20`（GPU 提交线程） | 13.5 | 2.6 | 47.1 | 15.8 / 7.9 |
| `VkRecorder` | 11.5 | 5.0 | 46.7 | 46 / 15 |

### 3.2 sched_switch / sched_waking trace

来源：39.7 秒、8 个 CPU 均无溢出，由本仓脚本解析，详见 §7。

| 线程 | on% | 小核 / 中核 / 超大核 | 平均频率 | 排队% | 被唤醒次数 |
|---|---|---|---|---|---|
| `Guest-1` | 69.3 | 1.3 / 58.0 / 40.7 | 2.63 GHz | 15.1 | 2478/s |
| `GpuComm` | 64.8 | 0.2 / 48.3 / 51.5 | 2.77 GHz | 0.9 | 146/s |
| 任务线程（每个） | ≈31 | ≈5 / ≈90 / ≈5 | 2.56 GHz | ≈10 | ≈1300/s |

### 3.3 结论

- **不是跑在小核上。** 单次 schedstat 采样看到 `Guest-1` 停在 cpu1，只是那一刻的快照；trace 显示它只有 1.3% 的时间在小核上。
- **游戏主线程和任务线程之间是细粒度往返。**
  - `Guest-1` 每秒被唤醒约 2500 次（每帧约 160 次），主要由 5 个任务线程唤醒；
  - 它反过来每秒唤醒每个任务线程约 350 次；
  - 这是游戏自身任务系统的同步方式，每次往返都要付出 HLE 同步调用和调度延迟。
- **快核不够分。**
  - 重负载线程合计约 3.4 个核的需求，集中在 4 个中核加 1 个超大核上。
  - `Guest-1` 每秒被抢占约 900 次，每帧约有 9 ms 在排队等 CPU。
  - `Guest-1` 和 `GpuComm` 还在争同一个超大核 cpu7。
- **有不可中断睡眠（D 状态）。** `Guest-1` 每秒约 260 次，任务线程每个约 180 次。结合 §4 中写跟踪缺页路径的开销，推测与缺页处理及 `mprotect`/mmap 锁争用有关。由于 `kptr_restrict` 隐藏了内核符号，**这一点未证实**。
- **Litep PROF 帧预算（307 个完整帧，每帧 65.0 ms）** 与上面一致：
  - `Guest-1` 每帧 HLE 约 897 次，合计 14.6 ms，其中真正在等待的只有 cond 4.5 ms + sema 2.1 ms；
  - 其余约 50 ms 是 JIT 游戏代码加上调度延迟；
  - `Guest-20` 每帧提交 11 次命令缓冲，然后在 cond 上等待约 47.7 ms。

## 4. CPU 时间花在哪（simpleperf，10 秒，84384 样本，0 丢失，约 162 帧）

| 线程 | on-CPU | 主要构成（ms/帧） |
|---|---|---|
| `Guest-1` | 42.0 | **JIT 游戏代码 28.4**；HLE 9.4（同步 2.7，Gnm 1.9）；写跟踪缺页 1.4；FEX 分发 0.9；内核自身时间合计 6.2 |
| 任务线程合计 | 94.1 | JIT 游戏代码 45.8；HLE 29.1（Gnm 9.5）；写跟踪缺页 6.9；内核自身时间 20.8 |
| `GpuComm` | 37.8 | 着色器查找 8.7；buffer cache 7.0；texture cache 6.3；rasterizer 6.1；资源绑定 2.7；写跟踪 2.2；SRT 2.1；PM4 解码 1.8 |
| `VkRecorder` | 10.0 | 75% 在 Turnip，主要是 `tu_update_descriptor_sets` |

`GpuComm` 里还有几项与游戏负载无关的固定开销：

- `memcpy` 占自身时间 9.5%（≈3.6 ms/帧）。
  - `pm4_stats` 显示每帧 10914 次 stream 拷贝、8 MB；
  - 其中同帧未变化 6286 次（4.2 MB），跨帧未变化 810 次（2.1 MB）。
- profiler ring 的 `encode_span_begin/end` 约 6.5%（≈2.5 ms/帧）。ring 默认常开。
- out-of-line 原子 helper（`__aarch64_ldadd4_acq_rel`、`__aarch64_cas4_acq`）约 7%（≈2.7 ms/帧）。

**逐 draw 的着色器设置 HLE**：`Guest-1` 加 5 个任务线程，每帧约 1200 次 `sceGnmSetVs/PsShader`（每次约 15 µs）和约 1350 次 `sceGnmIsUserPaEnabled`，合计约 20 ms CPU/帧。

## 5. 结论

1. **Thor 上血源是 CPU 受限。**
   - GPU 在 0.5 时只有约 70% busy，并且被降频到约 500 MHz；按满频折算只需 23–31 ms/帧。
   - 所以降低 Render Scale 只减少 GPU 工作量，不改变帧长。
2. **帧长由两条并行、几乎等长的 CPU 路径决定。**
   - 游戏主线程：on-CPU 约 43 ms（其中 JIT 游戏代码约 28 ms），另有排队约 9 ms、等任务线程约 11 ms；
   - `GpuComm` 翻译：约 38–43 ms。
   - 只压其中一条，另一条会立刻成为上限。
3. **最大的单项是 FEX 翻译后的游戏代码**：主线程约 28 ms/帧，任务线程合计约 46 ms/帧。当前 FEX 配置固定为 `MULTIBLOCK=0` 和 `GDBSERVER=1`（`src/core/guest_cpu/fex/fex_context.cpp:1082-1083`），TSO 为 FEXCore 默认值。这是为了可重启边界、取消和调试而有意这么设的，代价还没有被测量过。

## 6. 与其它设备的关系

- **用户那台设备**：身份未知，本轮没有连接。主干最新提交 `60ec46d0` 的验证记录是在 Pocket DS 上做的，血源进中央亚楠约 15 FPS，与 Thor 的 16 FPS 接近。用户说的"另一台设备"可能就是它，但未确认。Swan 在 2026-09-24 出厂频率下是 GPU 受限（gpubusy 97–99%，0.25 时仍约 92%）。所以"0.25 不提帧"在 Swan 上的原因可能不同：一部分是 GPU 固定开销不随分辨率下降，而不仅仅是 CPU。
- **不能把 Thor 的结论直接套用到 Swan**，需要在用户的设备上复核：`sample.sh` 加 `sample2.sh`，外加 gpubusy / gpuclk，即可判断属于哪一类。

## 7. 工具与流程记录

- **host 构建输出目录的配置校验**：新代码的构建配置多了 `scrcpy_capture_sdk` 字段，旧输出目录被判为配置不一致。
  - 核对差异只有这一个值为 null 的新字段后，在旧目录的 `profile.json` 里补上了它（原文件备份为 `profile.json.pre-scrcpy-key`），然后增量构建。
  - 在全新输出目录构建时，Windows 端的 `protoc` 会因 abseil `/MD` 与 protobuf `/MT` 运行时不一致而链接失败，**未修**。
- **Litep `capture_kgsl` 在 Thor 上两次失败**，原因都是 `KGSL capture exceeded its window`。
  - 设备端实际的 start/end 锚点间隔为 39.7 秒，请求的是 8 秒；推测是 root 桥接执行较慢。
  - 24 秒那次还出现了每 CPU 9 万–21 万条溢出，数据不用。
  - 8 秒那次数据完整，由本仓的 `tools/sched_parse.py` 手工解析。
  - 该数据**没有**绑定 Litep sidecar，因此 PROF 的"未标注"时间没有做逐帧的 on-CPU / runnable / sleep 拆分。
- **会话控制**：`FexSessionService` 对 shell 不导出（`am startservice` 返回 "Requires permission not exported"）。停止游戏改走界面：返回键 → Stop。停止后会话界面保留最后一帧，需要对空闲的 app 执行 force-stop 才能回到 Library。`tools/launch_bb.sh` 已包含这些步骤。
- **tools/ 目录下的脚本**
  - `pad.sh`：DebugBus 手柄输入（Thor）。
  - `launch_bb.sh`：停止 → 回到 Library → 启动 → 过提示/标题/离线/继续。
  - `sample.sh`：FPS、gpubusy/gpuclk、各簇 CPU 频率、按线程 on-CPU。
  - `sample2.sh`：按线程 on-CPU / 排队 / 睡眠及上下文切换，来自 schedstat。
  - `sched_parse.py`：按簇分布、按频率折算的算力、唤醒来源。
  - `perf_thor.py`：simpleperf 分桶。
- **证据**：摘要在 `evidence/thor-bloodborne-bottleneck-20260928/`。大文件（PROF、sched trace、perf.data）在 `build/validation/thor-bb-bottleneck-20260928/`，SHA 记录在 `artifacts.sha256`。

## 8. FEX multiblock 上限实验（同日追加）

**做法**

- 新增一个默认关闭的测量开关：`src/core/guest_cpu/fex/fex_context.cpp` 在 `CONFIG_MULTIBLOCK=0` 之后读取 `debug.shadps4.fex_multiblock`，值为 1 时改设 `MULTIBLOCK=1`，并在 logcat 的 `FexCore` 标签下打出警告。
- 其它配置不变，包括 `GDBSERVER=1`、`EntryBackedgePass` 和 TSO 默认值。
- APK `46c9a8bc…`（FEX 会话库 `libshadps4_fex_session.so` SHA 前缀 `5f5cdb9d`，已确认包含该属性字符串）。
- 同一 APK 按 A→B→A→B 交替测量。每个阶段先界面 Stop、force-stop 空闲 app，再从 Library 启动、读档到同一出生点；静置 20 秒后，先用 `sample.sh`（20 秒）、再用 `sample2.sh`（24 秒）采样。
- B 阶段的 logcat 均确认出现了 `MULTIBLOCK enabled`。
- 设备未锁频，温度约 92–95 °C（thermal zone 最高值）。

**结果**

| 阶段 | multiblock | FPS（sample / sched） | `Guest-1` on-CPU（ms/帧） | 任务线程 on-CPU（每个，ms/帧） | `GpuComm` CPU% / 睡眠（ms/帧） | GPU busy / 频率 |
|---|---|---|---|---|---|---|
| A1 | 关 | 16.92 / 16.85 | 41.6 / 41.0 | ≈17.6 | 68% / 17.6 | 71% / 489 MHz |
| B1 | 开 | 19.07 / 18.94 | 34.6 / 34.5 | ≈15.4 | 82% / 8.6 | 73% / 589 MHz |
| A2 | 关 | 16.18 / 16.34 | 42.6 / 42.1 | ≈18.6 | 68% / 17.8 | 69% / 483 MHz |
| B2 | 开 | 18.45 / 18.45 | 35.3 / 34.9 | ≈15.7 | 80% / 9.3 | 71% / 554 MHz |

- **帧率**：关闭时 4 次平均 16.57 FPS，打开时 4 次平均 18.73 FPS，**提升约 13%**。每一次打开都高于每一次关闭。
- **JIT 游戏代码变便宜**：`Guest-1` 的 on-CPU 从约 41.8 降到约 34.8 ms/帧（−17%）；任务线程每个少约 2.5 ms（−14%）。`Guest-1` 的排队（约 8.7 ms）和睡眠（约 10 ms）基本不变。
- **瓶颈转到 `GpuComm`**：它的 CPU 占用从 68% 升到 80–82%，睡眠从约 18 ms 降到约 9 ms/帧。所以 13% 是被 `GpuComm` 封住的结果，**不是 multiblock 在主线程侧收益的上限**。
- **B 阶段的主线程**：on-CPU 34.9 + 排队 8.9 + 睡眠 10.3 ≈ 54 ms/帧，与 `GpuComm` 的约 53 ms 再次接近，两条路径重新并列成为上限。

**正确性观察（很有限）**

- 两个 B 会话在出生点静置期间都没有崩溃。
- B2 里让人物前进 1.8 秒、转镜头 1.2 秒，画面正常（截图在 `build/validation/.../multiblock-ab/B2-*.png`）。
- 两个 B 会话都能通过界面 Stop 正常结束（`user_stop`，约 3 秒）。
- 这只说明"在这个场景、这段时间里没有出现问题"。**没有**覆盖以下方面：
  - 热循环中的暂停和取消（G47 一类测试）；
  - 调试器、auto tag 探针、写 watch；
  - 代码发布时的暂停（CodePublication park）；
  - 长时间游玩或战斗。

**要把 multiblock 变成默认值，前提是**：multiblock 区域里的内部回边（跳回区域内部、而不是 EntryPoint 块的循环）也要能响应暂停。例如在这类回边上插入一个"检查暂停请求标志，置位则走普通 `ExitFunction` 退出到目标 RIP"的检查，再用现有的热循环取消、调试和发布测试做回归。**在此之前，这个开关只能用于测量。**

实验结束后属性已恢复为空，`global.json` 保持原始 SHA，会话已正常停止。代码改动（14 行，只加了这个开关）当时未提交，后与 §10–§11 一起提交。

## 9. 下一步候选（按预期收益排序；写于 §8 之后）

> 更新：第 1 项的 multiblock 暂停安全已在 §11 实现；第 4 项里 profiler ring 的逐 draw 开销和 stream 屏障已在 §10 处理，out-of-line 原子因锁复用无收益而暂缓。仍未做的 `GpuComm` 候选：每 draw 的 `GetGraphicsPipeline` 约 6.7 ms（重建 runtime info、SRT 展平、StageSpecialization 构造与线性比较），以及 stream 拷贝去重。

1. **FEX 代码生成配置实验。** `MULTIBLOCK=1` 已测，见 §8：+13% FPS，`Guest-1` −17%，瓶颈随即转到 `GpuComm`。TSO 相关选项尚未测。
   - 这会触及 EntryBackedgePass、取消和调试的边界设计，结论只能作为上限参考。
   - TSO 放宽可能破坏游戏任务系统的无锁同步，必须配合正确性检查。
2. **线程调度。** 降低主线程排队的 9 ms/帧：例如提高 `Guest-1`、`GpuComm` 相对任务线程的调度优先级，或调整亲和性。改动小，但需 A/B 验证，并确认不影响系统其他部分。
3. **着色器设置 HLE 的 guest 侧快路径**（即 Swan spec 中的 P3）：约 20 ms CPU/帧，分布在多个线程上。
4. **`GpuComm` 固定开销**，共约 9 ms/帧：
   - stream 拷贝去重，其中 70% 的拷贝没有变化；
   - profiler ring 常开的开销；
   - 编译为内联 LSE 原子操作（去掉 out-of-line helper）。
5. **写跟踪缺页**：各线程合计约 10.5 ms/帧；D 状态的根因需要内核符号才能确认。

## 10. `GpuComm` 第一批：细粒度 profiler scope、映射锁与 stream 屏障（同日追加）

§8 之后 multiblock 打开时 `GpuComm` 是上限，本节的 A/B 都在 `MULTIBLOCK=1` 下、同一会话内用运行时开关切换，每个窗口 15–20 秒，指标是 `GpuComm` 的 on-CPU（ms/帧与 CPU%）。设备未锁频，FPS 随温度缓慢下漂（同一会话内约 20.3→18.6），所以只比较相邻窗口。

**改动**

1. **每 draw 的细粒度 scope 改为按需记录。** PROF 统计 `GpuComm` 每帧约 1.52 万对 scope，其中 `Bind.Textures`、`Bind.Buffers`（各约 3470/帧）、`Rasterizer.BindResources`、`Bind.HleCheck`、`Bind.ScaleCheck`（各约 1700–1730/帧）、`Buffer.TexelImageSync`（约 1400/帧）共约 1.35 万对。新增 `Common::Profiler::FineScope`，只有 DebugBus `profiler_ring fine on` 时才记录（默认关，关闭时每处只多一次 relaxed load）。`Rasterizer.Draw`/`Dispatch`、`PM4.Resume`、`HLE.CopyShader` 等粗粒度 scope 不变。需要逐 draw 拆分时，先 `profiler_ring fine on` 再采集。
2. **删掉 `Rasterizer::Draw` 里遗留的 Android SBS draw 采样日志**（Beat Saber 调试时加的，每 240 次 draw 打一行 `LOG_INFO`，外加每 draw 一次原子加）。
3. **stream 拷贝复用映射读锁（已撤回）。** 让同一个 buffer 绑定循环里连续的 stream 拷贝共用一次 `MemoryManager` 共享锁。每帧加锁次数从约 1.45 万降到约 6.6 千，但 `GpuComm` 时间没有可测变化，而它会延长写者（guest map/unmap/protect）的等待，所以撤回。

**结果**（`gpucomm-ab/`）

| 对照 | 窗口 | `GpuComm` ms/帧（关 → 开） | `GpuComm` CPU% | FPS |
|---|---|---|---|---|
| 细粒度 scope 开 → 关（F1） | 各 33 s | 45.7 → 43.2 | 83.0% → 79.7% | 18.17 → 18.45 |
| 同上（F2） | 各 33 s | 46.5 → 44.0 | 82.6% → 78.9% | 17.76 → 17.91 |
| 映射锁复用 关 → 开（R1/R2/R3） | 各 33 s | 43.4→42.7 / 42.8→43.0 / 44.5→44.1 | 基本不变 | 基本不变 |
| 新旧组合（N1–N3 对 O1–O3，ab1） | 各 27 s | 旧 42.6/43.5/44.6，新 39.3/40.5/41.8 | 83% → 79% | +0.3 至 +0.7 |

- 细粒度 scope 关闭后 `GpuComm` 每帧少约 2.5 ms，CPU 占用少约 3.5 个百分点，FPS 提高约 1–1.5%（`GpuComm` 此时约 80% 忙，还不是完全满载）。
- 映射锁复用的三对差值为 −0.7、+0.2、−0.4 ms，在噪声内。§4 的 perf 把约 2.2 ms 归到 `__aarch64_ldadd4_acq_rel`（95% 来自 stream 拷贝的 `unlock_shared`），但锁次数减半后没有省下时间，说明那部分归因偏高（帧指针回溯丢了中间一层，且原子操作的采样有滞后）。**以同会话 A/B 为准。**
- 用到的 APK：`c53e2c62…`（含已撤回的锁复用，用于 ab1/ab2）。

**4. stream 缓冲区的只读绑定不进屏障跟踪（新增）**

- `ResetBindings` 原来对每个绑定过的 buffer 调用 `Runtime::AccessBuffer`，往屏障跟踪树插一条读记录；绑定时还要 `IsBufferAccessed` 查一次。
- 其中大部分是 stream 缓冲区：ObtainBuffer 的小只读快照、逐 draw 常量，每帧约 1 万次。
- stream 缓冲区只有 CPU 写入。唯一的 GPU 写入是计算着色器的 LDS 模拟区，而那块本来就不进 `bound_buffers`。stream 区间只在 GPU 用完（等 fence）后才复用。所以这些读记录不会促成任何屏障。
- 改为：`Rasterizer::TrackRead` 统一处理只读绑定（顶点、索引、间接参数与计数、dispatch 间接参数），`BindBuffers` 对 stream 快照同样处理，都跳过 stream 缓冲区。
- DebugBus `upload_diag stream_barriers on` 可恢复旧行为，默认关。

**stream 屏障的结果**（APK `44682188…`，`looppoll-ab/`，每个会话内按关/开/关切换，各 27 s）

| 会话 | 关（新） `GpuComm` ms/帧 / CPU% | 开（旧） | FPS 关 / 开 |
|---|---|---|---|
| multiblock，poll 开（P1、P2 共 4 个"关"窗口，2 个"开"窗口） | 40.7 / 74.9% | 43.4 / 79.1% | 18.39 / 18.24 |
| multiblock，poll 关（Q1、Q2） | 40.7 / 74.1% | 43.3 / 78.2% | 18.20 / 18.07 |
| `MULTIBLOCK=0`（M0） | 40.0 / 64.0% | 42.8 / 67.6% | 15.98 / 15.80 |

- 5 个会话里，每次"开"都比相邻两个"关"窗口高 2.3–3.6 ms/帧，平均约 2.7 ms/帧，CPU 约 4 个百分点。
- FPS 只高 0.1–0.2（约 1%）：`GpuComm` 降到约 75% 后，帧长又回到与主线程并列的状态。

## 11. multiblock 的暂停安全：循环头 poll 与单步修复（同日追加）

§8 要把 multiblock 变成默认值，前提是区域内部的循环也能响应暂停。本节按 FEX 源码（`references/FEX` @ `3f1f30a0`）确认了问题并实现修复。

**问题**

- multiblock 区域里，只有区域起点和 CALL 之后的块是 EntryPoint；区域内的跳转落在入口前导代码之后，不经过入口 poll。
- 条件跳转的"跳转"一侧不带任何探针；无条件回跳前 FEX 会放一个探针，但缺页处理只认"区域头之后的第一个探针"，其余都跳过。
- 所以跳回非起点块的循环（最常见的 `jcc top` 形式）永远不会停下。`EntryBackedgePass` 还会把跳回 CALL 之后块的边改成 `ExitFunction`，而那个次入口的探针同样被跳过，这类循环也停不下。

**实现**

- 新增 `LoopHeaderPollPass`（`src/core/guest_cpu/fex/loop_header_poll_pass.h`），在寄存器分配之后运行：对区域内每条"目标是非起点 guest 块、且按块顺序向后或指向自身"的 Jump/CondJump 边，在目标块开头插入一个 `GuestOpcode` 标记和一次 `str xzr, [x28, #中断页偏移+8]`。
  - 任何环在线性块序里都至少有一条这样的边；指令内部块（REP、原子操作）的边不动。
  - 这个 store 不需要寄存器；中断页未保护时每次循环只多一次 store。
- `InterruptFaultHandler` 新增一个分支：`si_addr == fault_page + 8` 且 PC 在 JIT 代码内时，在当前区域的 RIP 表里找宿主 PC 恰好等于该 PC 的最后一条记录，得到循环头的 guest RIP，然后按入口 poll 的同一方式停下。找不到就跳过这条 store，页面保持保护。
  - 取"最后一条完全匹配"，是因为 FEX 编译重试时可能在表前留下作废的记录。
- `EntryBackedgePass` 只处理跳回区域起点的边，跳回 CALL 之后块的边改由 poll 处理。`MULTIBLOCK=0` 时没有这类块，行为不变。
- 只有 `debug.shadps4.fex_multiblock=1` 时才挂这个 pass。`debug.shadps4.fex_loop_poll=0` 可以关掉它，仅供测开销和做反证。

**单步越过 RET 的问题（`MULTIBLOCK=0` 下也存在）**

- 调试单步靠 TF 单指令编译，TF 检查只在这类块的入口。
- 被单步的 RET 或间接跳转会先查 call-return 栈和线程的 L1 查找缓存，命中就直接跳进没有 TF 检查的缓存代码，一路执行下去。
  - `MULTIBLOCK=1` 下，全速执行过的 CALL 会压入返回地址，所以第一次就会越过；
  - `MULTIBLOCK=0` 下，只要返回点以前全速执行过（进了 L1），也会越过。
- 修复：每次调试单步（含写 watch 的单步）前，清空该线程的 call-return 栈和 L1 缓存（与 FEX 失效代码时的做法相同），让目标走分发器，分发器看到 TF 会转去单步编译。只影响调试单步，正常运行不受影响。

**测试**（设备 AYN Thor，`guest_execution_tests` SHA `326abbb9…`，`multiblock-tests/`）

- 新增 G51a–e：跳回非起点头的 `jne` 循环、`jmp` 循环、以 CALL 之后块为头的循环、带 `LOOP` 内层的嵌套循环、跨回边依赖 CF 的 `adc` 循环。每个预热 1 万次迭代后连续暂停 20 次，要求每次都停在循环头、恢复后继续推进、最后返回正确结果；`adc` 用例要求每次暂停都保住 CF（`rax == 迭代数 - 1`）。
- 新增 DBG25：CALL 全速执行，被调函数里 INT3 停下，再单步越过 RET，必须停在 CALL 之后并得到正确的 eax。分冷、热两轮：热轮先完整跑一次，让返回点进入缓存。

| 运行 | 结果 |
|---|---|
| 完整执行测试，`MULTIBLOCK=0` / `=1` | 258/0、258/0 |
| `--focused-debugger`，`MULTIBLOCK=0` / `=1` | 86/0、86/0 |
| 反证：`MULTIBLOCK=1` 且关掉循环头 poll | 在 G25（持久 owner 版本切换，需要暂停循环中的线程）处失败退出（exit 4） |
| 反证：去掉单步前清缓存 | `MULTIBLOCK=0` 热轮失败（停在返回门，eax=8）；`MULTIBLOCK=1` 冷轮就失败 |

测试之前的中间结果（`mb0/mb1/dbg*` 等文件）来自修复前后不同的测试二进制，以最终两行为准。

**游戏内**（APK `44682188…`，`looppoll-ab/`，每个会话重新启动并读档到同一出生点，按 P1→Q1→P2→Q2→M0 顺序；表中为各会话"跳过 stream 屏障"窗口的平均值）

| 配置 | FPS | `Guest-1` ms/帧 | `GpuComm` ms/帧 |
|---|---|---|---|
| `MULTIBLOCK=1`，循环头 poll 开（P1、P2） | 18.39 | 35.5 | 40.7 |
| `MULTIBLOCK=1`，循环头 poll 关（Q1、Q2） | 18.20 | 35.8 | 40.7 |
| `MULTIBLOCK=0`（M0） | 15.98 | 43.6 | 40.0 |

- **循环头 poll 在这个场景里没有可测开销**：开与关的 FPS、`Guest-1` 时间都在噪声内。
- **multiblock 带 poll 仍比 `MULTIBLOCK=0` 快约 15%**，`Guest-1` on-CPU 少约 19%，与 §8 一致。
- **正确性**：
  - 四个 multiblock 会话都能通过界面 Stop 正常结束，第一次查询状态（约 0.34–0.39 s）时已进入 Stopping。poll 关时也能停，说明这个场景的游戏线程会频繁进 HLE，并不依赖循环 poll。poll 兜底的是纯计算的热循环，这由 G51 与 G25 的反证覆盖。
  - 画面截图正常（`looppoll-ab/P2-arrived.jpg`）。
- 结束后 `debug.shadps4.fex_multiblock`、`debug.shadps4.fex_loop_poll` 已清空，会话 `user_stop`。

**尚未覆盖**

- `adc` 以外的标志位（区域内的死标志消除可能让暂停时报告的 RFLAGS 部分位过时，执行结果不受影响）；
- auto tag 探针、代码发布暂停在真实游戏里的长时间运行；
- 血源以外的游戏（TMNT、MHW 等），以及长时间游玩、战斗；
- Swan 与用户的设备。

multiblock 仍是默认关闭的开关（`debug.shadps4.fex_multiblock=1`）。改为默认开之前，建议至少在 TMNT 与另一款游戏上各做一次启动到可操作场景、界面 Stop 的检查。

**TMNT 检查（同日，APK `44682188…`，提交 `714e16af` 之后，`tmnt-multiblock/`）**

- **multiblock 开（poll 开），PID 12120**：
  - 约 30 秒到主菜单（60 FPS），"开始游戏" → "继续冒险" 进入存档里的下水道关卡；进场即在战斗中，58 FPS。
  - 用 DebugBus 手柄连续攻击、移动。角色在这场战斗中阵亡，游戏正常回到巢穴（此前记录过"战斗/死亡后堆错误"的路线），59–60 FPS，之后可以移动。
  - 界面 Stop 约 1 秒内完成，`user_stop`。
- **同一位置对比**（"继续冒险" → 巢穴，走相同的两段移动后静止采样 15 秒；帧率都封顶 60，所以比较线程 on-CPU）：

  | 配置 | FPS | `Guest-1` ms/帧 | `GpuComm` ms/帧 |
  |---|---|---|---|
  | `MULTIBLOCK=0`（PID 14473） | 59.59 | 8.5 | 5.2 |
  | `MULTIBLOCK=1`（PID 15905） | 59.77 | 7.5 | 5.4 |

  每种配置只采了一次，只能说明方向，不是严格 A/B。
- **故障与日志**：三个进程的 `fex-fault-<pid>-1.txt` 都是空文件，host 日志没有 guest 故障。日志里的 Error 都是已知的离线类（奖杯解锁失败、PlayFab/HTTP 离线、AvPlayer 恢复失败）。每次 Stop 都在约 1 秒内完成，`user_stop`，guest 返回值与以往 TMNT 停止时相同。
- **副作用**：存档里原本 17 分钟的下水道冒险因阵亡结束，现在存档停在巢穴（冒险时间 0s）。
- **未覆盖**：长时间关卡、Boss、多人（沙发合作），以及其他游戏。
- 结束后属性已清空。

## 12. 后续（同日）

multiblock 已改为默认开启；§9 的"逐 draw 绑 shader HLE"已做成 guest 快路径，剩余 HLE 编码器也做了减负。
实现、逐字节对照、Swan 上的 CPU A/B 与期间遇到的（与快路径无关的）Swan GPU fault 见
[gnm-fastpath-20260928.md](gnm-fastpath-20260928.md)。
