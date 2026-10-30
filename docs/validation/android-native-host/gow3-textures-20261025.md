# GoW3 新区域条纹贴图：回放、纹理分配耗尽与扩容候选（2026-10-25～26）

最新进展：Android 干净会话回放复现相同条纹，24 次等待无强制放行、0 divergences。随后在原版游戏中自然捕获两次新区域纹理分配耗尽（45 + 59 次）：分配器返回 0、释放新记录却不清父对象指针，调用者继续使用；每批失败父对象共用了同一个已释放记录。已构建按游戏 SHA 绑定的 2.25 GiB video arena 候选 `3da72970`，仅复制到设备，尚未选择/运行，不能称已修复。详细新证据见末尾“10-26 续查”。

以下为 10-25 首次现场记录，其当时的推测与未执行项目由末尾续查更新。

## 现场与边界

- Odin3 `6edb0593`，`com.shadps4.android`，PID **13851** / generation **2**，UUID `70e66592b20a73fbea91baa71af8f515`；本轮前后身份一致，仍 Resumed。
- 游戏 `CUSA01740` / `emupsp-CUSA01740`，Olympus/Hermes 区域。用户确认：“一直在前台，进入新区域后出现”。日志中另有 22:19–22:48 后台暂停，但不能将其直接认作触发条件。
- 已装 APK SHA256 `607e97614cf556ca157dd21f2b3b3ee8dfa4b7ed3c0f2a30c28ce5e580652143`，录制 Info build `v.0.18.0-656-g22bd5a9d7-dirty`，分支 `feature/malos/swan_performance`。
- Turnip `d7e6746a5eb02500f8d4ac3022ae8459a3e683b56ba55b6dc2705b5706f46099` / Mesa `68dafb89a2`，Adreno 830。未安装另一个任务构建的 `c4185b53` 候选 APK。
- 实际录制设置：scale **75%**、readbacks **Precise (2)**、linear **1**、completion fences **0**。游戏 profile 与 `.bak` 前后内容一致。
- 原现场 `async_writeback=all`、corner views / texture bind cache / stream reuse 开；stream submit=100。本轮保留这些设置，仅短暂开启 SRT 双路径核对后恢复 `plan`。
- GPU faultcount 前后 **15**；未见本次采集新增故障。录制前 MemAvailable 约 12.3 GB。不能仅凭这些排除所有 GPU 或资源生命周期问题。
- 未停止/重启游戏、安装 APK、操作人物、改写 guest 内存或修改生产代码。录制本身会短暂停顿，不能把录制期间耗时用作正常游戏性能。

## 证据文件

本地目录：`build/validation/gow3-textures-20261025/`，原始内容只保留在本地与设备，不入 Git。文件清单、SHA 和身份见该目录 `manifest.json`。

| 文件 | 用途 / 验证 |
|---|---|
| `gow3-stripes-20261025-2301.sgpurply` | 783,866,086 B（747.55 MiB），3 帧，174 Submit / 174 Resume、24 WaitPoll、3 Flip；初始 12 VMA / 1,161,640 页，`untracked_bytes=0` |
| `frame_00000.png`–`frame_00002.png`、`frames.txt` | 同次录制的设备原帧，1440×810；肉眼确认第 0 帧保留墙面/地面条纹，人物与 HUD 相对正常。不是另一次 replay 的输出 |
| `before.png`、`after-verification.png` | Android 原生截图，1920×1080；核对前后异常仍在 |
| `c02d6bfea1883d2b50511bcf71cb1b5a.gcmdtrace.ps4` | 稍后 2 帧资源/动作 trace，4,663,695 B；与 replay 不属于同一帧窗口 |
| 同 UUID 的 `.gpu.pm4.trace` | PM4 trace，6,761,337 B；85,383 packets / 3,667 actions，trace 状态 ready |
| `texture-inventory.json`、`inventory.py` | 各帧 BC 纹理地址、尺寸、格式与使用次数；解析无 problems |
| `raw-sharp-scan.json`、`raw-sharp-recheck.json`、`scan_raw_sharps.py` | 原始初始 guest MemoryPages 中的 T# 候选扫描与可重复脚本 |
| `suspect-pages-{initial,final}.bin`、`suspect-memory.json` | 可疑地址所在 128 KiB；32 页均覆盖，三帧内该范围没有记录到增量更新 |
| `android-host-full.log`、状态文本 | 当前进程日志和前后状态；日志含早先 KH3 gen1，不能全部归到 GoW3 |

Replay SHA256（设备与本地一致）：

```text
26bd9a719ad747ad2129e2b5a62e397a4f328157d44ecbbc717d6cb30d1d07e6
```

`sgpurply.py check` 遍历完整文件通过，最终 **383 个控制事件 / 3 帧**与 End 记录一致。DebugBus 的状态进度计数为 384、流结束前 Info 为 382；这份文件的完整性以最终 End 和解析器计数为准，不把不同采样时点计数误判为文件损坏。

设备原帧 XXH3：

```text
0 76cf8341c0248ec1 1440 810
1 a1042de898ebbb58 1440 810
2 0c54e79aab280fe4 1440 810
```

## 定位结果

### 1. 活跃 draw 的同地址异构纹理

PM4 资源 trace 的两个帧窗口分别为 flip 236453 / 236454，1366 / 2301 个动作。BC 纹理中 1289 / 2022 次绑定，共 **3311 次**，指向 `0x2e1ff3900`：

| 格式、形状 | 第一帧 | 第二帧 |
|---|---:|---:|
| BC1，512×512，9 mips，2D | 490 | 714 |
| BC3，128×128，7 mips，Cube | 314 | 510 |
| BC3，512×512，9 mips，2D | 129 | 185 |
| BC1，1024×1024，10 mips，2D | 289 | 540 |
| BC1，512×1024，9 mips，2D | 34 | 38 |
| BC1，256×512，8 mips，2D | 24 | 26 |
| BC3，256×256，8 mips，2D | 9 | 9 |

全部 tiling=13。同窗其他 BC 地址大多有一致的描述，例如 `0x2e0a3ad00` 一直是 BC3 1024²。这些是绑定次数，**不是独立纹理数**。

资源 trace 来自 `Rasterizer::TraceAction` 中的 `image.GetSharp(*stage)`，记录的是解析出的 guest T#，而不是最后选中的 Vulkan Image。其来源是 `flattened_ud_buf`，所以进一步核对了原始 guest 内存和 SRT 执行路径。

### 2. 原始 guest 内存独立佐证

扫描 replay 的初始 MemoryPages（在 BeginStream 前，不扫描 Vulkan 资源）找到 **8370 处 T# 候选**指向同一地址；其中 **8267 处**的描述与上述 7 类活跃绑定一致。示例：

```text
guest VA 0x200305bec, BC1 1024×1024 / 10 mips / 2D / tiling13
39ffe10200003002ffc3ff002409d99600e07f00000000000000000000000000

guest VA 0x200306434, BC1 256×512 / 8 mips / 2D / tiling13
39ffe10200003002ffc07f00ac0fd79600e01f00000000000000000000000000
```

扫描只查 4 字节对齐、页内 32 字节描述，包含历史/非活跃副本；不能将总数作为活跃纹理数量。原始内存快照与稍后 PM4 不同窗，但二者同一会话、同一异常场景，地址与 7 类描述一致。

目标地址在已映射 Direct VMA 内，内容非全零/统一填充值，字节形态与 BC 数据相符；不能仅据字节认定它是正确的完整纹理。不同格式、尺寸及 cube 持续共用该数据，符合未完成纹理资源替换或地址回填异常的表现。**“共用占位资源”仍是假设，没有识别具体游戏资源对象或写入函数。**

### 3. SRT 与其他检查

`srt_plan verify` 同时运行 prepared plan 和解释器，约 3 秒内 **786812 次比较、0 mismatches**，随后恢复 `plan`；收尾再次读取确认。此结果没有发现 prepared SRT 走表的差异，不代表排除所有并发时序问题。

采集时 pipeline cache 的 lossy skipped draws / frames 均 0，不能用“PSO 跳过绘制”解释这批样本。GPU 原帧也有条纹，表明异常不只在 Android 最终合成层。

现有写回保护正在工作：`kept_foreign=40`、`kept_bytes=151`、`unsnapshotted=0`，但为进程累计统计，含之前 KH3。日志中的 GoW3 保护主要在 `0x2b9a02c00` 小目标，不能据此指认是可疑纹理地址被覆盖。

本次 22:48–22:57 仍有 Oly100/210/230/275/300/325/350/375 等 WAD 加载日志，未检出明确短读/ENOMEM/加载失败证据。与 [10 月 17 日同类条纹](../online-content/gow3-launch-20261015.md)相似，但不能沿用当时“关卡包不再加载”的判断。许多 guest stderr 被宿主以 Error 级别记录，本身不证明加载失败。

`gpu_images list` 只返回 6936 个缓存图像中的截断子集（`truncated=1`），子集中未找到目标地址；不能据此断言目标图像不在缓存。

## 10-25 23:10 时的 replay 交接（历史状态）

完整 trace 已就绪，但本轮 Windows SSH 主机连接超时；没有执行远端 replay。当前 Odin3 游戏仍在运行，未为了 replay 退出它。

桌面使用对应功能的构建，在独立输出目录执行：

```text
shadps4.exe --gpu-replay <trace完整路径> --gpu-replay-out <独立输出目录> --gpu-replay-exit
```

先检查 `replay_summary.txt` 的 complete / divergences / skipped pages，再与本次 `frame_00000.png`–`00002.png` 比较；跨 GPU 不能预设逐像素哈希相同。需要继续缩小首次异常 draw 时，再启用 `--gpu-replay-hash-images`，然后用 `--gpu-replay-hash-draws <事件号>`。具体参数见 [replay 验证文档](gpu-replay-20261004.md)。

Android 原文件保留在：

```text
/data/user/0/com.shadps4.android/files/host/captures/gpu_replay/gow3-stripes-20261025-2301.sgpurply
```

以后在用户保存并退出当前局后，可设 `debug.shadps4.gpu_replay=gow3-stripes-20261025-2301`，正常启动会话走 replay；结束必须清空该属性。**本轮未设置该属性，收尾确认仍为空。** Android 参数见 [Android replay 用法](android-gpu-replay-mhr-20261005.md#3b-android-端回放新增)。

GPU replay 能验证异常输入在干净渲染器中的输出，**不能重演录制开始前的 CPU 资源加载过程**。若 replay 仍呈现同类条纹，下一步需要好/坏状态对照：在保存后重新加载同一区域，比较对应材质 T# 和资源数据；跟踪上述 descriptor 基址字段的写入，沿资源任务、WAD 读取/解压、GPU 写回定位首次偏离。不能靠直接替换此地址或清缓存掩盖问题。

本轮收尾：replay `finished`，PM4 `ready`，无采集继续运行；SRT 恢复 plan，profile 未变，GPU faultcount 仍 15，同一游戏 PID/gen 保留。设备原始录制与配对 PNG 有意保留供后续 replay，约 0.8 GB；没有 debugger/forward 会话待清理。

## 10-26 续查：自然分配失败与候选

### 回放与重新加载

用户保存并明确允许退出后，结束原会话，在 Android 新会话 PID15377
回放同一份文件。`replay-baseline/replay_summary.txt` 为 `complete`，24 wait polls、
0 forced waits、0 divergences、3 recorded flips，约 0.502 s processor wall time。
三帧输出仍有同类条纹。输出哈希与录制原帧不同，没有逐像素一致声明。
这证明异常输入在干净渲染器中仍重现；回放不重演此前 CPU 资源加载。

原始 SELF 与解密 ELF 已导出并分别校验：

- SELF `6e72330e9092e5164049b865e5568f2d450904001a73fe0a2f4b39194c195b3c`
- ELF `67e41b61cb2439b382976107b80e071edfd4c2951ff4cf1320e9361b5662b731`

为 IDA 工具生成的 `gow3-67e41b61-analysis.elf` 仅改 ELF 容器与地址基准，
加载段内容未改；SHA `62bff3817b032cc24db641ec1b62ea7a76ebabb32837c9cc5eb0af00ddde444a`。
以下函数地址是 guest VA（模块基址 `0x400000`），不是 RVA。

重新加载最新“聖火上層”存档，最初没有同类错贴；它比原录制的 Hermes 场景更晚，
不能称同一镜头 A/B。随后进入走廊及圣火大厅时自然再现分配失败。

### 实际失败链

当前会话 PID17444/gen1，UUID `9c9bf596d2917a3cf2a5a1a16a9ef716`。
只读观察包 `gow3_vram_observe_v1`（SHA `dee1e72a…`）转发原始四参数及结果，
没有改变分配、预算、回收或加载行为。包日志的 `generation` 是 patch 调用元数据，
不可替代 session generation。

| 时间 | 失败次数 | 第一笔请求 | 当时尾部可用 | 父对象残留的共同记录 |
|---|---:|---:|---:|---|
| 00:00:10，加载 Oly150 | 45 | 353,280 B | 226,816 B | `0x204424800` |
| 00:10:02，后续区域 | 59 | 351,232 B | 31,744 B | `0x204425100` |

两批完整日志共 **104 次**，结果均为 0，旧 parent slot 均为 0，失败后却留下
非零指针。第一批请求合计 15,823,872 B，第二批 11,589,632 B。
第一批 45 个 parent 在后续只读内存检查中仍全部引用同一个记录；该记录已复用成
slot805 / size351,232 / refcount1。原异常录制中也找到了 73 个 parent 共用
slot608 / size48,640 / refcount1 的同类状态。

具体实现链：

1. `0x5c8a90` 为新纹理取一个 pool record，先写入 `*parent_slot`。
2. `align256(used)+size > capacity` 时释放该 record，返回 0，却不清空 slot。
3. `0x5c72b0` 没有检查零返回，直接读取 slot 的 record、继续初始化/上传。
4. 后续分配反复复用同一空闲 record，多个不同尺寸纹理引用同一地址。

这直接解释了原 trace 中 BC1/BC3、2D/cube 等互不兼容描述符共享一个数据地址。
不是根据一条日志猜测缓存失效。原版 streaming texture capacity 实际为
713,662,464 B（680.602 MiB），第一次 used 为713,435,648 B。
之后回收能将 used 降下来，但已污染的 parent 指针不会随之恢复。

### 候选实现及验证边界

[源码与构建方法](../../../guest/games/CUSA01740/01.02/README.md)。
`renderer_init` hook 在 `0x5ea380` 进入原始初始化前，将原始 video arena 全局值
从 `0x50000000` 调到 `0x90000000`。只接受已知原值；若已被其他设置修改，保留它并记录。
`0x5f0b80 → 0x5a0980` 仍实际调用 DirectMemory 分配/映射；随后原始 heap 和
texture arena 建立流程消费同一个大小。没有直接扩写现有 texture capacity，
没有越过 GPU fence 强行 compact，也没有隐藏分配失败。

包保留原分配器观察，原子累计调用/失败，仅记录首次分配及前64次失败。
此方案与桌面既有2.25 GiB方向一致，是容量规避，**没有修复原游戏在再次耗尽时的
通用失败处理缺陷**。10-15 在不同回读设置下试过2.25 GiB却仍有启动错贴，
不能据此否定此次已经直接观察到的独立容量问题。

已完成 freestanding x86-64 构建及反汇编核对；原 SELF SHA、两入口原字节、
三个数据绑定的权限/对齐均与导出 ELF 一致。离线比较不是实机 loader 负对照。
包 SHA256：`3da7297001b5c5cb71e2738d4af11280b6f192f184b107b5ca933e2f4d55a250`。
已复制到设备 `files/host/guest_patches/CUSA01740/gow3_texture_arena_v1.json`，
**未选择、未运行**；尚无候选真实内存容量、同路线画面或长跑通过结论。

### 当前设备与待验步骤

观察到用户在重载后继续推进游戏，故停止发送输入，保留当前局；已询问新的保存/退出时机。
这段新进度不包含在此前退出授权的存档内。下一步在允许退出后加载候选，先验证
DirectMemory实际映射与texture capacity，再重走聖火上層→走廊→圣火大厅，
核对零失败、材质地址/画面和正常 Stop。旧异常 replay 仍用于输入/渲染对照，
不能用它证明容量补丁解决了加载前的失败。

APK仍为607e9761、Turnip仍为d7e6746a；未重新安装 APK。GPU faultcount仍15。
游戏profile未改，GPU replay/guest debug启动属性均已清空。
三个 guest debugger 尝试均已关闭/cleaned，无当前 debugger/forward：暂停后软件断点
返回E16；另一startup-wait会话停在stopEpoch0拒绝修改。之后使用同UID只读
`/proc/<pid>/mem`，未绕过工具写断点或写进程内存。
当前仅保留观察包的开发override；扩容候选未启用。

新增本地证据：`replay-baseline/`、`allocator-probe/natural-failures-final.{log,json}`、
`allocator-probe/failure-summary.json`、`natural-alias-parents.json`、
`natural-failure-screen.png`、`live-allocator.jsonl`、`vram-allocate.c`、`compact.c`、
`renderer-init.c`、`direct-video-allocation.c`、`area-loader.c`、`arena-candidate/`。
原始游戏字节、反编译内容和设备日志均不入仓。
