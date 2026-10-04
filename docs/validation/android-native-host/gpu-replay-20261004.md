# GPU 确定性回放：桌面实现与血源验证（2026-10-04）

设计见 [gpu-replay-20261004.md](../../specs/gpu-replay-20261004.md)。本文记实现、用法、实测数据和回放中发现的着色器翻译问题。

环境：Windows，clang-cl RelWithDebInfo，RX 7600M XT（Thunderbolt 外置），血源 CUSA03023 1.00，中央亚楠出生点。测试在 `D:\workspace\shadps4-win-test`（每轮先把 `shadps4-win-bb\user\home` 同步过去），玩家目录未动。回放实现的最终 exe `954e02f6`；加上 §5 着色器修复后为 `1dc97968`。全部为本地未提交改动。

---

## 1. 结论

| 步骤 | 状态 |
|---|---|
| R0 GPU 驱动对象移入 Guest 内存 | 完成。label、3 个内嵌 shader、7 个 init 序列在 `0xFE0000000`（64 KiB，`GpuDriverObjects`），血源正常进世界 |
| R1 trace 容器与初始快照 | 完成 |
| R2 写跟踪与事件流 | 完成。抓帧期间游戏不崩溃，结束后帧率恢复 |
| R3 回放进程 `--gpu-replay` | 完成。不运行任何游戏代码，回放出的画面就是游戏画面；事件逐个对上，0 分歧 |
| R3 “两次回放逐帧哈希一致” | **修复后达到**。原先每帧约 0.5% 像素不同，原因是 alpha-test 后的分支按像素分歧，块内隐式 LOD 采样的导数未定义（§5）。改为在 kill 处把被杀像素 demote 成 helper 后，bb-r3a 三次回放 3 帧哈希相同、两次逐事件图像哈希 1770 行全同；bb-final 两次回放 5 帧哈希相同、2210 行全同（§4.5） |
| R4 定位工具 | 逐事件、逐 draw 的图像哈希已有，用它定位到了上面的问题；RenderDoc 抓回放帧未做 |

---

## 2. 用法

抓取（游戏运行中，DebugBus）：

```bash
python scripts/debug/debugbus.py "gpu_replay_capture 30 my_trace"
python scripts/debug/debugbus.py "gpu_replay_status"
```

trace 写到 `user/captures/gpu_replay/<name>.sgpurply`。`gpu_replay_cancel` 取消。

回放（不加载游戏；设置读用户目录下的配置）：

```bash
shadps4.exe --gpu-replay user\captures\gpu_replay\my_trace.sgpurply --gpu-replay-exit
```

- 输出目录默认 `<trace 名>_replay`（`--gpu-replay-out` 指定）：`frame_NNNNN.png`（超分前的 guest 图像）、`frames.txt`（每帧 XXH3）、`replay_summary.txt`。`--gpu-replay-no-png` 只算哈希。
- `--gpu-replay-hash-images`：每个事件后读回期间被 GPU 写过的图像并算哈希，写 `image_hashes.txt`；加 `--gpu-replay-hash-draws <事件号>` 时该事件内每个 draw / dispatch 后都算一次。很慢，用来对比两次回放。
- 读取：`python tools/gpu-replay/sgpurply.py info|vmas|events|check <trace>`。

回放固定的设置：`pipeline_compile_mode=sync`，内部分辨率、readbacks 按 trace 头和 Info，copy_gpu_buffers 关。

---

## 3. 实现概要

**抓取**（`video_core/replay/gpu_replay_recorder.*`）
- 在一帧的最后一轮处理结束、GpuIdle 放行提交门之前开始：`Finish`，写回 GPU 独有结果（buffer `gpu_modified_ranges`、可下载的 GPU 修改图像、GDS），然后对可写且不可执行的 Direct/Flexible/Pooled VMA 设录制写保护，再经后备视图拷贝全部页（零页只记地址），写 Liverpool/环形队列/GnmDriver/VideoOut/GDS 状态。
- 写跟踪：PageManager 新增每页 1 位的录制位图（32 MiB，按需提交），并入页保护计算；写故障清位、放行；`RecorderWriteFaults` 计数用于跳过无写入的边界。
- 边界：每次顶层 resume 之前、每次等待求值之后、注入命令执行之前、每轮结束。边界上处理映射变化（`Mapping` 记录：范围内现在的 VMA 列表；Windows 拆分 placeholder 会丢失邻接区域的保护，邻接 VMA 整体重新保护并拷贝）、扫描清位的页、先重新保护再拷贝、输出已登记的 EOP flip。
- 捕获期间注入命令只在 resume 之间执行；VideoOut label 等待改为让出；copy-shader HLE 的结果写回改为同步（`DeterministicGpu()`）。
- 栈：Windows 在故障线程的栈上写异常记录，栈页不能保护。游戏用 `pthread_attr_setstack` 提供的栈与 fiber 栈（开始时枚举线程、之后在 `CreateStack` 与 `_sceFiberSwitchToFiber` 通知）都排除。

**回放**（`video_core/replay/gpu_replay_player.*`，`Emulator::RunGpuReplay`）
- 从 trace 头恢复标题号、SDK、Neo、extra dmem/fmem 与直接/灵活内存规模（`SetupReplayRegions`），创建窗口和 GPU 栈（`InitHLELibs`）。
- 直接内存按记录的物理地址 `Allocate` 后 `MapMemory(Fixed)`；Flexible 按原 VA；Pooled 当作 direct 映射。页经后备视图写入。Liverpool 状态（寄存器、57 个 cs_state、CE RAM、环形队列按原 slot）、VideoOut 端口与缓冲（直接设字段，不走会清 label 的注册接口）、GDS 逐项恢复。
- Liverpool 的 GPU 线程改由事件驱动（`RunReplay`）：Submit 建任务（记下抓取时的提交号），Resume 恢复该队列队首任务并核对提交号，等待包经 `Poll` 先应用此前的写入再求值、取记录的结果，Command 执行 CPU flip / readback，BurstEnd 做 OnSubmit+Flush 并发 GpuIdle。增量写经后备视图写入后 `InvalidateMemory`。
- 回放中 present 线程不复位 flip label（label 只由 trace 驱动）；纹理 GC 不按驱动报告的显存压力驱逐 GPU 修改过的图像。

---

## 4. 实测

### 4.1 R1 初始快照（bb-r1）

- 21 个 VMA：Direct 4.5 GiB、Flexible 411 MiB、File 64 KiB；1,280,104 页（4.9 GiB），其中 458,327 页为零（35.8%）。
- 写回 853 ms，快照 2.7 s；原始 3.38 GB，zstd level 1 后 1.51 GB。

### 4.2 R2 事件流

| trace | 帧 | 事件 | 提交 | 等待求值 | 写故障 | 增量页 | 边界耗时 | 文件（原始→存储） |
|---|---|---|---|---|---|---|---|---|
| bb-r2a | 3 | 1,130 | 207 | 704（全部一次满足） | 48,948 | 48,190（188 MiB） | 651 ms / 916 次 | 3.58→1.56 GB |
| bb-r2b | 30 | 11,377 | 2,073 | 7,121 | 502,878 | 495,648（1.9 GiB） | 6.3 s / 9,242 次 | 5.45→2.00 GB |
| bb-final（最终 exe） | 5 | 1,886 | — | — | 87,220 | 85,976 | 1.15 s | —→1.59 GB |

- 提交数核对：2073 = 30 次驱动 init 序列 + 2043 个游戏 DCB（68.1/帧）；独立的 `pm4_stats` 测得 67.63 DCB/帧。
- 抓 30 帧后 5 s 内 274–301 flip（55–60 FPS），与抓取前相同；最终 exe 抓取前 275、抓取后 282。
- `sgpurply.py check`：End 记录的事件数、帧数与流中一致，提交在恢复之前出现。

### 4.3 R3 回放

- bb-r3a：`result=complete`，1,148 个控制事件、730 次等待求值、0 次强制让出、0 分歧，3 帧 960×540（内部分辨率 50%）。画面为中央亚楠出生点，角色、HUD 正常。恢复初始状态约 10 s，回放约 5 s。
- 两次回放的帧哈希不同：每帧 2,404–2,746 个像素不同（约 0.5%），多数只差 1，最大 13–60，零散分布在铁栅栏、植被等边缘。
- 回放侧去掉两个时序相关的写入后差异不变：纹理 GC 按显存压力驱逐（回放时总占用记 0）、copy-shader HLE 异步写回（改同步）。

### 4.4 定位

1. `--gpu-replay-hash-images`：两次运行的第一处不同在同一个图形提交（深度 + 6 张 960×540 MRT 的 G-buffer pass）。深度相同，MRT0（RGBA8）、MRT4（R11G11B10F）、MRT5（RGBA16F）不同。两轮复现位置一致。
2. `--gpu-replay-hash-draws`：该提交的第 33 个 draw（VS `0x6a833138`，PS `0x1c3953c6`）首次出现不同。它首次绑定的 6 张 BC 纹理两次哈希一致，深度一致。
3. 诊断：把隐式 LOD 采样临时改为显式 LOD 0（只在本地，验证后已撤回），两次回放 3 帧哈希相同、1770 行逐事件图像哈希全部相同。

### 4.5 修复后（exe `1dc97968`）

- 转换范围：bb-r3a 回放中 100 个片元着色器有 15 个是“存活掩码 + WQM + kill”模式，全部被识别转换（含 `0x1c3953c6`），用反汇编扫描核对没有漏判或被拒的。`0x1c3953c6` 的 IR：kill 处 `DiscardCond`，原分支条件变为 `ConditionRef #true`，末尾的 `DiscardCond` 保留。
- SPIR-V：这次回放导出的 235 个模块 `spirv-val --target-env vulkan1.3` 全部通过。
- 一致性：

| trace | 运行 | 帧哈希 | 逐事件图像哈希 | 等待求值 / 分歧 |
|---|---|---|---|---|
| bb-r3a（3 帧） | 3 次（含一次导出着色器的运行） | 三次相同 `3582f3fa…` `e9f8d47f…` `2280f0f6…` | 两次 1770 行全同，无跳过的图像（BC 纹理也按块哈希） | 730 / 0 |
| bb-final（5 帧） | 2 次 | 相同 | 2210 行全同 | 1175 / 0 |

- 与修复前比：每帧 5.3–7.4% 像素不同，集中在 alpha-test 几何（两侧植被、尖塔、铁门装饰、灯、地面碎物）。放大对比，修复前叶片边缘的亮点在修复后明显减少，结构与轮廓一致。与显式 LOD 0 诊断版大面积不同（那是诊断用的错误 mip，不是参考）。
- 游戏内：同一出生点 5 s 内 297、304、297 次 flip（60 FPS 上限）；游戏截图中植被、铁门、地面植物正常；日志无新错误。
- 其他游戏：怪物猎人世界（CUSA09554）运行 2 分钟，停在加载画面前已有 3 个片元着色器被转换（kill 后不再回到 WQM 的形状），导出的 44 个 SPIR-V 全部通过校验，未崩溃。这次运行的日志在 2.5 分钟内写了 67 MB，来自 `kernel/memory.cpp` 每次直接内存分配/映射的 Kernel.Vmm Info（MHW 约每秒 500 次）和 GameLiveStreaming/SharePlay 桩函数每帧的 Error，与本修复无关。随后按用户要求清理：Kernel.Vmm 逐次调用的参数/结果回显删除（失败另有带参数的 Error，映射/解映射/保护在崩溃报告的映射历史里）；`Free` 顺带解除的映射原先只有这条 Info，改为记入映射历史（`unmap-free`），同时通知 GPU 回放录制器；两个每帧轮询的桩和 Audio3d `PortSetAttribute` 降为 Debug，与同类函数一致。MHW 复测（exe `f25ada65`）：启动后 74 秒内共 1,600 行、270 KB，之后不再增长，全是启动阶段的一次性日志（Kernel.Vmm 只剩 5 行内存区域信息）；游戏照常推进到“已获得的追加内容”对话框。

---

## 5. 发现与修复：alpha-test 后的分支使导数未定义

PS `0x1c3953c6` 的 GCN（节选）：

```
s_mov_b64 s[76:77], exec          ; 存活掩码
s_wqm_b64 exec, exec              ; 整 quad 模式
...                                ; alpha 测试 → s[0:1]
s_andn2_b64 s[76:77], s[76:77], s[0:1]   ; 杀掉不过的像素；SCC = 整个 wave 是否还有存活
s_cbranch_scc0 exports            ; 整个 wave 都被杀才跳过
s_and_b64 exec, exec, s[76:77]
s_wqm_b64 exec, exec              ; 存活像素所在 quad 的其他像素作为 helper 继续
image_sample ...                  ; 隐式 LOD，需要 quad 内的导数
...
exports: s_mov_b64 exec, s[76:77]; exp mrt0 ... done vm   ; 被杀的像素在这里丢弃
```

GCN 上分支是整个 wave 一致的，块内 exec 为 WQM(存活)，被杀像素在存活 quad 里作为 helper 参与导数。

翻译后（`frontend/translate/scalar_alu.cpp`）：
- `S_ANDN2_B64` 等 64 位掩码运算 `SetScc(InverseBallot(result))`，SCC 是“本 lane 的位”；
- `S_WQM_B64` 为空操作；
- 结构化后成为 `If (本像素存活) { 块 }`，被杀像素不进入块，最后 `Discard(!exec)` 丢弃。

于是块内的 `image_sample` 落在 quad 内非一致的控制流里，Vulkan 规定此时隐式导数未定义；AMD 上不活跃 lane 的寄存器残留值参与导数，mip 选择随运行变化，输出差 1 左右，集中在 alpha-test 的边缘。这同样影响正常游戏（边缘闪烁），不只是回放。

写操作不受 exec 保护（只靠控制流），所以不能简单把条件改成 quad 一致。

**修复**（`Translator::FindLiveMask`，`translate.cpp`；`S_AND_B64`，`scalar_alu.cpp`；用户确认后实施，改变所有游戏中这类片元着色器的翻译）：
- 识别条件，全部满足才转换：片元着色器；第一次改 EXEC 是 `s_wqm_b64 exec, exec`，在它之前有 `s_mov_b64 mask, exec`；此后对 `mask` 的写只有 kill（`s_andn2_b64 mask, mask, x` 或 `s_and_b64 mask, mask, x`，按 SGPR 实际写入宽度检查，含 SMEM 多 dword 加载）；每个带 `vm` 的 export 前最近一次改 EXEC 是 `s_mov_b64 exec, mask`（或 0）或与 `mask` 相与，中间没有分支目标、没有再改 `mask`；没有 `s_setpc`/`s_swappc`/`s_movreld`/fork。至少一次 kill。
- kill 处：对本像素位已清零的调用 `Discard`，即 `OpDemoteToHelperInvocation`（被杀像素最终本来就会在 `vm` export 处丢弃）；SCC 设为真。原方案写的是 `存活 || IsHelperInvocation()`：识别条件保证此后仍在执行的调用要么存活、要么是 helper（光栅化 helper 或刚被 demote 的），这个式子恒为真，所以直接取真；也避开了 demote 之后读 `HelperInvocation` 内建变量的未定义值和局部 CSE 合并问题。
- 紧跟 `s_wqm_b64 exec, exec` 的 `s_and_b64 exec, exec, mask`（kill 后回到 WQM）不清掉 helper 的 EXEC，被杀像素继续参与后续 exec 分支内的导数，与 GCN 一致。不接 WQM 的（exact 模式，如写 UAV 前或最后 export 前）保持原语义。
- 不符合条件的着色器翻译不变。着色器缓存 `ShaderBinaryVersion` 36 → 37。
- 代价：全被杀的 quad 现在也执行 kill 之后的代码（作为 helper，不写内存和颜色）；Vulkan 允许驱动提前结束全 helper 的 quad。桌面帧率未见变化；Android（Turnip）未测。
- 识别成功的着色器在 `Render_Recompiler` Debug 级别打一行 `live pixel mask …`。

---

## 6. 未覆盖与限制

- 只验证了血源桌面、图形队列、3–30 帧；映射变化、CPU flip、readback、计算环形队列的回放路径已实现但未经实测（这几段画面里没有出现）。
- 抓取时未固定设置（本轮抓取用的是 `async_graphics_skip`、内部分辨率 50%），回放固定为 sync。
- Android（FEX guest 内存后端）不支持抓取。
- 第一帧的起始状态来自写回，模板、MSAA、压缩元数据不写回。
- 回放不支持 VR 帧。

---

## 7. 改动文件

- 新增：`src/video_core/replay/gpu_replay_{file,format,hooks,recorder,player,frames}.*`、`src/video_core/amdgpu/liverpool_replay.cpp`、`tools/gpu-replay/sgpurply.py`。
- 着色器修复：`shader_recompiler/frontend/translate/{translate.h,translate.cpp,scalar_alu.cpp}`、`shader_recompiler/recompiler.cpp`、`vk_pipeline_serialization.cpp`（缓存版本）。
- 修改：`liverpool.{h,cpp}`（Poll 模板、ResumeTask、RunReplay、提交记录）、`page_manager.{h,cpp}`（录制位图与写故障）、`memory.{h,cpp}`（映射快照、后备读取、映射变化通知、回放内存规模）、`gnmdriver.{h,cpp}`（驱动对象、状态、GetLiverpool）、`videoout/driver.*`、`video_out.*`（状态保存/恢复、EOP/CPU flip 回放、回放不复位 label）、`buffer_cache.*`（写回、GDS 读写、readback 命令记录）、`texture_cache.*`（写回、图像哈希、回放时 GC）、`vk_shader_hle.cpp`（确定性写回）、`vk_presenter.cpp`（逐帧读回）、`vk_rasterizer.*`（逐 draw 钩子）、`fiber.cpp`、`stack.cpp`、`platform.h`、`slot_vector.h`、`diagnostics_commands.cpp`、`emulator.*`、`main.cpp`、`CMakeLists.txt`。
