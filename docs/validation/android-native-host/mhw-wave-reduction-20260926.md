# MHW GPU 超时：整 wave 归约在分歧控制流中读到未定义 lane（2026-09-26）

分支 `feature/malos/mhw_fix`（基于 `malos/main` `b4972cbd`），本地未提交。接续 [09-25 trace 与 SGPR/VCC 掩码修复](mhw-gpu-trace-20260925.md)：那一轮之后 MHW 仍在进入游戏后约两分钟 GPU 超时，最后定位到的死循环候选是 compute shader `0xa38aae6c`（local size 32×2×1）。

## 1. 现场

AYN Thor `9c2841a4`，mainline Turnip（`debug.shadps4.vulkan_driver=turnip-mainline`）。当前构建（含上游追平）PID16383 关闭两个网络提示后仍 DeviceLost：

- Turnip：`tu_knl_kgsl.cc:1796: GPU faulted or hung (VK_ERROR_DEVICE_LOST)`（本轮给 release 驱动注册了 `VK_EXT_debug_utils` messenger，驱动的 device-lost 原因才进入日志）。
- 内核：`gpu timeout ctx 15 ts 29371`，此前约 6 秒有 5 条 `CP: AHB bus error, CP_RL_ERROR_DETAILS_0:0x10008e07`（寄存器 0x8e07 = A7xx `RB_CCU_CNTL`）。AHB 错误与超时的先后关系未证实为因果；KGSL snapshot 已被读走（`objects released`），本轮没有拿到快照。
- 桌面（RX 7600M XT）09-26 12:50 的 MHW 运行同样以 `GPU_SUBMIT_FAILED result=ErrorDeviceLost` 结束，说明不是 Adreno/Turnip 专有问题。

## 2. 根因：整 wave 归约惯用法

设备上已有的 09-25 shader dump（1002 个文件）中，20 个 MHW shader 含 PS4 编译器的整 wave 归约（19 个 compute + 之前 DeviceLost 相关的 FS `0x15e44dcc`）。`0xa38aae6c` 的 GCN（自写的最小反汇编脚本，基于 `opcodes.h` 表）：

```text
0x9d4  s_and_saveexec_b64 vcc, vcc        ; exec = _868 lanes
0x9d8  s_cbranch_execz -> 0x129c          ; 余下整个程序只由 _868 lane 执行
...
0xc40  s_cmp_lg_u32 0, vcc_lo / s_cbranch_scc1 -> 0x123c   ; waterfall 循环
0xc48  s_orn2_saveexec_b64 vcc, exec      ; 暂时启用全部 64 lane
0xc4c  v_cndmask_b32 v6, -1, v4, vcc      ; 非活动 lane 填单位元 -1
       ds_swizzle xor16/8/4/2/1 + v_min_u32 ×5
0xca0  v_readlane_b32 s4, v6, 31
0xca4  v_readlane_b32 s5, v6, 63
0xca8  s_mov_b64 exec, vcc
0xcac  s_min_u32 s43, s4, s5              ; 本轮处理 key == s43 的 lane
...
0x1220 v_cmp_eq_u32 vcc, 0, v11 ; 与活动掩码比较，全部处理完才退出
```

GCN 在 0xc48 把**所有** lane（包括 0x9d4 已关掉的）重新启用，用 -1 填充后做蝶形求最小值。翻译成 SPIR-V 后，0x9d8 的 `s_cbranch_execz` 被结构化为 `if (_868) { ... }`，关掉的 lane 根本不在这段控制流里；`subgroupShuffle` 读它们是未定义值。若读到的值小于所有活动 lane 的 key，`s43` 不等于任何活动 lane 的 key，没有 lane 取得进展，循环永不退出——与 GPU 超时一致。是否出现取决于 wave 内 `_868` 是否部分为假以及非活动寄存器里的残值，因此只在特定场景、运行一段时间后出现。

## 3. 修复

1. **惯用法整体翻译**（`Translator::TranslateWaveReduction`）：精确匹配 `s_orn2_saveexec_b64 SAVE, exec` → `v_cndmask_b32 V, identity, X, SAVE`（identity 必须是该运算的单位元）→ 5 步 `ds_swizzle`（bit mode，and=0x1f，or=0，xor 恰为 16/8/4/2/1 各一次）+ 同一 `v_min/max_{u,i}32`、`v_and/or/xor_b32` → 常量 lane 的 `v_readlane_b32`。填充、蝶形原样翻译（交换临时寄存器保持原定义），只替换结果：`V = ClusteredReduce(op, fill, 32)`，`readlane L = ClusteredReduce(op, (lane&32)==(L&32) ? fill : identity, 64)`。关掉的 lane 在 GCN 里只贡献单位元，在 SPIR-V 中不参与归约，结果相同。不匹配时回到原翻译。不按游戏或 hash 特判。
2. **CFG**：`s_orn2_saveexec_b64 X, exec`（exec 变全 1）关闭当前分歧作用域；此前它会被包进打开作用域时的 `if (exec)`，而那个条件在它之后本来就是全真，语义不变，只是不再把惯用法拆到三个块。
3. **sirit**（子模块，本地工作区修改，未提交）：新增 `OpGroupNonUniform{S,U}{Min,Max}` 与 `Bitwise{And,Or,Xor}`，带可选 ClusterSize。
4. **IR/后端**：`ClusteredUMin32` 等 7 个 opcode，发射 `GroupNonUniformArithmetic` + `GroupNonUniformClustered`；`Profile::supports_subgroup_clustered_reduce` 需要 compute 与 fragment 都支持 ARITHMETIC|CLUSTERED。
5. **Turnip 的 wave64 lowering 误用**（上游 #4995 移植引入）：Turnip 默认 `subgroupSize=128`，`LowerWave64BallotPass`（按 32 lane 写死）因此在 Turnip 上对 compute 生效，日志可见 `Lowering Ballot/ReadLane instruction for wave64`；但 compute 管线实际 `requiredSubgroupSize=64`。结果是 ballot 高 32 位被低 32 位复制、`ReadLane(≥32)` 读错 lane。Profile 的 `subgroup_size` 改为 compute 实际使用的大小（支持 64 时取 64）。
6. `ShaderBinaryVersion` 23→24；诊断开关 `debug.shadps4.wave_reduction=0`（桌面 `SHADPS4_WAVE_REDUCTION=0`）回到 shuffle 翻译，供同一构建 A/B，不能开启设备不支持的能力。

## 4. 验证与后续定位

**翻译结果。** 桌面（RX 7600M XT，AMD 驱动 2.0.395）开 `dump_shaders` 跑 MHW：`0xa38aae6c` 的新 SPIR-V 通过 `spirv-val --target-env vulkan1.3`，两半归约为 `subgroupClusteredMin(..., 64)`，原 shuffle 蝶形因无人使用被 DCE 删除；这一轮 dump 出的 51 个含该惯用法的 shader（48 CS + 3 FS）全部走了新翻译。AYN 在本轮中途断开（adb 只剩一台装有 `com.bachatas4.android` 的 Pocket DS，未动），GPU 探针 `tests/video_core/android_wave_reduction_probe.cpp`（同一可执行文件内新/旧翻译对照，9 种 lane 分布×3 组数据×min/or）已编译、未在设备上运行。

**桌面仍然挂起。** 修复后桌面 MHW 仍在开场动画后 GPU 超时（Windows LiveKernelEvent 141/TDR，进程随之退出）。CDL（`VK_LAYER_LUNARG_crash_diagnostic`）在 AMD Windows 驱动上没有 `VK_AMD_buffer_marker`，无法跟踪进度。为此新增默认关闭的诊断 `GpuBreadcrumbs`（`SHADPS4_GPU_BREADCRUMBS=1` / `debug.shadps4.gpu_breadcrumbs=1`，阈值 `*_MS`）：每个 guest dispatch 前后由 GPU 用 `vkCmdFillBuffer` 把序号写进 host-coherent 内存（前后全屏障，dispatch 被串行化），看门狗在两个值停止变化超过阈值时直接写 stderr（不走异步日志，TDR 杀进程前也能留下），给出正在执行的 dispatch、PM4 上下文（队列、包头 predicate 位、最近 `SET_PREDICATION`）和之前 48 个 dispatch。

三轮复现（wave3/6/9）结果一致：停在**计算队列（ACB）上的 `cs 0xcefc8276` 93×1×1**，不带谓词（`SET_PREDICATION` 出现上百万次但只用于 draw，dispatch 一个都没有 predicate 位），谓词假设排除；其中一次先停顿超过 1 秒后自行完成，下一次才 TDR。之前的序列：`dd70d846`(1) → `78a9797f`(2) → `4523916b`(512/512/2048) → `1f642b09`(indirect) → `2974ecda`(254) → `5436a929`(8) → `1a73aeba`(32) / `9e842f7b`(8) 交替多轮 → `9656f1f1`(indirect) → `cefc8276`。

- `cefc8276` 是网格跨步循环：`for (i = lid; i < end - start; i += 64)`，`start/end` 从缓冲区读出；若 `end < start`，无符号相减回绕成约 40 亿，循环约 6700 万次并持续写存储，等效挂起。翻译本身正确（`V_SAD_U32` 等核对过）。
- `9656f1f1` 在排好序的 key（`bits[31:18]`：1 位标志 + 13 位桶号）里找相邻 key 的边界，为每个 (标志, 桶) 写 start/end；只要参与边界检测的区间确实排好序，就不会出现 `end < start`。
- `5436a929` / `9e842f7b` 是 LDS 内的双调排序（`ds_read2st64_b64`/`ds_write2_b64` 等，64 位元素 = key + float 深度），`1a73aeba` 是全局 merge；比较-交换为纯 64 位 lane mask 逻辑，LDS 偏移换算（B64 8 字节、ST64 ×64）核对无误。

因此下一步怀疑点是"参与排序的元素数"与"边界检测使用的计数"不一致：MHW 用 `ds_append ... gds` 计数（GDS 计数器经 CP 传到内存），若计数的写回/复制与 dispatch 的顺序在模拟中不一致，边界检测会越过已排序区间读到垃圾。需要带 PM4 的 guest command trace（ACB 上的 DMA_DATA / RELEASE_MEM / WAIT_REG_MEM 与 GDS）确认，尚未做。

用户同时观察到桌面画面错乱、大量 texture re-upload（状态栏提示每帧至少 4 次）；桌面日志还有大量 "Geometry shader features unsupported, skipping"（on-chip GS / stream-out 未实现，相关 draw 被跳过）。均未处理。

**主菜单背景（09-26 晚，桌面 890M 核显，外接 7600M XT 当时不在）。** 标题/主菜单（开始游戏、PlayStation Store、选项、制作人员、支持信息）的文字与 ICEBORNE Logo 正常，但背景 3D 场景几乎全黑，只剩左侧一道蓝色光束与少量粒子。尚未抓帧定位：挂 RenderDoc layer（`VK_ADD_LAYER_PATH` + `VK_LOADER_LAYERS_ENABLE=VK_LAYER_RENDERDOC_Capture`）启动 MHW 时约 9 秒后进程静默退出（无系统崩溃记录，日志在异步写入中截断），不挂则能到菜单。下一步可试 RenderDoc + validation 同时加载（血源上此组合曾可用），或改用 PM4/guest command trace 找出背景 pass 的 shader，再用 `tools/gcn-disasm` 对照原始 GCN 与生成的 SPIR-V。

## 5. 未做 / 边界

- 修复后的 MHW 在 AYN 上尚未复测（设备断开）；桌面挂起的根因（排序计数/边界）未确认，见 §4。

- 其它整 wave 形式（`s_not_b64 exec, 0` 等启用全部 lane 后的跨 lane 读）未处理；dump 中只有 1 处，位于程序开头、无分歧控制流。
- 非 64 lane 子组（桌面 wave32 设备、不支持 required size 的设备）不启用新翻译，行为不变。
- KGSL `CP_RL_ERROR_DETAILS_0:0x10008e07` 的含义未查到源码佐证。
